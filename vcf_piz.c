// ------------------------------------------------------------------
//   vcf_piz.c
//   Copyright (C) 2019-2021 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include <math.h>
#include "vcf_private.h"
#include "zfile.h"
#include "txtfile.h"
#include "seg.h"
#include "context.h"
#include "file.h"
#include "dict_id.h"
#include "strings.h"
#include "codec.h"
#include "reconstruct.h"

bool vcf_piz_read_one_vb (VBlock *vb, Section sl)
{ 
    ((VBlockVCFP)vb)->last_end_line_i = LAST_LINE_I_INIT;
    return true;
}

// determine whether we should reconstruct this VB in LUFT coordinates
bool vcf_vb_is_luft (VBlockP vb)
{
    if (!vb) return false; // txt header translation is handled by vcf_inspect_txt_header_piz, not a header translator

    return flag.luft && (vb->vb_coords & DC_LUFT); // if --luft, translate except if its primary-only
}

// returns true if section is to be skipped reading / uncompressing
bool vcf_piz_is_skip_section (VBlockP vb, SectionType st, DictId dict_id)
{
    if (flag.drop_genotypes && // note: if all samples are filtered out with --samples then flag.drop_genotypes=true (set in vcf_samples_analyze_field_name_line)
        (dict_id.num == dict_id_fields[VCF_FORMAT] || dict_id.num == dict_id_fields[VCF_SAMPLES] || dict_id_is_vcf_format_sf (dict_id)))
        return true;

    if (flag.gt_only && sections_has_dict_id (st) && dict_id_is_vcf_format_sf (dict_id) 
        && dict_id.num != dict_id_FORMAT_GT
        && dict_id.num != dict_id_FORMAT_GT_HT
        && dict_id.num != dict_id_PBWT_RUNS
        && dict_id.num != dict_id_PBWT_FGRC
        && dict_id.num != dict_id_FORMAT_GT_HT_INDEX)
        return true;

    // if --count, we only need TOPLEVEL and the fields needed for the available filters (--regions)
    if (flag.count && sections_has_dict_id (st) &&
        (     dict_id.num != dict_id_fields[VCF_TOPLEVEL] && 
              dict_id.num != dict_id_fields[VCF_CHROM]    && // easier to always have CHROM
             (dict_id.num != dict_id_fields[VCF_POS] || !flag.regions))) return true;

    return false;
}

// filter is called before reconstruction of a repeat or an item, and returns false if item should 
// not be reconstructed. contexts are not consumed.
CONTAINER_FILTER_FUNC (vcf_piz_filter)
{
    if (item < 0) return true;

    if (flag.gt_only && dict_id.num == dict_id_fields[VCF_SAMPLES] 
        && con->items[item].dict_id.num != dict_id_FORMAT_GT) 
        return false; 

    // in dual-coordinates files - get the COORDS and oSTATUS at the beginning of each line
    else if (con->items[item].dict_id.num == dict_id_fields[VCF_oSTATUS] || 
             con->items[item].dict_id.num == dict_id_fields[VCF_COORDS]) {
        if (flag.show_liftover)
            return true; // show
        else if (z_dual_coords)
            *reconstruct = false; // set last_index, without reconstructing
        else
            return false; // filter out entirely without consuming (non-DC files have no oStatus data)
    }

    // for dual-coordinates genozip files - select with to show the XXXXOVER or the XXXXBACK based on flag.luft
    else if (dict_id.num == dict_id_fields[VCF_INFO]) {
        if (vb->vb_coords == DC_LUFT && (con->items[item].dict_id.num == dict_id_INFO_LIFTOVER || con->items[item].dict_id.num == dict_id_INFO_REJTOVER))
            return false;
            
        else if (vb->vb_coords == DC_PRIMARY && (con->items[item].dict_id.num == dict_id_INFO_LIFTBACK || con->items[item].dict_id.num == dict_id_INFO_REJTBACK))
            return false;
    } 
        
    return true;    
}

// FORMAT - obey GT-only and drop-genotypes ; load haplotype line
SPECIAL_RECONSTRUCTOR (vcf_piz_special_FORMAT)
{
    VBlockVCF *vb_vcf = (VBlockVCF *)vb;

    if (flag.drop_genotypes) goto done;

    bool has_GT = (snip_len>=2 && snip[0]=='G' && snip[1] == 'T' && (snip_len==2 || snip[2] == ':'));
    
    if (reconstruct) {
        if (flag.gt_only) {
            if (has_GT)
                RECONSTRUCT ("GT\t", 3);
        }
        else 
            RECONSTRUCT (snip, snip_len);
    }

    // initialize haplotype stuff
    if (has_GT && !vb_vcf->ht_matrix_ctx &&
        (vb_vcf->ht_matrix_ctx = ctx_get_existing_ctx (vb, dict_id_FORMAT_GT_HT))) { // note: ht_matrix_ctx will be NULL if there is no GT data in this VB, despite GT appearing in the FORMAT

        vb_vcf->hapmat_index_ctx = ctx_get_existing_ctx (vb, dict_id_FORMAT_GT_HT_INDEX);
        codec_hapmat_piz_calculate_columns (vb);
    }
done:
    return false; // no new value
}

// ------------------------------------
// callback called after reconstruction
// ------------------------------------

static void inline vcf_piz_SAMPLES_subset_samples (VBlockVCFP vb, unsigned rep, int32_t recon_len)
{
    if (!samples_am_i_included (rep))
        vb->txt_data.len -= recon_len;
}

CONTAINER_CALLBACK (vcf_piz_container_cb)
{
    VBlockVCFP vcf_vb = (VBlockVCFP)vb;
    #define have_INFO_SF  (vcf_vb->sf_snip.len > 0)

    // case: we have an INFO/SF field and we reconstructed and we reconstructed a repeat (i.e. one ht) GT field of a sample 
    if (dict_id.num == dict_id_FORMAT_GT && have_INFO_SF) 
        vcf_piz_GT_cb_calc_INFO_SF (vcf_vb, rep, recon, recon_len);

    else if (is_top_level) {

        // case: we have an INFO/SF field and we reconstructed one VCF line
        if (have_INFO_SF) 
            vcf_piz_TOPLEVEL_cb_insert_INFO_SF (vcf_vb); // cleans up allocations - call even if line will be dropped due oSTATUS

        // case: we are reconstructing with --luft and we reconstructed one VCF line
        if (z_dual_coords) 
            vcf_lo_piz_TOPLEVEL_cb_drop_line_if_bad_oSTATUS_or_no_header (vb);
    }

    else if (dict_id.num == dict_id_fields[VCF_SAMPLES] && flag.samples) 
        vcf_piz_SAMPLES_subset_samples (vcf_vb, rep, recon_len);
}
