// ------------------------------------------------------------------
//   sam_minimap2.c
//   Copyright (C) 2020-2025 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited,
//   under penalties specified in the license.

#include "sam_private.h"
#include "chrom.h"

// -------------------------------------------------------
// s1:i Chaining score
// -------------------------------------------------------

void sam_seg_s1_i (VBlockSAMP vb, ZipDataLineSAMP dl, int64_t s1, unsigned add_bytes)
{
    int32_t as;
    if (sam_seg_peek_int_field (vb, OPTION_AS_i, vb->idx_AS_i, -0x8000000, 0x7fffffff, true/*needed for delta*/, &as)) 
        seg_delta_vs_other_localN (VB, CTX(OPTION_s1_i), CTX(OPTION_AS_i), s1, 255, add_bytes);

    else
        seg_integer_as_snip_do (VB, CTX(OPTION_s1_i), s1, add_bytes); // unlikely to be ever reached - seg as text to keep context as LT_SINGLETON
}

// -------------------------------------------------------
// s2:i Chaining score of the best secondary chain
// -------------------------------------------------------

void sam_seg_s2_i (VBlockSAMP vb, ZipDataLineSAMP dl, int64_t s2, unsigned add_bytes)
{
}

// -------------------------------------------------------
// cm:i Number of minimizers on the chain
// -------------------------------------------------------

void sam_seg_cm_i (VBlockSAMP vb, ZipDataLineSAMP dl, int64_t cm, unsigned add_bytes)
{
    if (segconf_running) {
        // calculate average SEQ.len / cm:i
        segconf.seq_len_to_cm += (cm > 0) ? (int)((float)dl->SEQ.len / (float)cm + 0.5) : 0;
        goto fallback;
    }

    else if (segconf.seq_len_to_cm) {
        int32_t prediction = dl->SEQ.len / segconf.seq_len_to_cm;

        SNIPi2 (SNIP_SPECIAL, SAM_SPECIAL_cm, (int32_t)cm - prediction);
        seg_by_did (VB, STRa(snip), OPTION_cm_i, add_bytes); 
    }

    else fallback:
        seg_integer (VB, CTX(OPTION_cm_i), cm, true, add_bytes);
}

SPECIAL_RECONSTRUCTOR (sam_piz_special_cm)
{
    int32_t prediction = vb->seq_len / segconf.seq_len_to_cm;

    new_value->i = prediction + atoi(snip);

    if (reconstruct) RECONSTRUCT_INT (new_value->i);
    
    return HAS_NEW_VALUE;
}

// ----------------------------------------------------------
// ms:i DP score of the max scoring segment in the alignment
// ----------------------------------------------------------

// void sam_seg_ms_i (VBlockSAMP vb, ZipDataLineSAMP dl, int64_t ms, unsigned add_bytes)
// {
//     // if (ms == vb->ref_and_seq_consumed)
// }
