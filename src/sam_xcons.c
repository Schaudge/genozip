// ------------------------------------------------------------------
//   sam_xcons.c
//   Copyright (C) 2022-2023 Genozip Limited. Patent pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited,
//   under penalties specified in the license.

// Compresses auxilliary fields generated by BSBolt

#include "sam_private.h"
#include "reconstruct.h"

// in consensus reads XO:i is sometimes close to AS:i
void sam_seg_xcons_XO_i (VBlockSAMP vb, ZipDataLineSAM *dl, int64_t xo, unsigned add_bytes)
{
    int32_t as;

    if (sam_seg_get_aux_int (vb, vb->idx_AS_i, &as, IS_BAM_ZIP, 1, 0x7fffffff, SOFT_FAIL)) {
        int channel_i = (as == dl->SEQ.len);
        ContextP channel_ctx = seg_mux_get_channel_ctx (VB, OPTION_XO_i, (MultiplexerP)&vb->mux_XO, channel_i);

        seg_delta_vs_other_do (VB, channel_ctx, CTX(OPTION_AS_i), 0, 0, xo, -1, add_bytes);
        seg_by_did (VB, STRa(vb->mux_XO.snip), OPTION_XO_i, 0); // de-multiplexer
    }

    else
        sam_seg_aux_field_fallback_int (vb, CTX(OPTION_XO_i), xo, add_bytes);
}

SPECIAL_RECONSTRUCTOR (sam_piz_special_DEMUX_BY_AS)
{
    int64_t as = reconstruct_peek (vb, CTX(OPTION_AS_i), 0, 0).i;
    
    int channel_i = (as == vb->seq_len);

    return reconstruct_demultiplex (vb, ctx, STRa(snip), channel_i, new_value, reconstruct);
}

// YY is predicted to be non-0 iff XX is 0
void sam_seg_xcons_YY_i (VBlockSAMP vb, int64_t yy, unsigned add_bytes)
{
    int channel_i = ctx_has_value_in_line_(VB, CTX(OPTION_XX_i)) && !CTX(OPTION_XX_i)->last_value.i;

    ContextP channel_ctx = seg_mux_get_channel_ctx (VB, OPTION_YY_i, (MultiplexerP)&vb->mux_YY, channel_i);

    seg_integer (VB, channel_ctx, yy, false, add_bytes);
    ctx_set_last_value (VB, CTX(OPTION_YY_i), yy);

    seg_by_did (VB, STRa(vb->mux_YY.snip), OPTION_YY_i, 0); // de-multiplexer
}

SPECIAL_RECONSTRUCTOR (sam_piz_special_DEMUX_BY_XX_0)
{
    int channel_i = ctx_has_value_in_line_(VB, CTX(OPTION_XX_i)) && !CTX(OPTION_XX_i)->last_value.i;

    return reconstruct_demultiplex (vb, ctx, STRa(snip), channel_i, new_value, reconstruct);
}

// XC is predicted to be XX + YY
void sam_seg_xcons_XC_i (VBlockSAMP vb, int64_t xc, unsigned add_bytes)
{
    int64_t prediction = 0;
    if (ctx_has_value_in_line_(VB, CTX(OPTION_XX_i))) prediction += CTX(OPTION_XX_i)->last_value.i;
    if (ctx_has_value_in_line_(VB, CTX(OPTION_YY_i))) prediction += CTX(OPTION_YY_i)->last_value.i;
    if (ctx_has_value_in_line_(VB, CTX(OPTION_XY_i))) prediction += CTX(OPTION_XY_i)->last_value.i;
    
    if (xc == prediction)
        seg_by_did (VB, STRa(XC_snip), OPTION_XC_i, add_bytes);

    else
        sam_seg_aux_field_fallback_int (vb, CTX(OPTION_XC_i), xc, add_bytes);
}
