// ------------------------------------------------------------------
//   sam_bsbolt.c
//   Copyright (C) 2022-2025 Genozip Limited. Patent pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited,
//   under penalties specified in the license.

// Compresses auxilliary fields generated by BSBolt

#include "sam_private.h"

// Mapping strand (C=Crick, W=Watson) and alignment conversion pattern (C2T or G2A)
// 4 possible values: W_C2T ; W_G2A ; C_C2T ; C_G2A
void sam_seg_bsbolt_YS_Z (VBlockSAMP vb, ZipDataLineSAMP dl, STRp(ys), unsigned add_bytes)
{
    // note: we expect Watson strand alignments to be revcomp=false and Crick to be revcomp=true
    ASSSEG (ys_len == 5 && (ys[0]=='W' || ys[0]=='C') && (!memcmp (ys+1, "_C2T", 4) || !memcmp (ys+1, "_G2A", 4)),
            "Invalid YS:Z=%.*s value: expecting one of: W_C2T ; W_G2A ; C_C2T ; C_G2A", STRf(ys));

    seg_special2 (VB, SAM_SPECIAL_BSBOLT_YS, 
                  vb->bisulfite_strand?'*' : ((ys[0] == 'C') == dl->FLAG.rev_comp)?'^' : ys[0], ys[2], 
                  CTX(OPTION_YS_Z), add_bytes);
}

SPECIAL_RECONSTRUCTOR (sam_piz_special_BSBOLT_YS)
{
    if (reconstruct) {
        RECONSTRUCT1 (snip[0] == '*' ? "WC"[VB_SAM->bisulfite_strand == 'G'] 
                    : snip[0] == '^' ? "WC"[last_flags.rev_comp]
                    :                  snip[0]);

        RECONSTRUCT (snip[1]=='C' ? "_C2T" : "_G2A", 4);
    }

    return NO_NEW_VALUE;
}

// enter methylatble bases into the INTERNAL reference in their unconverted form 
// (not currently used as bisulfite features are disabled for REF_INTERNAL (bug 648), and not thoroughly tested)
void sam_seg_bsbolt_XB_Z_analyze (VBlockSAMP vb, ZipDataLineSAMP dl)
{
    if (!IS_REF_INTERNAL || // analyzing sets bases in an internal reference - not needed if not internal
        has_MD ||           // analyzing MD sets the same bases
        !has(XB_Z) || !vb->bisulfite_strand || vb->comp_i != SAM_COMP_MAIN) return;

    STR(xb);
    sam_seg_get_aux_Z (vb, vb->idx_XB_Z, pSTRa(xb), IS_BAM_ZIP);
    uint32_t xb_i = 0;

    RangeP range = NULL;
    RefLock lock = REFLOCK_NONE;
    uint32_t ref_consumed = vb->ref_consumed; // M/=/X and D
    PosType32 pos = dl->POS;
    uint32_t number=0;

    #define set_number ({ if (!number) {                                \
                              char *after;                              \
                              number = strtol (&xb[xb_i], &after, 10);  \
                              xb_i += after - &xb[xb_i];                \
                          }; number; })                                            

    for_cigar (vb->binary_cigar) {
        case BC_M: case BC_E: case BC_X: 
            for (uint32_t i=0; i < op->n; i++) {
                if (!number && IS_DIGIT(xb[xb_i])) set_number; 

                if (number) number--;
                
                else {
                    rom error = sam_seg_analyze_set_one_ref_base (vb, false, pos, vb->bisulfite_strand, ref_consumed, &range, &lock); 
                    if (error == ERR_ANALYZE_RANGE_NOT_AVAILABLE) return; // possibly pos/ref_consumed go beyond end of range

                    xb_i++;
                }
        
                pos++;
                ref_consumed--;
            }
            break;

        case BC_D: case BC_N:
            pos += op->n;
            ref_consumed -= op->n;
            break;
        
        // note: soft-clip (BC_S) bases are not represented in the XB:Z string
        case BC_I:
            set_number;
            ASSSEG (number >= op->n, "expecting number=%u >= op->n=%u (XB:Z=\"%.*s\" CIGAR=\"%s\")", 
                    number, op->n, STRf(xb),
                    dis_binary_cigar (VB, B1ST(BamCigarOp, vb->binary_cigar), vb->binary_cigar.len32, &vb->scratch).s);

            number -= op->n;
            break;

        default: {}
    }

    ASSSEG (xb_i == xb_len, "Mismatch between XB:Z=\"%.*s\" and CIGAR=\"%s\" (xb_i=%u xb_len=%u)", STRf(xb), 
            dis_binary_cigar (VB, B1ST(BamCigarOp, vb->binary_cigar), vb->binary_cigar.len32, &vb->scratch).s, xb_i, xb_len);

    if (range) ref_unlock (&lock);
}

static void show_wrong_xb (VBlockSAMP vb, ZipDataLineSAMP dl, STRp(XB), rom extra)
{
    iprintf ("%s: QNAME=\"%.*s\" bisulfite_strand=%c XB=\"%.*s\" CIGAR=\"%.*s\" %s\n", 
             LN_NAME, dl->QNAME_len, dl_qname(dl), vb->bisulfite_strand, STRf(XB), STRfw(dl->CIGAR), extra);
    if (vb->scratch.len32)   printf ("XM: %.*s\n", STRfb(vb->scratch));
    if (vb->meth_call.len32) printf ("SQ: %.*s\n\n", STRfb(vb->meth_call));
}

// Read bisulfite conversion position and context
// Example: XB:Z:2z10x4z10z5zzZz10z1z5zZzz3z9z1zzz1zzz1z3z8zz15z1X2z5
// X/x=methylated/unmethylated CpG ; Y/y=CHG Z/z=CHH ; numbers=gaps
void sam_seg_bsbolt_XB (VBlockSAMP vb, ZipDataLineSAMP dl, STRp(XB), unsigned add_bytes)
{
    START_TIMER;

    static const char bsbolt_to_bismark[256] = { ['X']='Z', ['x']='z', ['Y']='X', ['y']='x', ['Z']='H', ['z']='h' };    

    // in PRIM and DEPN we dont have the methylation call because we didn't seg SEQ vs reference. To do: generate methylation call prediction in this case too/
    if (vb->comp_i != SAM_COMP_MAIN) goto fallback;

    // convert XB to Bismark format
    buf_alloc_exact (vb, vb->scratch, dl->SEQ.len, char, "scratch");
    char *bis = B1STc (vb->scratch);
    
    // soft-clips don't appear in bsbolt's XB but do appear in Bismark's XM
    if (vb->soft_clip[0]) memset (bis, '.', vb->soft_clip[0]);
    if (vb->soft_clip[1]) memset (&bis[dl->SEQ.len - vb->soft_clip[1]], '.', vb->soft_clip[1]);
    
    uint32_t xb_i=0; 
    uint32_t bis_i = vb->soft_clip[0];
    uint32_t after_bis = dl->SEQ.len - vb->soft_clip[1];

    while (xb_i < XB_len && bis_i < after_bis) {
        if (IS_DIGIT(XB[xb_i])) {
            char *after;
            uint32_t n = strtol (&XB[xb_i], &after, 10);
            if (bis_i + n > dl->SEQ.len) goto fallback;

            memset (&bis[bis_i], '.', n);
            bis_i += n;

            uint32_t len = after - &XB[xb_i];
            xb_i += len;
        }

        else {
            char bismark = bsbolt_to_bismark[(uint8_t)XB[xb_i]];
            if (!bismark) goto fallback;
            
            bis[bis_i++] = bismark;
            xb_i++;
        }
    }

    if (xb_i < XB_len || bis_i < after_bis) goto fallback;

    if (flag.show_wrong_xb && !segconf_running && vb->scratch.len32 && vb->meth_call.len32 && !str_issame_(STRb(vb->scratch),STRb(vb->meth_call))) 
        show_wrong_xb (vb, dl, STRa(XB), vb->meth_call.len ? "" : "(no meth_call)");

    COPY_TIMER (sam_seg_bsbolt_XB); // note: sam_seg_bismark_XM_Z accounts for itself

    sam_seg_bismark_XM_Z (vb, dl, OPTION_XB_Z, SAM_SPECIAL_BSBOLT_XB, STRb(vb->scratch), add_bytes);

    buf_free (vb->scratch);
    return;
 
fallback:
    if (flag.show_wrong_xb && !segconf_running) 
        show_wrong_xb (vb, dl, STRa(XB), "(fallback)");

    buf_free (vb->scratch);

    seg_add_to_local_string (VB, CTX(OPTION_XB_Z), STRa(XB), LOOKUP_SIMPLE, add_bytes);
}

SPECIAL_RECONSTRUCTOR_DT (sam_piz_special_BSBOLT_XB)
{
    static const char bismark_to_bsbolt[256] = { ['Z']='X', ['z']='x', ['X']='Y', ['x']='y', ['H']='Z', ['h']='z' };

    VBlockSAMP vb = (VBlockSAMP)vb_; 
    char *recon = BAFTtxt, *next = recon, *start = recon;

    sam_piz_special_BISMARK_XM (VB, ctx, STRa(snip), new_value, reconstruct);
    if (!reconstruct) goto done;

    // convert Bismark format back to BSBolt format
    char *after = BAFTtxt;
    while (recon < after) {
        if (*recon == '.') {
            uint32_t n = str_count_consecutive_char (recon, after - recon, '.');
            
            if (recon == start)     { recon += vb->soft_clip[0] ; n -= vb->soft_clip[0]; }
            if (recon + n == after) { recon += vb->soft_clip[1] ; n -= vb->soft_clip[1]; }
            
            if (n) { // n==0 if all of is soft_clip[1]
                next  += str_int_ex (n, next, false);
                recon += n;
            }
        }

        else 
            *next++ = bismark_to_bsbolt[(uint8_t)*recon++];
    }

    Ltxt = BNUMtxt (next);

done:
    return NO_NEW_VALUE;
}
  