// ------------------------------------------------------------------
//   vcf_piz.c
//   Copyright (C) 2019-2024 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#include <math.h>
#include "vcf_private.h"
#include "zfile.h"
#include "txtfile.h"
#include "context.h"
#include "file.h"
#include "dict_id.h"
#include "reconstruct.h"
#include "piz.h"

void vcf_piz_finalize (void)
{
    vcf_header_finalize();
}

void vcf_piz_genozip_header (ConstSectionHeaderGenozipHeaderP header)
{
    if (VER(14)) 
        segconf.has[FORMAT_RGQ] = header->vcf.segconf_has_RGQ;

    if (VER(15)) {
        z_file->max_ploidy_for_mux    = header->vcf.max_ploidy_for_mux; // since 15.0.36
        segconf.GQ_method             = header->vcf.segconf_GQ_method;
        segconf.FMT_DP_method         = header->vcf.segconf_FMT_DP_method;
        segconf.MATEID_method         = header->vcf.segconf_MATEID_method;
        segconf.vcf_del_svlen_is_neg  = header->vcf.segconf_del_svlen_is_neg;
        segconf.wid_AC.width          = header->vcf.width.AC;
        segconf.wid_AF.width          = header->vcf.width.AF;
        segconf.wid_AN.width          = header->vcf.width.AN;
        segconf.wid_DP.width          = header->vcf.width.DP;
        segconf.wid_QD.width          = header->vcf.width.QD;
        segconf.wid_SF.width          = header->vcf.width.SF;
        segconf.wid_MLEAC.width       = header->vcf.width.MLEAC;
        segconf.wid_MLEAC.width       = header->vcf.width.MLEAC;
        segconf.wid_AS_SB_TABLE.width = header->vcf.width.AS_SB_TABLE;
        segconf.wid_ID.width          = header->vcf.width.ID;
    }
}

bool vcf_piz_init_vb (VBlockP vb_, ConstSectionHeaderVbHeaderP header)
{ 
    VBlockVCFP vb = (VBlockVCFP)vb_;

    vb->recon_size = BGEN32 (header->recon_size); 
    
    // HT_n_lines: number of lines that are included in HT_MATRIX. Since 15.0.48, we don't
    // include lines with no FORMAT/GT or lines copied in vcf_seg_sv_SAMPLES. Prior to that, 
    // all lines were always included.
    decl_ctx (FORMAT_GT_HT); 
    if (VER(15)) {
        ctx->HT_n_lines = BGEN32 (header->vcf_HT_n_lines); // since 15.0.48.
        
        // case: vcf_zip_set_vb_header_specific set header->vcf_HT_n_lines, but truly no lines have GT
        if (ctx->HT_n_lines == 0xffffffff) 
            ctx->HT_n_lines = 0;
        
        // case: file is 15.0.0 to 15.0.46 where this field was not set
        else if (ctx->HT_n_lines == 0)  
            goto fallback; 
    }
    
    else fallback: {
        // in older files, we always included all lines in the HT matrix if any line was included
        if (section_get_section (vb->vblock_i, SEC_LOCAL, _FORMAT_PBWT_RUNS)) 
            ctx->HT_n_lines = vb->lines.len32; 
    }

    CTX(INFO_END)->last_end_line_i = LAST_LINE_I_INIT;

    return true; // all good*
}

void vcf_piz_recon_init (VBlockP vb)
{
}

// returns true if section is to be skipped reading / uncompressing
IS_SKIP (vcf_piz_is_skip_section)
{
    if (flag.drop_genotypes && // note: if all samples are filtered out with --samples then flag.drop_genotypes=true (set in vcf_samples_analyze_field_name_line)
        (dict_id.num == _VCF_FORMAT || 
         (dict_id.num == _VCF_SAMPLES && !segconf.has[FORMAT_RGQ]) || // note: if has[RGQ], vcf_piz_special_main_REFALT peeks SAMPLES so we need it 
         dict_id_is_vcf_format_sf (dict_id)))
        return true;

    if (flag.gt_only && IS_DICTED_SEC (st) && dict_id_is_vcf_format_sf (dict_id) 
        && dict_id.num != _FORMAT_GT
        && dict_id.num != _FORMAT_GT_HT
        && dict_id.num != _FORMAT_PBWT_RUNS
        && dict_id.num != _FORMAT_PBWT_FGRC)
        return true;

    // if --count, we only need TOPLEVEL and the fields needed for the available filters (--regions)
    if (flag.count && IS_DICTED_SEC (st) &&
        (     dict_id.num != _VCF_TOPLEVEL && 
              dict_id.num != _VCF_CHROM    && // easier to always have CHROM
             (dict_id.num != _VCF_POS || !flag.regions))) return true;

    return false;
}

// insert a field after following fields have already been reconstructed
void vcf_piz_insert_field (VBlockVCFP vb, ContextP ctx, STRp(value), int chars_reserved)
{
    if (!IS_RECON_INSERTION(ctx)) return;
    
    char *addr = last_txtx (VB, ctx);
    int move_by = (int)value_len - chars_reserved;

    // actually insert or remove space in txt_data if different than what was reserved
    if (move_by) {
        if (move_by > 0) memmove (addr + move_by, addr, BAFTtxt - addr);             // make room if chars_reserved is not enough
        else             memmove (addr, addr - move_by, BAFTtxt - (addr - move_by)); // shrink room added by vcf_piz_defer_to_after_samples if it was too much

        Ltxt += move_by;

        // note: keep txt_data.len 64b to detect bugs
        ASSPIZ (vb->txt_data.len <= vb->txt_data.size, "txt_data overflow: len=%"PRIu64" > size=%"PRIu64". vb->txt_data dumped to %s.gz", 
                vb->txt_data.len, (uint64_t)vb->txt_data.size, txtfile_dump_vb (VB, z_name));
        
        // adjust last_txt of other INFO contexts that might need insertion (and hence last_txt)
        if (dict_id_is_vcf_info_sf (ctx->dict_id)) {
            Did dids[] = { INFO_QD, INFO_SF, INFO_DP, INFO_AN, INFO_AS_SB_TABLE };
            uint32_t last_txt_index = ctx->last_txt.index;

            bool found_me = false;
            for (int i=0; i < ARRAY_LEN(dids); i++) {
                if (CTX(dids[i])->last_txt.index > last_txt_index)
                    CTX(dids[i])->last_txt.index += move_by;
                
                if (dids[i] == ctx->did_i) found_me = true;
            }

            ASSPIZ (found_me, "Missing ctx=%s in dids[]", ctx->tag_name);   
        }
        
        // adjust lookback addresses that might be affected by this insertion
        vcf_piz_ps_pid_lookback_shift (VB, addr, move_by);
    }
    
    memcpy (addr, value, value_len); // copy
    ctx->last_txt.len = value_len;
}

bool vcf_piz_line_has_RGQ (VBlockVCFP vb)
{
    decl_ctx (FORMAT_RGQ);

    if (ctx->line_has_RGQ == unknown)
        ctx->line_has_RGQ = segconf.has[FORMAT_RGQ] && 
                            container_peek_has_item (VB, CTX(VCF_SAMPLES), (DictId)_FORMAT_RGQ, false); // note: segconf.has[FORMAT_RGQ] is never set in PIZ prior to v14

    return ctx->line_has_RGQ;
}

SPECIAL_RECONSTRUCTOR (vcf_piz_special_MUX_BY_HAS_RGQ)
{
    return reconstruct_demultiplex (vb, ctx, STRa(snip), vcf_piz_line_has_RGQ (VB_VCF), new_value, reconstruct);
}
// filter is called before reconstruction of a repeat or an item, and returns false if item should 
// not be reconstructed. contexts are not consumed.
CONTAINER_FILTER_FUNC (vcf_piz_filter)
{
    uint64_t dnum = (item >= 0) ? con->items[item].dict_id.num : 0;

    switch (dict_id.num) {

        case _VCF_TOPLEVEL:
            // After reconstructing POS (next field is ID) - save POS.last_value, as it might be changed by an INFO/END. 
            if (dnum == _VCF_ID) {
                CTX(VCF_POS)->pos_last_value = CTX(VCF_POS)->last_value.i; // consumed by regions_is_site_included
                break;
            }

            else if (dnum == _VCF_QUAL) {
                if (!VER(15)) vcf_piz_refalt_parse (VB_VCF); // starting 14.0.12, called from vcf_piz_con_item_cb (no harm if called twice)
            }

            // --drop-genotypes: remove the two tabs at the end of the line
            else if (dnum == _VCF_EOL) { 
                if (flag.drop_genotypes) Ltxt -= 2;
            }

            // --drop-genotypes: drop SAMPLES (might be loaded if has[RGQ], as needed for vcf_piz_special_main_REFALT)
            else if (dnum == _VCF_SAMPLES) {
                if (flag.drop_genotypes) return false;
            }

            // DVCF-related fields that exist if files compressed 12.0.0 - 15.0.41
            else if (dnum == _VCF_oSTATUS || dnum == _VCF_COORDS) 
                return false; // filter out entirely without consuming 
        
            break;
            
        case _VCF_SAMPLES:
        case _VCF_SAMPLES_0: // "no mate" channel of SAMPLES multiplexor
            // Set sample_i before reconstructing each sample
            if (item == -1) 
                vb->sample_i = rep;

            // --GT-only - don't reconstruct non-GT sample subfields
            else if (flag.gt_only && dnum != _FORMAT_GT) 
                return false; 

            break;

        default: break;
    }

    return true;    

}

// ------------------------------------
// callback called after reconstruction
// ------------------------------------

static void inline vcf_piz_SAMPLES_subset_samples (VBlockVCFP vb, unsigned rep, unsigned num_reps, int32_t recon_len)
{
    if (!samples_am_i_included (rep))
        Ltxt -= recon_len + (rep == num_reps - 1); // if last sample, we also remove the preceeding \t (recon_len includes the sample's separator \t, except for the last sample that doesn't have a separator)
}

CONTAINER_ITEM_CALLBACK (vcf_piz_con_item_cb)
{
    switch (con_item->dict_id.num) {
        // IMPORTANT: when adding a "case", also update set CI1_ITEM_CB in vcf_seg_FORMAT
        
        case _FORMAT_DP:
            if (ctx_has_value (vb, FORMAT_DP)) { // not '.' or missing
                if (CTX(INFO_DP)->dp.by_format_dp) 
                    CTX(INFO_DP)->dp.sum_format_dp += CTX(FORMAT_DP)->last_value.i;
            
                // add up DP's of samples with GT!=0/0, for consumption by INFO/QD predictor
                QdPredType pd = CTX(INFO_QD)->qd.pred_type;
                if (pd == QD_PRED_SUM_DP || pd == QD_PRED_SUM_DP_P001 || pd == QD_PRED_SUM_DP_M001)
                    vcf_piz_sum_DP_for_QD (vb, STRa(recon));
            }
            break;
            
        case _FORMAT_SB:
            vcf_piz_sum_SB_for_AS_SB_TABLE (vb, STRa(recon));
            break;
            
        case _FORMAT_PS: // since v13: PS has item_cb
            vcf_piz_ps_pid_lookback_insert (vb, FORMAT_PS, STRa(recon));
            break;

        case _FORMAT_PID: // since v13: PID has item_cb
            vcf_piz_ps_pid_lookback_insert (vb, FORMAT_PID, STRa(recon));
            break;

        // note: for _FORMAT_IPS we insert in vcf_piz_special_MUX_BY_IGT_PHASE
        
        case _VCF_REFALT: // files compress starting v14.0.12
            vcf_piz_refalt_parse (VB_VCF);
            break;

        default:
            ABORT_PIZ ("vcf_piz_con_item_cb doesn't know how to handle dict_id=%s. %s", 
                       dis_dict_id (con_item->dict_id).s, genozip_update_msg());
    }
}

CONTAINER_CALLBACK (vcf_piz_container_cb)
{
    #define have_INFO_SF (CTX(INFO_SF)->deferred_snip.len > 0)

    // case: we have an INFO/SF field and we reconstructed and we reconstructed a repeat (i.e. one ht) GT field of a sample 
    if (dict_id.num == _FORMAT_GT) {
        CTX(INFO_AN)->an.count_ht += (*recon != '.'); // used for reconstructing INFO/AN

        // after first HT is reconstructed, re-write the predictable phase (note: predictable phases are only used for ploidy=2)
        if (rep==0 && con->repsep[0] == '&') 
            vcf_piz_FORMAT_GT_rewrite_predicted_phase (vb, STRa (recon));
   
        if (rep==1 && vb->flags.vcf.use_null_DP_method && con->repeats==2 && 
            con->items[0].separator[1] != CI1_ITEM_PRIVATE/*override flag*/) 
            vcf_piz_GT_cb_null_GT_if_null_DP (vb, recon);

        if (have_INFO_SF) 
            vcf_piz_GT_cb_calc_INFO_SF (VB_VCF, rep, STRa(recon));
    }

    else if (is_top_level) {
        // insert INFO fields who's value is determined by the sample fields that we just finished reconstructing

        if (ctx_encountered_in_line (vb, INFO_AN))
            vcf_piz_insert_INFO_AN (VB_VCF);

        // note: DP must be inserted before vcf_piz_insert_INFO_QD, because QD needs DP.last_value
        if (CTX(INFO_DP)->dp.by_format_dp)
            vcf_piz_insert_INFO_DP (VB_VCF);

        if (have_INFO_SF)  
            vcf_piz_insert_INFO_SF (VB_VCF); // cleans up allocations - call even if line will be dropped due oSTATUS

        if (CTX(INFO_QD)->qd.pred_type) 
            vcf_piz_insert_INFO_QD (VB_VCF);

        if (CTX(INFO_AS_SB_TABLE)) 
            vcf_piz_insert_INFO_AS_SB_TABLE (VB_VCF);

        if (flag.snps_only && !vcf_refalt_piz_is_variant_snp (VB_VCF))
            vb->drop_curr_line = "snps_only";

        if (flag.indels_only && !vcf_refalt_piz_is_variant_indel (VB_VCF))
            vb->drop_curr_line = "indels_only";
    }

    else if (dict_id.num == _VCF_SAMPLES) {
        ctx_set_last_value (vb, CTX(VCF_LOOKBACK), (ValueType){ .i = con->repeats });

        if (flag.samples) 
            vcf_piz_SAMPLES_subset_samples (VB_VCF, rep, con->repeats, recon_len);
    }
}
