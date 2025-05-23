// ------------------------------------------------------------------
//   vcf_giab.c
//   Copyright (C) 2019-2025 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#include "vcf_private.h"

STRl(datasets_snip, 32);
STRl(callsets_snip, 32);
STRl(platforms_snip, 32);

void vcf_giab_zip_initialize (void)
{
    seg_prepare_snip_special_other_char (VCF_SPECIAL_ARRAY_LEN_OF, _INFO_datasetnames,  datasets_snip,  ',');
    seg_prepare_snip_special_other_char (VCF_SPECIAL_ARRAY_LEN_OF, _INFO_callsetnames,  callsets_snip,  ',');
    seg_prepare_snip_special_other_char (VCF_SPECIAL_ARRAY_LEN_OF, _INFO_platformnames, platforms_snip, ',');
}

void vcf_giab_seg_initialize (VBlockVCFP vb)
{
    seg_mux_init (vb, FORMAT_IGT, VCF_SPECIAL_MUX_BY_IS_SAMPLE_0,  false, IGT);
    seg_mux_init (vb, FORMAT_IPS, VCF_SPECIAL_MUX_BY_IGT_PHASE, false, IPS);
}

//--------------------------------------------------------------------------------
// FORMAT/IPS: <ID=IPS,Number=1,Type=String,Description="Phase set for IGT">
//--------------------------------------------------------------------------------

// TO DO: store current_phase (only in vb->sample_i==0): in Seg, can store in last_txt, 
// in Piz, to avoid needing to deal with txt_data insertions, store in a "special_store" context-specific union of ol_nodes.
// Seg: if same as current_phase, seg SPECIAL_COPY_STORED. If not, check if same as pos,ref,alt and seg SPECIAL_IPS,
// with a parameter of whether ref and alt appear rev-comped.
// (no need for lookback as IPS is expected to always be either equal to pos/ref/alt, or to previous phase)
void vcf_seg_FORMAT_IPS (VBlockVCFP vb, ZipDataLineVCF *dl, ContextP ctx, STRp(ips))
{
    STRlast (igt, FORMAT_IGT);
    bool is_phased = (ctx_encountered (VB, FORMAT_IGT) && igt_len == 3 && igt[1] == '|');

    ContextP channel_ctx = 
        seg_mux_get_channel_ctx (VB, FORMAT_IPS, (MultiplexerP)&vb->mux_IPS, is_phased);

    seg_by_ctx (VB, STRa(ips), channel_ctx, ips_len); // note: for channel_i=0 - expeceted to be '.'

    seg_by_ctx (VB, STRa(vb->mux_IPS.snip), ctx, 0);
}

SPECIAL_RECONSTRUCTOR (vcf_piz_special_MUX_BY_IGT_PHASE)
{    
    STRlast (igt, FORMAT_IGT);
    bool is_phased = (ctx_encountered (VB, FORMAT_IGT) && igt_len == 3 && igt[1] == '|');

    HasNewValue ret = reconstruct_demultiplex (vb, ctx, STRa(snip), is_phased, new_value, reconstruct);

    return ret;
}    

//--------------------------------------------------------------------------------
// FORMAT/IGT: <ID=IGT,Number=1,Type=String,Description="Original input genotype">
//--------------------------------------------------------------------------------

void vcf_seg_FORMAT_IGT (VBlockVCFP vb, ContextP ctx, STRp(igt))
{
    seg_set_last_txt (VB, ctx, STRa(igt)); // consumed by vcf_seg_FORMAT_IPS

    if (!ctx_encountered (VB, FORMAT_GT) || vcf_num_samples != 3 || igt_len > 3 ||
        igt_len != CTX(FORMAT_GT)->gt.prev_ploidy * 2 - 1) {
        seg_by_ctx (VB, STRa(igt), ctx, igt_len);
        return;
    }

    ContextP channel_ctx = 
        seg_mux_get_channel_ctx (VB, FORMAT_IGT, (MultiplexerP)&vb->mux_IGT, (vb->sample_i > 0));

    if (vb->sample_i == 0) {
        Allele ht0 = CTX(FORMAT_GT)->gt.ht[0];
        Allele ht1 = CTX(FORMAT_GT)->gt.ht[1];
        
        // case monoploid: predicting GT=IGT
        if (str_is_1char (igt, ht0))
            seg_special0 (VB, VCF_SPECIAL_IGT, channel_ctx, igt_len);     

        // case: unphased diploid IGT: predicting GT=IGT, possibly flipping the order so that the smaller allele appears first
        else if (igt[1] == '/' && (   (ht0 <= ht1 && igt[0] == ht0 && igt[2] == ht1)
                                   || (ht0 >  ht1 && igt[0] == ht1 && igt[2] == ht0)))
            seg_special1 (VB, VCF_SPECIAL_IGT, '/', channel_ctx, igt_len);     

        // case phased diploid IGT: predicting IGT alleles = GT alleles
        else if (igt[1] == '|' && igt[0] == ht0 && igt[2] == ht1)
            seg_special1 (VB, VCF_SPECIAL_IGT, '|', channel_ctx, igt_len);     

        // case phased diploid IGT: predicting IGT alleles = GT alleles, but in reverse order
        else if (igt[1] == '|' && igt[0] == ht1 && igt[2] == ht0)
            seg_special1 (VB, VCF_SPECIAL_IGT, ':', channel_ctx, igt_len);     

        else
            goto fallback;
    }

    else fallback:
        seg_by_ctx (VB, STRa(igt), channel_ctx, igt_len); // note: for channel_i=1 - expeceted to be '.'

    seg_by_ctx (VB, STRa(vb->mux_IGT.snip), ctx, 0);
}

SPECIAL_RECONSTRUCTOR (vcf_piz_special_MUX_BY_IS_SAMPLE_0)
{    
    int channel_i = (vb->sample_i > 0);

    return reconstruct_demultiplex (vb, ctx, STRa(snip), channel_i, new_value, reconstruct);
}    

SPECIAL_RECONSTRUCTOR (vcf_piz_special_IGT)
{
    STRlast(gt, FORMAT_GT);

    if (gt_len == 1)
        RECONSTRUCT1 (*gt);

    else if ((snip[0] == '/' && gt[0] <= gt[2]) || snip[0] == '|') {
        RECONSTRUCT1 (gt[0]);
        RECONSTRUCT1 (snip[0]);
        RECONSTRUCT1 (gt[2]);
    }

    else { // flip allele order
        RECONSTRUCT1 (gt[2]);
        RECONSTRUCT1 (snip[0]=='/' ? '/' : '|');
        RECONSTRUCT1 (gt[0]);
    }

    return NO_NEW_VALUE;
}

//--------------------------------------------------------------------------------------------------------
// FORMAT/ADALL: <ID=ADALL,Number=R,Type=Integer,Description="Net allele depths across all datasets">
//--------------------------------------------------------------------------------------------------------

void vcf_seg_ADALL_items (VBlockVCFP vb, ContextP ctx, STRps(item), ContextP *item_ctxs, const int64_t *values)
{
    for (unsigned i=0; i < n_items; i++) 
        if (i==0 || i==1) {
            if (!vb->mux_ADALL[i].num_channels)
                seg_mux_init (vb, item_ctxs[i]->did_i, VCF_SPECIAL_MUX_BY_DOSAGE, false, ADALL[i]);
            
            vcf_seg_FORMAT_mux_by_dosage (vb, item_ctxs[i], STRi(item, i), &vb->mux_ADALL[i]);
        }
        else 
            seg_by_ctx (VB, STRi(item, i), item_ctxs[i], item_lens[i]);
}
