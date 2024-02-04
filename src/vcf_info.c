// ------------------------------------------------------------------
//   vcf_info.c
//   Copyright (C) 2019-2024 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#include "vcf_private.h"
#include "piz.h"
#include "optimize.h"
#include "file.h"
#include "dict_id.h"
#include "codec.h"
#include "reconstruct.h"
#include "stats.h"

#define info_items CTX(VCF_INFO)->info_items

// called after reading VCF header, before segconf
void vcf_info_zip_initialize (void) 
{
    vcf_dbsnp_zip_initialize(); // called even if not in VCF header, because can be discovered in segconf too
    vcf_gatk_zip_initialize();
    if (segconf.vcf_is_vagrent)    vcf_vagrent_zip_initialize();
    if (segconf.vcf_is_mastermind) vcf_mastermind_zip_initialize();
    if (segconf.vcf_is_vep)        vcf_vep_zip_initialize();
    if (segconf.vcf_illum_gtyping) vcf_illum_gtyping_zip_initialize();
    if (segconf.vcf_is_platypus)   vcf_platypus_zip_initialize();
}

void vcf_info_seg_initialize (VBlockVCFP vb) 
{
    #define T(cond, did_i) ((cond) ? (did_i) : DID_NONE)

    ctx_set_store (VB, STORE_INT, INFO_AN, INFO_AC, INFO_ADP, INFO_DP, INFO_MLEAC, 
                   INFO_DP4_RF, INFO_DP4_RR, INFO_DP4_AF, INFO_DP4_AR, 
                   INFO_AC_Hom, INFO_AC_Het, INFO_AC_Hemi,
                   DID_EOL);

    CTX(INFO_AF)->flags.store = STORE_FLOAT;
    // xxx (is this really needed for --indels-only?) CTX(INFO_SVTYPE)-> flags.store = STORE_INDEX; // since v13 - consumed by vcf_refalt_piz_is_variant_indel

    CTX(INFO_SF)->sf.SF_by_GT = unknown;
    
    ctx_set_dyn_int (VB, INFO_SVLEN, INFO_DP4_RF, INFO_DP4_AF,
                     T(segconf.INFO_DP_method == INFO_DP_DEFAULT, INFO_DP),
                     DID_EOL);

    ctx_consolidate_stats (VB, INFO_RAW_MQandDP, INFO_RAW_MQandDP_MQ, INFO_RAW_MQandDP_DP, DID_EOL);
    
    if (segconf.has[INFO_CLNHGVS]) vcf_seg_hgvs_consolidate_stats (vb, INFO_CLNHGVS);
    if (segconf.has[INFO_HGVSG])   vcf_seg_hgvs_consolidate_stats (vb, INFO_HGVSG);
    if (segconf.has[INFO_ANN])     vcf_seg_hgvs_consolidate_stats (vb, INFO_ANN); // subfield HGVS_c

    #undef T
}

//--------
// INFO/DP
// -------

static void vcf_seg_INFO_DP_by_FORMAT_DP (VBlockP vb); // forward

// return true if caller still needs to seg 
static void vcf_seg_INFO_DP (VBlockVCFP vb, ContextP ctx, STRp(dp_str))
{
    SEGCONF_RECORD_WIDTH (DP, dp_str_len);

    // used in: vcf_seg_one_sample (for 1-sample files), vcf_seg_INFO_DP_by_FORMAT_DP (multi sample files)
    int64_t dp;
    bool has_value = str_get_int (STRa(dp_str), &dp);

    // also tried delta vs DP4, but it made it worse
    ContextP ctx_basecounts;
    if (ctx_has_value_in_line (vb, _INFO_BaseCounts, &ctx_basecounts)) 
        seg_delta_vs_other (VB, ctx, ctx_basecounts, STRa(dp_str));

    else if (!has_value || segconf.INFO_DP_method == INFO_DP_DEFAULT) 
        seg_integer_or_not (VB, ctx, STRa(dp_str), dp_str_len);

    // defer segging to vcf_seg_INFO_DP_by_FORMAT_DP called after samples are done
    else { // BY_FORMAT_DP
        ctx->dp.by_format_dp = true;
        vb_add_to_deferred_q (VB, ctx, vcf_seg_INFO_DP_by_FORMAT_DP, vb->idx_DP);
    }

    if (has_value) 
        ctx_set_last_value (VB, ctx, dp);
}

// used for multi-sample VCFs, IF FORMAT/DP is segged as a simple integer
static void vcf_seg_INFO_DP_by_FORMAT_DP (VBlockP vb)
{
    decl_ctx (INFO_DP);

    int value_len = str_int_len (ctx->last_value.i);

    // note: INFO/DP >= sum(FORMAT/DP) as the per-sample value is filtered, see: https://gatk.broadinstitute.org/hc/en-us/articles/360036891012-DepthPerSampleHC
    // note: up to 15.0.35, we had the value_len before the \t
    SNIPi3 (SNIP_SPECIAL, VCF_SPECIAL_DP_by_DP, '\t', ctx->last_value.i - ctx->dp.sum_format_dp);
    seg_by_ctx (VB, STRa(snip), ctx, value_len); 
}

// initialize reconstructing INFO/DP by sum(FORMAT/DP) - save space in txt_data, and initialize delta
SPECIAL_RECONSTRUCTOR (vcf_piz_special_DP_by_DP)
{
    str_split (snip, snip_len, 2, '\t', item, 2); // note: up to 15.0.35, items[0] was the length of the integer to be inserted. we ignore it now.

    if (!flag.drop_genotypes && !flag.gt_only && !flag.samples) {
        ctx->dp.sum_format_dp = atoi (items[1]); // initialize with delta
        ctx->dp.by_format_dp = true;             // DP needs to be inserted by vcf_piz_insert_INFO_DP
        
        vcf_piz_defer_to_after_samples (DP);

        return NO_NEW_VALUE; // we don't have the value yet - it will be set in vcf_piz_insert_INFO_DP
    }
    else {
        if (reconstruct) 
            RECONSTRUCT ("-1", 2); // bc we can't calculate INFO/DP in these cases bc we need FORMAT/DP of all samples
    
        new_value->i = -1;
        return HAS_NEW_VALUE;
    }
}

// finalize reconstructing INFO/DP by sum(FORMAT/DP) - called after reconstructing all samples
void vcf_piz_insert_INFO_DP (VBlockVCFP vb)
{
    decl_ctx (INFO_DP);
    
    if (IS_RECON_INSERTION(ctx)) {
        STRl(info_dp,16);
        info_dp_len = str_int_ex (ctx->dp.sum_format_dp, info_dp, false);

        vcf_piz_insert_field (vb, ctx, STRa(info_dp), segconf.wid_DP.width);
    }

    ctx_set_last_value (VB, ctx, (int64_t)ctx->dp.sum_format_dp); // consumed by eg vcf_piz_insert_INFO_QD
}

// used starting v13.0.5, replaced in v14 with a new vcf_piz_special_DP_by_DP
SPECIAL_RECONSTRUCTOR (vcf_piz_special_DP_by_DP_v13)
{
    str_split (snip, snip_len, 2, '\t', item, 2);

    int num_dps_this_line   = atoi (items[0]);
    int64_t value_minus_sum = atoi (items[1]);

    ContextP format_dp_ctx = CTX(FORMAT_DP);

    int64_t sum=0;

    ASSPIZ (format_dp_ctx->next_local + num_dps_this_line <= format_dp_ctx->local.len, "Not enough data in FORMAT/DP.local to reconstructed INFO/DP: next_local=%u local.len=%u but needed num_dps_this_line=%u",
            format_dp_ctx->next_local, format_dp_ctx->local.len32, num_dps_this_line);
            
    uint32_t invalid = lt_desc[format_dp_ctx->ltype].max_int; // represents '.'
    for (int i=0; i < num_dps_this_line; i++) {
        uint32_t format_dp = (format_dp_ctx->ltype == LT_UINT8)  ? (uint32_t)*B8 ( format_dp_ctx->local, format_dp_ctx->next_local + i)
                           : (format_dp_ctx->ltype == LT_UINT16) ? (uint32_t)*B16 (format_dp_ctx->local, format_dp_ctx->next_local + i)
                           : /* LT_UINT32 */                       (uint32_t)*B32 (format_dp_ctx->local, format_dp_ctx->next_local + i);

        if (format_dp != invalid) sum += format_dp; 
    }

    new_value->i = value_minus_sum + sum;

    RECONSTRUCT_INT (new_value->i);

    return HAS_NEW_VALUE;
}

static bool vcf_seg_INFO_DP4_delta (VBlockP vb, ContextP ctx, STRp(value), uint32_t unused_rep)
{
    if (ctx_encountered_in_line (vb, (ctx-1)->did_i)) {
        seg_delta_vs_other (VB, ctx, ctx-1, STRa(value));
        return true; // segged successfully
    }
    else
        return false;
}

// <ID=DP4,Number=4,Type=Integer,Description="# high-quality ref-forward bases, ref-reverse, alt-forward and alt-reverse bases">
// Expecting first two values to be roughly similar, as the two last bases roughly similar
static void vcf_seg_INFO_DP4 (VBlockVCFP vb, ContextP ctx, STRp(dp4))
{
    static const MediumContainer container_DP4 = {
        .repeats      = 1, 
        .nitems_lo    = 4, 
        .items        = { { .dict_id.num = _INFO_DP4_RF, .separator = "," }, 
                          { .dict_id.num = _INFO_DP4_RR, .separator = "," }, 
                          { .dict_id.num = _INFO_DP4_AF, .separator = "," }, 
                          { .dict_id.num = _INFO_DP4_AR                   } } };

    SegCallback callbacks[4] = { 0, vcf_seg_INFO_DP4_delta, 0, vcf_seg_INFO_DP4_delta }; 

    seg_struct (VB, ctx, container_DP4, STRa(dp4), callbacks, dp4_len, true);
}

// -------
// INFO/AA
// -------

// return 0 if the allele equals main REF, the alt number if it equals one of the ALTs, or -1 if none or -2 if '.'
static int vcf_INFO_ALLELE_get_allele (VBlockVCFP vb, STRp (value))
{
    // check for '.'
    if (IS_PERIOD (value)) return -2;

    // check if its equal main REF (which can by REF or oREF)
    if (str_issame (value, vb->main_ref)) return 0;

    // check if its equal one of the ALTs
    str_split (vb->main_alt, vb->main_alt_len, 0, ',', alt, false);

    for (int alt_i=0; alt_i < n_alts; alt_i++) 
        if (str_issame_(STRa(value), STRi(alt, alt_i)))
            return alt_i + 1;

    // case: not REF or any of the ALTs
    return -1;
}

// checks if value is identifcal to the REF or one of the ALT alleles, and if so segs a SPECIAL snip
// Used for INFO/AA, INFO/CSQ/Allele, INFO/ANN/Allele. Any field using this should have the VCF2VCF_ALLELE translator set in vcf_lo_luft_trans_id.
bool vcf_seg_INFO_allele (VBlockP vb_, ContextP ctx, STRp(value), uint32_t repeat)  
{
    VBlockVCFP vb = (VBlockVCFP)vb_;
    
    int allele = vcf_INFO_ALLELE_get_allele (vb, STRa(value));

    // case: this is one of the alleles in REF/ALT - our special alg will just copy from that allele
    if (allele >= 0) {
        char snip[] = { SNIP_SPECIAL, VCF_SPECIAL_ALLELE, '0' + vb->line_coords, '0' + allele /* ASCII 48...147 */ };
        seg_by_ctx (VB, snip, sizeof (snip), ctx, value_len);
    }

    // case: a unique allele and no xstrand - we just leave it as-is
    else
        seg_by_ctx (VB, STRa(value), ctx, value_len); 

    // validate that the primary value (as received from caller or lifted back) can be luft-translated 
    // note: for INFO/AA, but not for INFO/CSQ/Allele and INFO/ANN/Allele, this is done already in vcf_seg_info_one_subfield (no harm in redoing)
    if (vb->line_coords == DC_PRIMARY && needs_translation (ctx)) {
        if (allele != -1) 
            ctx->line_is_luft_trans = true; // assign translator to this item in the container, to be activated with --luft
        else 
            REJECT_SUBFIELD (LO_INFO, ctx, ".\tCannot cross-render INFO subfield %s: \"%.*s\"", ctx->tag_name, value_len, value);            
    }

    return true; // segged successfully
}

SPECIAL_RECONSTRUCTOR (vcf_piz_special_ALLELE)
{
    Coords seg_line_coord = snip[0] - '0';
    int allele = snip[1] - '0';
    LiftOverStatus ostatus = last_ostatus;

    if (LO_IS_OK_SWITCH (ostatus) && seg_line_coord != VB_VCF->vb_coords) {
        ASSPIZ (allele >= 0 && allele <= 1, "unexpected allele=%d with REF<>ALT switch", allele);
        allele = 1 - allele;
    }

    ContextP refalt_ctx = VB_VCF->vb_coords == DC_PRIMARY ? CTX (VCF_REFALT) : CTX (VCF_oREFALT); 

    STRlast (refalt, refalt_ctx->did_i);

    if (!refalt_len) goto done; // variant is single coordinate in the other coordinate

    char *tab = memchr (refalt, '\t', refalt_len);
    ASSPIZ (tab, "Invalid refalt: \"%.*s\"", MIN_(refalt_len, 100), refalt);

    // case: the allele is REF
    if (allele == 0)
        RECONSTRUCT (refalt, tab - refalt);
    
    // case: the allele is one of the alts
    else {
        str_split (tab+1, &refalt[refalt_len] - (tab+1), 0, ',', alt, false);
        RECONSTRUCT (alts[allele-1], alt_lens[allele-1]);
    }

done:
    return NO_NEW_VALUE;
}

// translator only validates - as vcf_piz_special_ALLELE copies verbatim (revcomp, if xstrand, is already done in REF/ALT)
TRANSLATOR_FUNC (vcf_piz_luft_ALLELE)
{
    VBlockVCFP vcf_vb = VB_VCF;

    // reject if LO_OK_REF_NEW_SNP and value is equal to REF
    if (validate_only && last_ostatus == LO_OK_REF_NEW_SNP && str_issame (recon, vcf_vb->main_ref)) 
        return false;

    // reject if the value is not equal to REF, any ALT or '.'
    if (validate_only && vcf_INFO_ALLELE_get_allele (vcf_vb, STRa(recon)) == -1) return false;

    return true;
}

// ------------------------
// INFO/SVLEN & INFO/REFLEN
// ------------------------

static inline void vcf_seg_INFO_SVLEN (VBlockVCFP vb, ContextP ctx, STRp(svlen_str))
{
    int64_t svlen;
    if (!str_get_int (STRa(svlen_str), &svlen)) 
        seg_by_ctx (VB, STRa(svlen_str), ctx, svlen_str_len);

    // if SVLEN is negative, it is expected to be minus the delta between END and POS
    else if (-svlen == CTX(VCF_POS)->last_delta) // INFO_END is an alias of POS - so the last delta would be between END and POS
        seg_by_ctx (VB, ((char[]){ SNIP_SPECIAL, VCF_SPECIAL_SVLEN }), 2, ctx, svlen_str_len);

    // for left-anchored deletions or insertions, SVLEN might be the length of the payload
    else if (svlen == MAX_(vb->main_alt_len, vb->main_ref_len) - 1)
        seg_by_ctx (VB, ((char[]){ SNIP_SPECIAL, VCF_SPECIAL_SVLEN, '1' }), 3, ctx, svlen_str_len);

    else
        seg_integer_or_not (VB, ctx, STRa(svlen_str), svlen_str_len);
}

static inline void vcf_seg_INFO_REFLEN (VBlockVCFP vb, ContextP ctx, STRp(reflen_str)) // note: ctx is INFO/END *not* POS (despite being an alias)
{
    int64_t reflen;

    if (CTX(VCF_POS)->last_delta && str_get_int (STRa(reflen_str), &reflen) && reflen == CTX(VCF_POS)->last_delta)
        seg_by_ctx (VB, (char[]){ SNIP_SPECIAL, VCF_SPECIAL_SVLEN, '2' }, 3, ctx, reflen_str_len);
    else
        seg_by_ctx (VB, STRa(reflen_str), ctx, reflen_str_len);
}


// the case where SVLEN is minus the delta between END and POS
SPECIAL_RECONSTRUCTOR_DT (vcf_piz_special_SVLEN)
{
    VBlockVCFP vb = (VBlockVCFP)vb_;
    
    if (!snip_len) 
        new_value->i = -CTX(VCF_POS)->last_delta; // END is a alias of POS - they share the same data stream - so last_delta would be the delta between END and POS

    else if (*snip == '2') // introduced 15.0.13
        new_value->i = CTX(VCF_POS)->last_delta;

    else if (*snip == '1') // introduced 15.0.13
        new_value->i = MAX_(vb->main_alt_len, vb->main_ref_len) - 1;

    else
        ABORT_PIZ ("unrecognized snip '%c'(%u). %s", *snip, (uint8_t)*snip, genozip_update_msg());

    if (reconstruct) RECONSTRUCT_INT (new_value->i);

    return HAS_NEW_VALUE;
}

// -----------
// INFO/SVTYPE
// -----------

static inline bool vcf_seg_SVTYPE (VBlockVCFP vb, ContextP ctx, STRp(svtype))
{
    uint32_t alt_len = vb->main_alt_len;
    uint32_t ref_len = vb->main_ref_len;
    rom alt = vb->main_alt;

    // TODO: need careful testing to see this is handled correctly in case of a REF/ALT switch
    if (z_is_dvcf) goto fallback;

    // prediction: ALT has a '[' or a ']', then SVTYPE is "BND"
    else if (memchr (alt, '[', alt_len) || memchr (alt, ']', alt_len)) 
        { if (memcmp (svtype, "BND", 3)) goto fallback; }

    // prediction: if ALT starts/ends with <>, then its the same as SVTYPE except <>
    else if (alt[0] == '<' && alt[alt_len-1] == '>') 
        { if (!str_issame_(STRa(svtype), alt+1, alt_len-2)) goto fallback; }

    // prediction: if ref_len>1 and alt_len=1, then SVTYPE is <DEL>
    else if (ref_len > 1 && alt_len == 1) 
        { if (memcmp (svtype, "DEL", 3)) goto fallback; }

    // prediction: if ref_len=1 and alt_len>1, then SVTYPE is <INS>
    else if (ref_len == 1 && alt_len > 1) 
        { if (memcmp (svtype, "INS", 3)) goto fallback; }

    else fallback:
        return true;

    // prediction succeeded
    seg_by_ctx (VB, (char[]){ SNIP_SPECIAL, VCF_SPECIAL_SVTYPE }, 2, ctx, svtype_len);
    return false;  // no need for fallback
}

SPECIAL_RECONSTRUCTOR (vcf_piz_special_SVTYPE)
{    
    rom alt = VB_VCF->main_alt;
    uint32_t alt_len = VB_VCF->main_alt_len;
    uint32_t ref_len = VB_VCF->main_ref_len;

    // prediction: ALT has a '[' or a ']', then SVTYPE is "BND"
    if (memchr (alt, '[', alt_len) || memchr (alt, ']', alt_len)) 
        RECONSTRUCT ("BND", 3);
    
    // prediction: if ALT has 2 characters more than SVTYPE, starting/end with <>, then its the same as SVTYPE except <>
    else if (alt[0] == '<' && alt[alt_len-1] == '>') 
        RECONSTRUCT (alt+1, alt_len-2);

    // prediction: if ref_len>1 and alt_len=1, then SVTYPE is <DEL>
    else if (ref_len > 1 && alt_len == 1) 
        RECONSTRUCT ("DEL", 3);

    // prediction: if ref_len=1 and alt_len>1, then SVTYPE is <INS>
    else if (ref_len == 1 && alt_len > 1) 
        RECONSTRUCT ("INS", 3);

    else
        ABORT_PIZ ("failed to reconstruct SVTYPE: ref_len=%u alt=\"%.*s\"", ref_len, STRf(alt));

    return NO_NEW_VALUE;
}    

// --------------
// INFO container
// --------------

// for dual coordinate files (Primary, Luft and --chain) - add DVCF depending on ostatus (run after
// all INFO and FORMAT fields, so ostatus is final)
static void vcf_seg_info_add_DVCF_to_InfoItems (VBlockVCFP vb)
{
    // case: Dual coordinates file line has no PRIM, Lrej or Prej - this can happen if variants were added to the file,
    // for example, as a result of a "bcftools merge" with a non-DVCF file
    bool added_variant = false;
    if (!ctx_encountered_in_line (VB, INFO_LUFT) && // note: no need to check PRIM because LUFT and PRIM always appear together
        !ctx_encountered_in_line (VB, INFO_LREJ) &&
        !ctx_encountered_in_line (VB, INFO_PREJ)) {
        vcf_lo_seg_rollback_and_reject (vb, LO_ADDED_VARIANT, NULL); // note: we don't report this reject because it doesn't happen during --chain
        added_variant = true; // we added a REJX field in a variant that will be reconstructed in the current coordintes
    }

    // case: line originally had LIFTOVER or LIFTBACK. These can be fields from the txt files, or created by --chain
    bool has_luft    = ctx_encountered_in_line (VB, INFO_LUFT);
    bool has_prim    = ctx_encountered_in_line (VB, INFO_PRIM);
    bool has_lrej    = ctx_encountered_in_line (VB, INFO_LREJ);
    bool has_prej    = ctx_encountered_in_line (VB, INFO_PREJ);
    bool rolled_back = LO_IS_REJECTED (last_ostatus) && (has_luft || has_prim); // rejected in the Seg process
           
    // make sure we have either both LIFT/PRIM or both Lrej/Prej subfields in Primary and Luft
    ASSVCF ((has_luft && has_prim) || (has_lrej && has_prej), "%s", 
            vb->line_coords==DC_PRIMARY ? "Missing INFO/LUFT or INFO/Lrej subfield" : "Missing INFO/PRIM or INFO/Prej subfield");

    // case: --chain and INFO is '.' - remove the '.' as we are adding a DVCF field
    if (info_items.len == 1 && B1ST (InfoItem, info_items)->name_len == 1 && *B1ST (InfoItem, info_items)->name == '.') {
        info_items.len = 0;
        vb->recon_size--;
        vb->recon_size_luft--;
    }

    // dual coordinate line - we seg both options and vcf_piz_filter will decide which to render
    if (LO_IS_OK (last_ostatus)) {

        BNXT (InfoItem, info_items) = (InfoItem) { 
            .name      = INFO_LUFT_NAME"=", 
            .name_len  = INFO_DVCF_LEN + 1, // +1 for the '='
            .ctx       = CTX (INFO_LUFT),
            .value     = "" // non-zero means value exists
        };  
        
        BNXT (InfoItem, info_items) = (InfoItem) { 
            .name      = INFO_PRIM_NAME"=", 
            .name_len  = INFO_DVCF_LEN + 1, // +1 for the '='
            .ctx       = CTX (INFO_PRIM),
            .value     = "" // non-zero means value exists
        };  

        // case: --chain - we're adding ONE of these subfields to each of Primary and Luft reconstructions
        if (chain_is_loaded) {
            uint32_t growth = INFO_DVCF_LEN + 1 + (info_items.len32 > 2); // +1 for '=', +1 for ';' if we already have item(s)
            vb->recon_size += growth;
            vb->recon_size_luft += growth;
        }
    }

    else { 
        BNXT (InfoItem, info_items) = (InfoItem) { 
            .name      = INFO_LREJ_NAME"=", 
            .name_len  = INFO_DVCF_LEN + 1, 
            .ctx       = CTX (INFO_LREJ),
            .value     = "" // non-zero means value exists
        };

        BNXT (InfoItem, info_items) = (InfoItem) { 
            .name      = INFO_PREJ_NAME"=", 
            .name_len  = INFO_DVCF_LEN + 1, 
            .ctx       = CTX (INFO_PREJ),
            .value     = "" // non-zero means value exists
        };

        // case: we added a REJX INFO field that wasn't in the TXT data: --chain or rolled back (see vcf_lo_seg_rollback_and_reject) or an added variant
        if (chain_is_loaded || rolled_back || added_variant) {
            uint32_t growth = INFO_DVCF_LEN + 1 + (info_items.len32 > 2); // +1 for '=', +1 for ';' if we already have item(s) execpt for the DVCF items

            if (vb->line_coords == DC_PRIMARY) 
                vb->recon_size += growth;
            else 
                vb->recon_size_luft += growth;
        }
    }

    // add tags for the DVCF info items
    if (!vb->is_rejects_vb) {
        InfoItem *ii = BLST (InfoItem, info_items) - 1;
        vcf_tags_add_tag (vb, ii[0].ctx, DTYPE_VCF_INFO, ii[0].ctx->tag_name, ii[0].name_len-1);
        vcf_tags_add_tag (vb, ii[1].ctx, DTYPE_VCF_INFO, ii[1].ctx->tag_name, ii[1].name_len-1);
    }
}

static void vcf_seg_info_one_subfield (VBlockVCFP vb, ContextP ctx, STRp(value))
{
    unsigned modified_len = value_len + 20;
    char modified[modified_len]; // used for 1. fields that are optimized 2. fields translated luft->primary. A_1 transformed 4.321e-03->0.004321
        
    // note: since we use modified for both optimization and luft_back - we currently don't support
    // subfields having both translators and optimization. This can be fixed if needed.

    ctx->line_is_luft_trans = false; // initialize
    
    #define ADJUST_FOR_MODIFIED ({                                  \
        int32_t growth = (int32_t)modified_len - (int32_t)value_len;\
        if (growth) {                                               \
            vb->recon_size      += growth;                          \
            vb->recon_size_luft += growth;                          \
        }                                                           \
        STRset (value, modified); })                                       

    // --chain: if this is RendAlg=A_1 subfield in a REF⇆ALT variant, convert a eg 4.31e-03 to e.g. 0.00431. This is to
    // ensure primary->luft->primary is lossless (4.31e-03 cannot be converted losslessly as we can't preserve format info)
    if (chain_is_loaded && ctx->luft_trans == VCF2VCF_A_1 && LO_IS_OK_SWITCH (last_ostatus) && 
        str_scientific_to_decimal (STRa(value), qSTRa(modified), NULL)) {
        ADJUST_FOR_MODIFIED;
    }        

    // Translatable item on a Luft line: attempt to lift-back the value, so we can seg it as primary
    if (vb->line_coords == DC_LUFT && needs_translation (ctx)) {

        // If cross-rendering to Primary is successful - proceed to Seg this value in primary coords, and assign a translator for reconstructing if --luft
        if (vcf_lo_seg_cross_render_to_primary (vb, ctx, STRa(value), qSTRa(modified), false)) {
            STRset (value, modified); 
            ctx->line_is_luft_trans = true; // assign translator to this item in the container, to be activated with --luft
        } 

        // This item in Luft coordinates is not translatable to primary. It is therefore a luft-only line, and we seg the remainder of it items in
        // luft coords, and only ever reconstruct it in luft (as the line with have Coord=LUFT). Since this item is already in LUFT coords
        else 
            vcf_lo_seg_rollback_and_reject (vb, LO_INFO, ctx);
    }

    // validate that the primary value (as received from caller or lifted back) can be luft-translated 
    // note: looks at snips before optimization, we're counting on the optimization not changing the validation outcome
    if (vb->line_coords == DC_PRIMARY && needs_translation (ctx)) {

        if (DT_FUNC(vb, translator)[ctx->luft_trans](VB, ctx, (char *)value, value_len, 0, true)) 
            ctx->line_is_luft_trans = true; // assign translator to this item in the container, to be activated with --luft
        else 
            REJECT_SUBFIELD (LO_INFO, ctx, ".\tCannot cross-render INFO subfield %s: \"%.*s\"", ctx->tag_name, value_len, value);            
    }

    // ##INFO=<ID=AA,Number=1,Type=String,Description="Ancestral Allele">
    if (z_is_dvcf && ctx->luft_trans == VCF2VCF_ALLELE && !(segconf.vcf_is_cosmic && ctx->dict_id.num == _INFO_AA)) // INFO/AA in COSMIC is something else
        vcf_seg_INFO_allele (VB, ctx, STRa(value), 0);

    // many fields, for example: ##INFO=<ID=AN_amr_male,Number=1,Type=Integer,Description="Total number of alleles in male samples of Latino ancestry">
    else if ((ctx->tag_name[0] == 'A' && (ctx->tag_name[1] == 'N' || ctx->tag_name[1] == 'C') && (ctx->tag_name[2] == '_' || ctx->tag_name[2] == '-')) ||
             !memcmp (ctx->tag_name, "nhomalt", 7)) {
        seg_integer_or_not (VB, ctx, STRa(value), value_len);
    }

    else switch (ctx->dict_id.num) {
        #define CALL(f) ({ (f); break; })
        #define CALL_IF(cond,f)  if (cond) { (f); break; } else goto standard_seg 
        #define CALL_WITH_FALLBACK(f) if (f(vb, ctx, STRa(value))) { seg_by_ctx (VB, STRa(value), ctx, value_len); } break
        #define STORE_AND_SEG(store_type) ({ seg_set_last_txt_store_value (VB, ctx, STRa(value), store_type); seg_by_ctx (VB, STRa(value), ctx, value_len); break; })
        #define DEFER(f) ({ vb_add_to_deferred_q (VB, ctx, vcf_seg_INFO_##f, vb->idx_##f); seg_set_last_txt (VB, ctx, STRa(value)); break; })

        // ---------------------------------------
        // Fields defined in the VCF specification
        // ---------------------------------------
        case _INFO_AC:              CALL (vcf_seg_INFO_AC (vb, ctx, STRa(value))); 
        case _INFO_AA:              CALL_IF (!segconf.vcf_is_cosmic, vcf_seg_INFO_allele (VB, ctx, STRa(value), 0)); // note: in COSMIC, INFO/AA is something entirely different
        case _INFO_DP:              CALL (vcf_seg_INFO_DP (vb, ctx, STRa(value)));
        case _INFO_END:             CALL (vcf_seg_INFO_END (vb, ctx, STRa(value))); // note: END is an alias of POS - they share the same delta stream - the next POS will be a delta vs this END)

        // structural variation
        case _INFO_CIEND:           CALL (seg_by_did (VB, STRa(value), INFO_CIPOS, value_len));  // alias of INFO/CIPOS
        case _INFO_SVLEN:           CALL (vcf_seg_INFO_SVLEN (vb, ctx, STRa(value)));
        case _INFO_SVTYPE:          CALL_WITH_FALLBACK (vcf_seg_SVTYPE);

        // ---------------------------------------
        // GATK fields
        // ---------------------------------------
        case _INFO_RAW_MQandDP:     CALL (vcf_seg_INFO_RAW_MQandDP (vb, ctx, STRa(value)));
        case _INFO_HaplotypeScore:  CALL (seg_float_or_not (VB, ctx, STRa(value), value_len));
        case _INFO_BaseCounts:      CALL_WITH_FALLBACK (vcf_seg_INFO_BaseCounts);
        case _INFO_SF:              CALL_WITH_FALLBACK (vcf_seg_INFO_SF_init); // Source File
        case _INFO_MLEAC:           CALL (vcf_seg_INFO_MLEAC (vb, ctx, STRa(value)));
        case _INFO_MLEAF:           CALL (vcf_seg_INFO_MLEAF (vb, ctx, STRa(value)));
        case _INFO_QD:              DEFER (QD); // deferred seg to after samples
        case _INFO_RU:              CALL (vcf_seg_INFO_RU (vb, ctx, STRa(value)));
        case _INFO_RPA:             CALL (vcf_seg_INFO_RPA (vb, ctx, STRa(value)));
        case _INFO_MFRL:            
        case _INFO_MBQ:
        case _INFO_MMQ:             CALL (seg_array (VB, ctx, ctx->did_i, STRa(value), ',', 0, false, STORE_INT, DICT_ID_NONE, value_len));
        case _INFO_NALOD:
        case _INFO_NLOD:
        case _INFO_TLOD:            CALL (seg_add_to_local_string (VB, ctx, STRa(value), LOOKUP_NONE, value_len));
        case _INFO_GERMQ:
        case _INFO_CONTQ:
        case _INFO_SEQQ:
        case _INFO_STRANDQ:
        case _INFO_STRQ:
        case _INFO_ECNT:            CALL (seg_integer_or_not (VB, ctx, STRa(value), value_len));
        case _INFO_AS_SB_TABLE:     CALL_IF (segconf.AS_SB_TABLE_by_SB, DEFER(AS_SB_TABLE));

        case _INFO_VQSLOD: // Optimize VQSLOD 
            if (flag.optimize_VQSLOD && optimize_float_2_sig_dig (STRa(value), 0, modified, &modified_len)) 
                ADJUST_FOR_MODIFIED;
            goto standard_seg;

        // ---------------------------------------
        // VEP fields
        // ---------------------------------------
        case _INFO_CSQ:
        case _INFO_vep:             CALL_IF (segconf.vcf_is_vep, vcf_seg_INFO_CSQ (vb, ctx, STRa(value)));
        case _INFO_AGE_HISTOGRAM_HET:
        case _INFO_AGE_HISTOGRAM_HOM: 
        case _INFO_DP_HIST:
        case _INFO_GQ_HIST:         CALL (seg_uint32_matrix (VB, ctx, ctx->did_i, STRa(value), ',', '|', false, value_len));
        case _INFO_DP4:             CALL (vcf_seg_INFO_DP4 (vb, ctx, STRa(value)));
        case _INFO_MC:              CALL (seg_array (VB, ctx, ctx->did_i, STRa(value), ',', 0, false, STORE_NONE, DICT_ID_NONE, value_len));
        case _INFO_CLNDN:           CALL (seg_array (VB, ctx, ctx->did_i, STRa(value), '|', 0, false, STORE_NONE, DICT_ID_NONE, value_len));
        case _INFO_CLNID:           CALL (seg_integer_or_not (VB, ctx, STRa(value), value_len));
        case _INFO_CLNHGVS: // ClinVar & dbSNP
        case _INFO_HGVSG:   // COSMIC & Mastermind
            if (segconf.vcf_is_mastermind) 
                                    CALL (vcf_seg_mastermind_HGVSG (vb, ctx, STRa(value)));
            else                    CALL (vcf_seg_INFO_HGVS (VB, ctx, STRa(value), 0)); 

        // ##INFO=<ID=CLNVI,Number=.,Type=String,Description="the variant's clinical sources reported as tag-value pairs of database and variant identifier">
        // example: CPIC:0b3ac4db1d8e6e08a87b6942|CPIC:647d4339d5c1ddb78daff52f|CPIC:9968ce1c4d35811e7175cd29|CPIC:PA166160951|CPIC:c6c73562e2b9e4ebceb0b8bc
        // I tried seg_array_of_struct - it is worse than simple seg

        case _INFO_ALLELEID:        CALL (seg_integer_or_not (VB, ctx, STRa(value), value_len));
        case _INFO_RSID:            CALL (seg_id_field (VB, ctx, STRa(value), false, value_len));

        // bcftools csq
        case _INFO_BCSQ:            CALL (seg_array (VB, ctx, INFO_BCSQ, STRa(value), ',', '|', false, STORE_NONE, DICT_ID_NONE, value_len));

        // ---------------------------------------
        // SnpEff fields
        // ---------------------------------------
        case _INFO_ANN:             CALL (vcf_seg_INFO_ANN (vb, ctx, STRa(value)));
        case _INFO_EFF:             CALL (vcf_seg_INFO_EFF (vb, ctx, STRa(value)));

        // ---------------------------------------
        // ICGC
        // ---------------------------------------
        case _INFO_mutation:        CALL_IF (segconf.vcf_is_icgc, vcf_seg_INFO_mutation (vb, ctx, STRa(value)));
        case _INFO_CONSEQUENCE:     CALL_IF (segconf.vcf_is_icgc, seg_array (VB, ctx, INFO_CONSEQUENCE, STRa(value), ',', 0, false, STORE_NONE, DICT_ID_NONE, value_len));
        case _INFO_OCCURRENCE:      CALL_IF (segconf.vcf_is_icgc, seg_array (VB, ctx, INFO_OCCURRENCE,  STRa(value), ',', 0, false, STORE_NONE, DICT_ID_NONE, value_len));

        // ---------------------------------------
        // COSMIC
        // ---------------------------------------
        case _INFO_LEGACY_ID:       CALL_IF (segconf.vcf_is_cosmic, vcf_seg_INFO_LEGACY_ID   (vb, ctx, STRa(value)));
        case _INFO_SO_TERM:         CALL_IF (segconf.vcf_is_cosmic, vcf_seg_INFO_SO_TERM     (vb, ctx, STRa(value)));

        // ---------------------------------------
        // Mastermind
        // ---------------------------------------
        case _INFO_MMID3:           CALL_IF (segconf.vcf_is_mastermind, vcf_seg_INFO_MMID3   (vb, ctx, STRa(value)));
        case _INFO_MMURI3:          CALL_IF (segconf.vcf_is_mastermind, vcf_seg_INFO_MMURI3  (vb, ctx, STRa(value)));
        case _INFO_MMURI:           CALL_IF (segconf.vcf_is_mastermind, seg_add_to_local_string (VB, ctx, STRa(value), LOOKUP_NONE, value_len));
        case _INFO_GENE:            CALL_IF (segconf.vcf_is_mastermind, STORE_AND_SEG (STORE_NONE)); // consumed by vcf_seg_INFO_MMID3

        // ---------------------------------------
        // Illumina genotyping
        // ---------------------------------------
        case _INFO_PROBE_A:         CALL_IF (segconf.vcf_illum_gtyping, vcf_seg_PROBE_A      (vb, ctx, STRa(value)));
        case _INFO_PROBE_B:         CALL_IF (segconf.vcf_illum_gtyping, vcf_seg_PROBE_B      (vb, ctx, STRa(value)));
        case _INFO_ALLELE_A:        CALL_IF (segconf.vcf_illum_gtyping, vcf_seg_ALLELE_A     (vb, ctx, STRa(value)));
        case _INFO_ALLELE_B:        CALL_IF (segconf.vcf_illum_gtyping, vcf_seg_ALLELE_B     (vb, ctx, STRa(value)));
        case _INFO_ILLUMINA_CHR:    CALL_IF (segconf.vcf_illum_gtyping, vcf_seg_ILLUMINA_CHR (vb, ctx, STRa(value)));
        case _INFO_ILLUMINA_POS:    CALL_IF (segconf.vcf_illum_gtyping, vcf_seg_ILLUMINA_POS (vb, ctx, STRa(value)));
        case _INFO_ILLUMINA_STRAND: CALL_IF (segconf.vcf_illum_gtyping, vcf_seg_ILLUMINA_STRAND (vb, ctx, STRa(value)));
        case _INFO_refSNP:          CALL_IF (segconf.vcf_illum_gtyping, seg_id_field         (VB, ctx, STRa(value), false, value_len));

        // ---------------------------------------
        // dbSNP
        // ---------------------------------------
        case _INFO_dbSNPBuildID:    CALL_IF (segconf.vcf_is_dbSNP, seg_integer_or_not (VB, ctx, STRa(value), value_len));
        case _INFO_RS:              CALL_IF (segconf.vcf_is_dbSNP, vcf_seg_INFO_RS (vb, ctx, STRa(value)));
        case _INFO_RSPOS:           CALL_IF (segconf.vcf_is_dbSNP, vcf_seg_INFO_RSPOS (vb, ctx, STRa(value)));
        case _INFO_GENEINFO:        CALL_IF (segconf.vcf_is_dbSNP, seg_array (VB, ctx, INFO_GENEINFO, STRa(value), '|', 0, false, STORE_NONE, DICT_ID_NONE, value_len));
        case _INFO_VC:              CALL_IF (segconf.vcf_is_dbSNP, vcf_seg_INFO_VC (vb, ctx, STRa(value)));
        case _INFO_FREQ:            CALL_IF (segconf.vcf_is_dbSNP, seg_add_to_local_string (VB, ctx, STRa(value), LOOKUP_NONE, value_len));
        // case _INFO_TOPMED: // better leave as simple snip as the items are allele frequencies which are correleted

        // ---------------------------------------
        // dbNSFP
        // ---------------------------------------
        case _INFO_Polyphen2_HDIV_score : 
        case _INFO_PUniprot_aapos :
        case _INFO_SiPhy_29way_pi : CALL (seg_array (VB, ctx, ctx->did_i, STRa(value), ',', 0, false, STORE_NONE, DICT_ID_NONE, value_len));

        case _INFO_VEST3_score    :
        case _INFO_FATHMM_score   : CALL (seg_add_to_local_string (VB, ctx, STRa(value), LOOKUP_NONE, value_len));

        // ---------------------------------------
        // gnomAD
        // ---------------------------------------
        case _INFO_age_hist_het_bin_freq:
        //case _INFO_age_hist_hom_bin_freq: // same dict_id as _INFO_age_hist_het_bin_freq
        case _INFO_gq_hist_alt_bin_freq:
        //case _INFO_gq_hist_all_bin_freq:
        case _INFO_dp_hist_alt_bin_freq:
        //case _INFO_dp_hist_all_bin_freq:
        case _INFO_ab_hist_alt_bin_freq:
                                    CALL (seg_array (VB, ctx, ctx->did_i, STRa(value), '|', 0, false, STORE_INT, DICT_ID_NONE, value_len));

        // ---------------------------------------
        // VAGrENT
        // ---------------------------------------
        case _INFO_VD:              CALL_IF (segconf.vcf_is_vagrent, vcf_seg_INFO_VD (vb, ctx, STRa(value)));
        case _INFO_VW:              CALL_IF (segconf.vcf_is_vagrent, vcf_seg_INFO_VW (vb, ctx, STRa(value)));

        // ---------------------------------------
        // Illumina IsaacVariantCaller / starling
        // ---------------------------------------
        case _INFO_REFREP:          CALL_IF (segconf.vcf_is_isaac, seg_integer_or_not (VB, ctx, STRa(value), value_len));
        case _INFO_IDREP:           CALL_IF (segconf.vcf_is_isaac, vcf_seg_INFO_IDREP (vb, ctx, STRa(value)));
        case _INFO_CSQT:            CALL_IF (segconf.vcf_is_isaac, seg_array (VB, ctx, ctx->did_i, STRa(value), ',', 0, false, STORE_NONE, DICT_ID_NONE, value_len));
        case _INFO_cosmic:          CALL_IF (segconf.vcf_is_isaac, seg_array (VB, ctx, ctx->did_i, STRa(value), ',', 0, false, STORE_NONE, DICT_ID_NONE, value_len));

        // ---------------------------------------
        // Illumina DRAGEN fields
        // ---------------------------------------
        case _INFO_REFLEN:          CALL (vcf_seg_INFO_REFLEN (vb, ctx, STRa(value)));

        // ---------------------------------------
        // Ultima Genomics 
        // ---------------------------------------
        case _INFO_X_LM:
        case _INFO_X_RM:            CALL_IF (segconf.vcf_is_ultima, vcf_seg_INFO_X_LM_RM (vb, ctx, STRa(value)));
        case _INFO_X_IL:            CALL_IF (segconf.vcf_is_ultima, vcf_seg_INFO_X_IL (vb, ctx, STRa(value)));
        case _INFO_X_IC:            CALL_IF (segconf.vcf_is_ultima, vcf_seg_INFO_X_IC (vb, ctx, STRa(value)));
        case _INFO_X_HIN:           CALL_IF (segconf.vcf_is_ultima, vcf_seg_INFO_X_HIN (vb, ctx, STRa(value)));
        case _INFO_X_HIL:           CALL_IF (segconf.vcf_is_ultima, vcf_seg_INFO_X_HIL (vb, ctx, STRa(value)));
        case _INFO_TREE_SCORE:      CALL_IF (segconf.vcf_is_ultima, vcf_seg_INFO_X_HIL (vb, ctx, STRa(value)));
        case _INFO_VARIANT_TYPE:    CALL_IF (segconf.vcf_is_ultima, vcf_seg_INFO_VARIANT_TYPE (vb, ctx, STRa(value)));
        case _INFO_ASSEMBLED_HAPS:  CALL_IF (segconf.vcf_is_ultima, seg_integer_or_not (VB, ctx, STRa(value), value_len));
        case _INFO_FILTERED_HAPS:   CALL_IF (segconf.vcf_is_ultima, vcf_seg_INFO_FILTERED_HAPS (vb, ctx, STRa(value)));
        
        // ---------------------------------------
        // Platypus
        // ---------------------------------------
        case _INFO_TR:
        case _INFO_TC:
        case _INFO_TCF:
        case _INFO_PP:
        case _INFO_MGOF:            CALL_IF (segconf.vcf_is_platypus, seg_integer_or_not (VB, ctx, STRa(value), value_len));
        case _INFO_SC:              CALL_IF (segconf.vcf_is_platypus, vcf_seg_playpus_INFO_SC (vb, ctx, STRa(value)));
        case _INFO_HP:              CALL_IF (segconf.vcf_is_platypus, vcf_seg_playpus_INFO_HP (vb, ctx, STRa(value)));
        case _INFO_WS:
        case _INFO_WE:              CALL_IF (segconf.vcf_is_platypus, vcf_seg_playpus_INFO_WS_WE (vb, ctx, STRa(value)));
        case _INFO_TCR:             CALL_IF (segconf.vcf_is_platypus, vcf_seg_playpus_INFO_TCR (vb, ctx, STRa(value)));

        // ---------------------------------------
        // manta
        // ---------------------------------------
        // case _INFO_LEFT_SVINSSEQ: 
        // case _INFO_RIGHT_SVINSSEQ: // tried ACGT, better off without

        default: standard_seg:
            seg_by_ctx (VB, STRa(value), ctx, value_len);
            
            if (ctx->flags.store == STORE_INT) {
                int64_t val;
                if (str_get_int (STRa(value), &val))
                    ctx_set_last_value (VB, ctx, val);
            }
    }

    seg_set_last_txt (VB, ctx, STRa(value));
}

static SORTER (sort_by_subfield_name)
{ 
    InfoItem *ina = (InfoItem *)a;
    InfoItem *inb = (InfoItem *)b;
    
    return strncmp (ina->name, inb->name, MIN_(ina->name_len, inb->name_len));
}

void vcf_seg_info_subfields (VBlockVCFP vb, STRp(info))
{
    info_items.len = 0; // reset from previous line

    // case: INFO field is '.' (empty) (but not in DVCF as we will need to deal with DVCF items)
    if (!z_is_dvcf && IS_PERIOD (info) && !segconf.vcf_is_isaac) { // note: in Isaac, it slightly better to mux the "."
        seg_by_did (VB, ".", 1, VCF_INFO, 2); // + 1 for \t or \n
        return;
    }

    // parse the info string
    str_split (info, info_len, MAX_FIELDS-2, ';', pair, false); // -2 - leave room for LUFT + PRIM
    ASSVCF (n_pairs, "Too many INFO subfields, Genozip supports up to %u", MAX_FIELDS-2);

    buf_alloc (vb, &info_items, 0, n_pairs + 2, InfoItem, CTX_GROWTH, "info_items");

    InfoItem lift_ii = {}, rejt_ii = {};

    // pass 1: initialize info items + get indices of AC, and the DVCF items
    for (unsigned i=0; i < n_pairs; i++) {
        rom equal_sign = memchr (pairs[i], '=', pair_lens[i]);
        unsigned name_len = (unsigned)(equal_sign - pairs[i]); // nonsense if no equal sign
        unsigned tag_name_len = equal_sign ? name_len : pair_lens[i];

        InfoItem ii = { .name_len  = equal_sign ? name_len + 1                : pair_lens[i], // including the '=' if there is one
                        .value     = equal_sign ? equal_sign + 1              : NULL,
                        .value_len = equal_sign ? pair_lens[i] - name_len - 1 : 0  };
        memcpy (ii.name, pairs[i], ii.name_len); // note: we make a copy the name, because vcf_seg_FORMAT_GT might overwrite the INFO field
        
        // create context if it doesn't already exist (also verifies tag is not too long)
        DictId dict_id = dict_id_make (pairs[i], tag_name_len, DTYPE_1);
        ii.ctx = ctx_get_ctx_tag (vb, dict_id, pairs[i], tag_name_len); // create if it doesn't already exist
        
        if (z_is_dvcf && !vb->is_rejects_vb) vcf_tags_add_tag (vb, ii.ctx, DTYPE_VCF_INFO, pairs[i], tag_name_len);

        if (segconf.running) segconf.has[ii.ctx->did_i]++;

        ASSVCF (!z_is_dvcf || 
                  (((dict_id.num != _INFO_LUFT && dict_id.num != _INFO_LREJ) || vb->line_coords == DC_PRIMARY) && 
                   ((dict_id.num != _INFO_PRIM && dict_id.num != _INFO_PREJ) || vb->line_coords == DC_LUFT)),
                "Not expecting INFO/%.*s in a %s-coordinate line", tag_name_len, pairs[i], vcf_coords_name (vb->line_coords));

        if (dict_id.num == _INFO_LUFT || dict_id.num == _INFO_PRIM) 
            { lift_ii = ii; continue; } // dont add LUFT and PRIM to Items yet

        else if (dict_id.num == _INFO_LREJ || dict_id.num == _INFO_PREJ) 
            { rejt_ii = ii; continue; } // dont add Lrej and Prej to Items yet

        #define X(x) case INFO_##x : vb->idx_##x = info_items.len32; break
        switch (ii.ctx->did_i) {
            X(AN); X(AF); X(AC); X(MLEAC); X(MLEAF); X(AC_Hom); X(AC_Het); X(AC_Hemi); X(DP); X(QD); X(SF);
            X(AS_SB_TABLE);
            default: {}
        }
        #undef X

        BNXT (InfoItem, info_items) = ii;
    }

    // case: we have a LUFT or PRIM item - Seg it now, but don't add it yet to InfoItems
    if (lift_ii.value) { 
        vcf_lo_seg_INFO_LUFT_and_PRIM (vb, lift_ii.ctx, lift_ii.value, lift_ii.value_len); 

        // case: we have both LIFT and REJT - could happen as a result of bcftools merge - discard the REJT for now, and let our Seg
        // decide if to reject it
        if (rejt_ii.value) {
            uint32_t shrinkage = rejt_ii.name_len + rejt_ii.value_len + 1; // unaccount for name, value and  
            vb->recon_size      -= shrinkage;
            vb->recon_size_luft -= shrinkage; // since its read from TXT, it is accounted for initialially in both recon_size and recon_size_luft
        }

        // case: line was reject - PRIM/LUFT changed to REJx (note: vcf_lo_seg_rollback_and_reject didn't decrement recon_size* in this case, bc PRIM/LUFT was not "encountered" yet)
        if (LO_IS_REJECTED (last_ostatus)) {
            vb->recon_size      -= lift_ii.value_len;
            vb->recon_size_luft -= lift_ii.value_len; // since its read from TXT, it is accounted for initialially in both recon_size and recon_size_luft
        }
    }
        
    // case: we have a *rej item - Seg it now, but don't add it yet to InfoItems
    else if (rejt_ii.value)
        vcf_lo_seg_INFO_REJX (vb, rejt_ii.ctx, rejt_ii.value, rejt_ii.value_len); 

    ARRAY (InfoItem, ii, info_items);

    // pass 2: seg all subfields except AC (and PRIM/LUFT that weren't added)
    for (unsigned i=0; i < ii_len; i++) 
        vcf_seg_info_one_subfield (vb, ii[i].ctx, STRa(ii[i].value));
}

// Seg INFO fields that were deferred to after all samples are segged
void vcf_seg_finalize_INFO_fields (VBlockVCFP vb)
{
    if (!info_items.len && !z_is_dvcf) return; // no INFO items on this line (except if dual-coords - we will add them in a sec)

    Container con = { .repeats             = 1, 
                      .drop_final_item_sep = true,
                      .filter_items        = z_is_dvcf,   // vcf_piz_filter chooses which (if any) DVCF item to show based on flag.luft and flag.single_coord
                      .callback            = z_is_dvcf }; // vcf_piz_container_cb appends oSTATUS to INFO if requested 
 
    // now that we segged all INFO and FORMAT subfields, we have the final ostatus and can add the DVCF items
    if (z_is_dvcf)
        vcf_seg_info_add_DVCF_to_InfoItems (vb);

    ARRAY (InfoItem, ii, info_items);

    con_set_nitems (con, ii_len);

    // if requested, we will re-sort the info fields in alphabetical order. This will result less words in the dictionary
    // thereby both improving compression and improving --regions speed. 
    if (flag.optimize_sort && ii_len > 1) 
        qsort (ii, ii_len, sizeof(InfoItem), sort_by_subfield_name);

    char prefixes[CONTAINER_MAX_PREFIXES_LEN];  // these are the Container prefixes
    prefixes[0] = prefixes[1] = CON_PX_SEP; // initial CON_PX_SEP follow by separator of empty Container-wide prefix
    unsigned prefixes_len = 2;

    // Populate the Container 
    uint32_t total_names_len=0;
    for (unsigned i=0; i < ii_len; i++) {
        // Set the Container item and find (or create) a context for this name
        con.items[i] = (ContainerItem){ .dict_id   = !ii[i].value                        ? DICT_ID_NONE 
                                                   : ii[i].ctx->dict_id.num == _INFO_END ? (DictId)_VCF_POS
                                                   :                                       ii[i].ctx->dict_id,
                                        .separator = { ';' } }; 

        // if we're preparing a dual-coordinate VCF and this line needs translation to Luft - assign the liftover-translator for this item,
        if (ii[i].ctx && ii[i].ctx->line_is_luft_trans) { // item was segged in Primary coords and needs a luft translator to be reconstruced in --luft
            con.items[i].translator = ii[i].ctx->luft_trans;

            if (ii[i].ctx->luft_trans == VCF2VCF_A_AN)
                ii[i].ctx->flags.store = STORE_INT; // consumed by vcf_piz_luft_A_AN
        }
            
        // add to the prefixes
        ASSVCF (prefixes_len + ii[i].name_len + 1 <= CONTAINER_MAX_PREFIXES_LEN, 
                "INFO contains tag names that, combined (including the '='), exceed the maximum of %u characters", CONTAINER_MAX_PREFIXES_LEN);

        memcpy (&prefixes[prefixes_len], ii[i].name, ii[i].name_len);
        prefixes_len += ii[i].name_len;
        prefixes[prefixes_len++] = CON_PX_SEP;

        // don't include LIFTBACK or LIFTREJT because they are not reconstructed by default (genounzip) 
        // note: vcf_lo_seg_INFO_REJX / vcf_lo_seg_INFO_LUFT_and_PRIM already verified that this is a dual-coord file
        if (!ii[i].ctx || (ii[i].ctx->dict_id.num != _INFO_PRIM && ii[i].ctx->dict_id.num != _INFO_PREJ))
            total_names_len += ii[i].name_len + 1; // +1 for ; \t or \n separator
    }

    // --chain: if any tags need renaming we create a second, renames, prefixes string
    char ren_prefixes[con_nitems(con) * MAX_TAG_LEN]; 
    unsigned ren_prefixes_len = z_is_dvcf && !vb->is_rejects_vb ? vcf_tags_rename (vb, con_nitems(con), 0, 0, 0, B1ST (InfoItem, info_items), ren_prefixes) : 0;

    // seg deferred fields 
    for (uint8_t i=0; i < vb->deferred_q_len; i++) 
        vb->deferred_q[i].seg (VB);

    // case GVCF: multiplex by has_RGQ or FILTER in Isaac
    if (!segconf.running && (segconf.has[FORMAT_RGQ] || segconf.vcf_is_isaac)) {
        ContextP channel_ctx = 
            seg_mux_get_channel_ctx (VB, VCF_INFO, (MultiplexerP)&vb->mux_INFO, (segconf.has[FORMAT_RGQ] ? CTX(FORMAT_RGQ)->line_has_RGQ : vcf_isaac_info_channel_i (VB)));
        
        seg_by_did (VB, STRa(vb->mux_INFO.snip), VCF_INFO, 0);

        // if we're compressing a Luft rendition, swap the prefixes
        if (vb->line_coords == DC_LUFT && ren_prefixes_len) 
            container_seg_with_rename (vb, channel_ctx, &con, ren_prefixes, ren_prefixes_len, prefixes, prefixes_len, total_names_len /* names inc. = and separator */, NULL);
        else 
            container_seg_with_rename (vb, channel_ctx, &con, prefixes, prefixes_len, ren_prefixes, ren_prefixes_len, total_names_len /* names inc. = and separator */, NULL);
    }

    // case: not GVCF
    else {
        // if we're compressing a Luft rendition, swap the prefixes
        if (vb->line_coords == DC_LUFT && ren_prefixes_len) 
            container_seg_with_rename (vb, CTX(VCF_INFO), &con, ren_prefixes, ren_prefixes_len, prefixes, prefixes_len, total_names_len /* names inc. = and separator */, NULL);
        else 
            container_seg_with_rename (vb, CTX(VCF_INFO), &con, prefixes, prefixes_len, ren_prefixes, ren_prefixes_len, total_names_len /* names inc. = and separator */, NULL);
    }
}

