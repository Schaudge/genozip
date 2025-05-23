// ------------------------------------------------------------------
//   sam_pacbio.c
//   Copyright (C) 2022-2025 Genozip Limited. Patent pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited,
//   under penalties specified in the license.

// Compresses auxilliary fields generated by the BLASR aligner

#include "sam_private.h"

// read alignment start position (1 based)
void sam_seg_blasr_FI_i (VBlockSAMP vb, ZipDataLineSAMP dl, int64_t fi, unsigned add_bytes)
{
    decl_ctx (OPTION_FI_i);

    // check if prediction is correct (it is often correct, but not always)
    if (fi == 1 + vb->soft_clip[dl->FLAG.rev_comp])
        seg_special0 (VB, SAM_SPECIAL_FI, ctx, add_bytes);

    else 
        seg_integer (VB, ctx, fi, true, add_bytes);
}

SPECIAL_RECONSTRUCTOR (sam_piz_special_FI)
{
    new_value->i = 1 + VB_SAM->soft_clip[last_flags.rev_comp];

    if (reconstruct)
        RECONSTRUCT_INT (new_value->i);

    return HAS_NEW_VALUE;
}
