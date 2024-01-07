// ------------------------------------------------------------------
//   vcf_info_SF.c
//   Copyright (C) 2019-2024 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#include "vcf_private.h"
#include "seg.h"
#include "piz.h"
#include "context.h"
#include "strings.h"
#include "dict_id.h"
#include "reconstruct.h"

#define adjustment  CTX(INFO_SF)->last_delta
#define sf_snip_buf CTX(INFO_SF)->sf_snip
#define next param

// INFO/SF contains a comma-seperated list of the 0-based index of the samples that are NOT '.'
// example: "SF=0,1,15,22,40,51,59,78,88,89,90,112,124,140,147,155,156,164,168,183,189,197,211,215,216,217,222,239,244,256,269,270,277,281,290,291,299,323,338,340,348" 
// Algorithm: SF is segged either as an as-is string, or as a SPECIAL that includes the index of all the non-'.' samples. 
// if use_special_sf=YES, we use SNIP_SPECIAL and we validate the correctness during vcf_seg_FORMAT_GT -
// if it is wrong we set use_special_sf=NO. The assumption is that normally, it is either true for all lines or false.
bool vcf_seg_INFO_SF_init (VBlockVCFP vb, ContextP sf_ctx, STRp(value))
{
    switch (sf_ctx->use_special_sf) {

        case no: 
            return true; // "special" is suppressed - caller should go ahead and seg normally

        case unknown: 
            sf_ctx->use_special_sf = yes; // first call to this function, after finding that we have an INFO/SF field - set and fall through

        case yes: 
            // we store the SF value in a buffer, since seg_FORMAT_GT overlays the haplotype buffer onto txt_data and may override the SF field
            // we will need the SF data if the field fails verification in vcf_seg_INFO_SF_one_sample
            buf_alloc (vb, &vb->sf_txt, 0, value_len + 1, char, 2, "sf_txt"); // +1 for nul-terminator
            memcpy (B1STc (vb->sf_txt), value, value_len);
            vb->sf_txt.len = value_len;
            *BAFTc (vb->sf_txt) = 0; // nul-terminate
            vb->sf_txt.next = 0; 
            adjustment = 0;      
            
            // snip being contructed 
            buf_alloc (vb, &sf_snip_buf, 0, value_len + 20, char, 2, "sf_snip"); // initial value - we will increase if needed
            BNXTc (sf_snip_buf) = SNIP_SPECIAL;
            BNXTc (sf_snip_buf) = VCF_SPECIAL_SF;

            return false; // caller should not seg as we already did

        default:
            ABORT ("invalid use_special_sf=%d", sf_ctx->use_special_sf);
    }
}

// verify next number on the list of the SF field is sample_i (called from vcf_seg_FORMAT_GT)
void vcf_seg_INFO_SF_one_sample (VBlockVCFP vb)
{
    // case: no more SF values left to compare - we ignore this sample
    while (vb->sf_txt.next < vb->sf_txt.len) {

        buf_alloc (vb, &sf_snip_buf, 10, 0, char, 2, "sf_snip");

        char *sf_one_value = Bc (vb->sf_txt, vb->sf_txt.next); 
        char *after;
        int32_t value = strtol (sf_one_value, &after, 10);

        int32_t adjusted_sample_i = (int32_t)(vb->sample_i + adjustment); // adjustment is the number of values in SF that are not in samples

        // case: badly formatted SF field
        if (*after != ',' && *after != 0) {
            CTX(INFO_SF)->use_special_sf = no; // failed - turn off for the remainder of this vb
            break;
        }
        
        // case: value exists in SF and samples
        else if (value == adjusted_sample_i) {
            BNXTc (sf_snip_buf) = ',';
            vb->sf_txt.next = BNUM (vb->sf_txt, after) + 1; // +1 to skip comma
            break;
        }

        // case: value in SF file doesn't appear in samples - keep the value in the snip
        else if (value < adjusted_sample_i) {
            sf_snip_buf.len += str_int (value, BAFTc (sf_snip_buf));
            BNXTc (sf_snip_buf) = ',';
            adjustment++;
            vb->sf_txt.next = BNUM (vb->sf_txt, after) + 1; // +1 to skip comma
            // continue and read the next value
        }

        // case: value in SF is larger than current sample - don't advance iterator - perhaps future sample will cover it
        else { // value > adjusted_sample_i
            BNXTc (sf_snip_buf) = '~'; // skipped sample
            break; 
        }
    }
}

void vcf_seg_INFO_SF_seg (VBlockVCFP vb)
{   
    // case: SF data remains after all samples - copy it
    int32_t remaining_len = (uint32_t)(vb->sf_txt.len - vb->sf_txt.next); // -1 if all done, because we skipped a non-existing comma
    if (remaining_len > 0) {
        buf_add_more (VB, &sf_snip_buf, Bc (vb->sf_txt, vb->sf_txt.next), remaining_len, "sf_snip");
        BNXTc (sf_snip_buf) = ','; // buf_add_more allocates one character extra
    }

    if (CTX(INFO_SF)->use_special_sf == yes) 
        seg_by_ctx (VB, STRb(sf_snip_buf), CTX(INFO_SF), vb->sf_txt.len);
    
    else if (CTX(INFO_SF)->use_special_sf == no)
        seg_by_ctx (VB, STRb(vb->sf_txt), CTX(INFO_SF), vb->sf_txt.len);

    buf_free (vb->sf_txt);
    buf_free (sf_snip_buf);
}

#undef adjustment
#undef param

#define adjustment vb->contexts[INFO_SF].last_delta
#define sample_i   vb->contexts[INFO_SF].last_value.i
#define snip_i     sf_snip_buf.param

// leave space for reconstructing SF - actual reconstruction will be in vcf_piz_container_cb
SPECIAL_RECONSTRUCTOR_DT (vcf_piz_special_INFO_SF)
{
    VBlockVCFP vb = (VBlockVCFP)vb_;

    if (reconstruct) {
        adjustment    = 0;
        sample_i      = 0; 
        snip_i        = 0;

        // temporary place for SF
        buf_alloc (vb, &vb->sf_txt, 0, 5 * vcf_header_get_num_samples(), char, 1, "sf_txt"); // initial estimate, we may further grow it later
        vb->sf_txt.len = 0;

        // copy snip to sf_snip (note: the SNIP_SPECIAL+code are already removed)
        buf_add_moreS (vb, &sf_snip_buf, snip, "sf_snip");
    }

    return NO_NEW_VALUE;
}

// While reconstructing the GT fields of the samples - calculate the INFO/SF field
void vcf_piz_GT_cb_calc_INFO_SF (VBlockVCFP vb, unsigned rep, char *recon, int32_t recon_len)
{
    if (rep != 0) return; // we only look at the first ht in a sample, and only if its not '.'/'%'

    if (*recon == '.' || *recon == '%') { // . can be written as % in vcf_seg_FORMAT_GT
        sample_i++;
        return;
    }

    ARRAY (const char, sf_snip, sf_snip_buf);

    while (snip_i < sf_snip_len) {

        buf_alloc (vb, &vb->sf_txt, 12, 0, char, 2, "sf_txt"); // sufficient for int32 + ','

        int32_t adjusted_sample_i = (int32_t)(sample_i + adjustment); // last_delta is the number of values in SF that are not in samples

        // case: we derive the value from sample_i
        if (sf_snip[snip_i] == ',') {
            snip_i++;

            buf_add_int_as_text (&vb->sf_txt, adjusted_sample_i);

            if (snip_i < sf_snip_len) // add comma if not done yet
                BNXTc (vb->sf_txt) = ',';

            break;
        }

        // case: sample was not used to derive any value
        if (sf_snip[snip_i] == '~') {
            snip_i++;
            break;
        }

        // case: value quoted verbatim - output and continue searching for , or ~ for this sample
        else {
            char *comma = strchr (&sf_snip[snip_i], ',');
            unsigned len = comma - &sf_snip[snip_i];
            bool add_comma = (snip_i + len + 1 < sf_snip_len); // add comma if not last

            buf_add (&vb->sf_txt, &sf_snip[snip_i], len + add_comma);
            snip_i += len + 1;

            adjustment++;
        }
    }

    sample_i++;
}

// Upon completing the line - insert the calculated INFO/SF field to its place
void vcf_piz_insert_INFO_SF (VBlockVCFP vb)
{
    ARRAY (const char, sf_snip, sf_snip_buf);

    // if there are some items remaining in the snip (values that don't appear in samples) - copy them
    if (snip_i < sf_snip_len) {
        unsigned remaining_len = sf_snip_len - snip_i - 1; // all except the final comma
        buf_add_more ((VBlockP)vb, &vb->sf_txt, &sf_snip[snip_i], remaining_len, "sf_txt"); 
    }

    // make room for the SF txt and copy it to its final location
    vcf_piz_insert_field (vb, INFO_SF, STRb(vb->sf_txt));

    buf_free (sf_snip_buf);
    buf_free (vb->sf_txt);
}

#undef sample_i
#undef adjustment
#undef snip_i
