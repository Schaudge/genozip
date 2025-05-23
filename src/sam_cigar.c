// ------------------------------------------------------------------
//   sam_cigar.c
//   Copyright (C) 2019-2025 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

// a module for handling CIGAR and MC:Z

#include "sam_private.h"
#include "md5.h"
#include "random_access.h"
#include "chrom.h"
#include "huffman.h"
#include "htscodecs/rANS_static4x16.h"

static const bool cigar_valid_op[256] = { ['M']=true, ['I']=true, ['D']=true, ['N']=true, ['S']=true, ['H']=true, ['P']=true, ['=']=true, ['X']=true }; 

const char cigar_op_to_char[16] = "MIDNSHP=Xabcdefg"; // BAM to SAM (a-g are invalid values)

#ifdef __clang__ 
#pragma GCC diagnostic ignored "-Winitializer-overrides" // overlapping indices in this array initializer
#endif
static const uint8_t cigar_char_to_op[256] = { [0 ... 255]=BC_INVALID, 
                                               ['M']=BC_M, ['I']=BC_I, ['D']=BC_D, ['N']=BC_N, ['S']=BC_S, 
                                               ['H']=BC_H, ['P']=BC_P, ['=']=BC_E, ['X']=BC_X, ['*']=BC_NONE }; 

#undef S 
#define S (c == 'S')
#define H (c == 'H')
#define I (c == 'I')
#define D (c == 'D')
#define M (c == 'M')
#define N (c == 'N')
#define P (c == 'P')
#define E (c == '=')
#define X (c == 'X')

//---------
// Shared
//---------

StoH sam_cigar_S_to_H (VBlockSAMP vb, STRc(cigar), bool is_binary)
{
    StoH s2h = { .is_binary = is_binary };

    if (is_binary) {
        BamCigarOp *binary_cigar = (BamCigarOp *)cigar;
        cigar_len /= sizeof (BamCigarOp);

        if (binary_cigar[0].op == BC_S) {
            binary_cigar[0].op = BC_H;
            s2h.left = (char *)&binary_cigar[0];
        }

        if (binary_cigar[cigar_len-1].op == BC_S) {
            binary_cigar[cigar_len-1].op = BC_H;
            s2h.right = (char *)&binary_cigar[cigar_len-1];
        }
    }

    else {
        // replace left clipping - find first op
        char *c = cigar; while (IS_DIGIT(*c)) c++;
        if (*c == 'S') {
            *c = 'H';
            s2h.left = c;
        }

        // replace right clipping
        if (cigar[cigar_len-1] == 'S') {
            s2h.right = &cigar[cigar_len-1];
            *s2h.right = 'H';
        }
    }

    return s2h;
}

HtoS sam_cigar_H_to_S (VBlockSAMP vb, STRc(cigar), bool is_binary)
{
    HtoS h2s = { .is_binary = is_binary };

    if (is_binary) {
        BamCigarOp *binary_cigar = (BamCigarOp *)cigar;
        cigar_len /= sizeof (BamCigarOp);

        if (binary_cigar[0].op == BC_H) {
            binary_cigar[0].op = BC_S;
            h2s.left = (char *)&binary_cigar[0];
        }

        if (binary_cigar[cigar_len-1].op == BC_H) {
            binary_cigar[cigar_len-1].op = BC_S;
            h2s.right = (char *)&binary_cigar[cigar_len-1];
        }
    }

    else {
        // replace left clipping - find first op
        if (vb->hard_clip[0]) {
            char *c = cigar; while (IS_DIGIT(*c)) c++;
            if (*c == 'H') {
                *c = 'S';
                h2s.left = c;
            } 
        }

        // replace right clipping
        if (cigar[cigar_len-1] == 'H') {
            h2s.right = &cigar[cigar_len-1];
            *h2s.right = 'S';
        }
    }

    return h2s;
}

bool sam_cigar_has_H (STRp(cigar)) // textual
{
    if (cigar[cigar_len-1] == 'H') return true;

    // skip digits to first op
    while (IS_DIGIT(*cigar) && cigar_len) { cigar++; cigar_len--; }

    if (!cigar_len) return false; // no op was found - invalid CIGAR

    return *cigar == 'H';
}

// gets seq_len implied by cigar or squanked cigar segments: "M24S" "M14S" "S" "". 
static uint32_t sam_cigar_get_seq_len_plus_H (STRp(cigar))
{
    uint32_t n=0, seq_len=0;

    for (uint32_t i=0; i < cigar_len; i++) {
        char c = cigar[i];
        if (IS_DIGIT(c)) 
            n = n*10 + c-'0';

        else { // op
            if (M || I || S || E || X || H) seq_len += n;
            n = 0;
        }
    }
    return seq_len;
}

void sam_cigar_binary_to_textual (VBlockP vb, const BamCigarOp *cigar, uint16_t n_cigar_op, 
                                  bool reverse, // reverse the cigar 15M3I20S -> 20S3I15M
                                  BufferP textual_cigar /* out */)
{
    START_TIMER;

    if (!n_cigar_op) {
        buf_alloc (vb, textual_cigar, 2, 0, char, 100, textual_cigar->name ? NULL : "textual_cigar");
        BNXTc (*textual_cigar) = '*';
        *BAFTc (*textual_cigar) = 0; // nul terminate
        goto finish;
    }

    // calculate length
    uint32_t len=0;
    for (uint16_t i=0; i < n_cigar_op; i++) {
        uint32_t op_len = cigar[i].n; // maximum is 268,435,455
        if      (op_len < 10)       len += 2; // 1 for the op, 1 for the number
        else if (op_len < 100)      len += 3; 
        else if (op_len < 1000)     len += 4; 
        else if (op_len < 10000)    len += 5; 
        else if (op_len < 100000)   len += 6; 
        else if (op_len < 1000000)  len += 7;
        else if (op_len < 10000000) len += 8;
        else                        len += 9;
    }

    buf_alloc (vb, textual_cigar, len + 1 /* for \0 */, 100, char, 0, textual_cigar->name ? NULL : "textual_cigar");

    char *next = BAFTc (*textual_cigar);

    if (!reverse)
        for (int i=0; i < n_cigar_op; i++) {
            next += str_int_fast (cigar[i].n, next);
            *next++ = cigar_op_to_char[cigar[i].op];
        }
    else
        for (int i=n_cigar_op-1; i >= 0 ; i--) {
            next += str_int_fast (cigar[i].n, next);
            *next++ = cigar_op_to_char[cigar[i].op];
        }

    *next = 0; // nul terminate
    textual_cigar->len32 = BNUM (*textual_cigar, next);

finish:    
    COPY_TIMER (sam_cigar_binary_to_textual);
}

static void sam_reverse_binary_cigar (BufferP in_buf, BufferP out_buf/*can be the same as in_buf*/, rom out_buf_name)
{
    ARRAY (BamCigarOp, in, *in_buf);

    if (in_buf == out_buf)
        for (uint32_t i=0; i < in_buf->len32 / 2; i++)
            SWAP (in[i], in[in_len - i - 1]);

    else {
        ARRAY_alloc (BamCigarOp, out, in_len, false, *out_buf, in_buf->vb, out_buf_name);
        for (uint32_t i=0; i < in_buf->len32; i++)
            out[i] = in[in_len - i - 1];
    }
}

rom display_binary_cigar (VBlockSAMP vb)
{
    buf_free (vb->textual_cigar); // we might destroy needed data, but its ok as this is only called in an error condition
    sam_cigar_binary_to_textual (VB, B1ST(BamCigarOp, vb->binary_cigar), vb->binary_cigar.len32, false, &vb->textual_cigar);
    return B1STc (vb->textual_cigar);
}

// return true if string is a valid textual cigar
bool sam_is_cigar (STRp(cigar), bool allow_empty)
{
    if (IS_ASTERISK(cigar)) return allow_empty; 

    rom after = cigar + cigar_len;

    while (cigar < after) {
        if (!IS_DIGIT(*cigar)) return false; // expecting at least one digit

        while (IS_DIGIT(*cigar) && (cigar < after-1)) cigar++; 
        
        if (cigar_char_to_op[(uint8_t)*cigar++] == 255) return false; // invalid op
    }

    return true;
}

bool sam_cigar_textual_to_binary (VBlockP vb, STRp(cigar), BufferP binary_cigar, rom buf_name)
{
    ASSERTNOTINUSE (*binary_cigar);

    if (IS_ASTERISK(cigar)) return true; // empty binary cigar

    uint32_t n_ops = 0;
    for (int i=0; i < cigar_len; i++)
        if (!IS_DIGIT (cigar[i])) n_ops++;

    ARRAY_alloc (BamCigarOp, ops, n_ops, false, *binary_cigar, vb, buf_name); // buffer must be already named by caller

    rom after = cigar + cigar_len;

    for (int op_i=0; op_i < n_ops; op_i++) {
        if (!IS_DIGIT(*cigar)) return false; // at least one digit (may be 0)

        uint32_t n=0; 
        while (IS_DIGIT(*cigar) && (cigar < after-1)) { 
            n = 10*n + *cigar - '0'; 
            cigar++; 
        }
        
        ops[op_i] = (BamCigarOp) { .op = cigar_char_to_op[(uint8_t)*cigar++], .n = n } ;
        
        if (ops[op_i].op == 255) { // invalid op
            buf_free (*binary_cigar);
            return false;
        }
    }

    return true;
}

// display first 10K characters of a binary cigar - textually
StrTextMegaLong dis_binary_cigar (VBlockP vb, ConstBamCigarOpP cigar, uint32_t cigar_len/*in ops*/, BufferP working_buf)
{
    ASSERTNOTINUSE (*working_buf);

    StrTextMegaLong out = {};
    sam_cigar_binary_to_textual (vb, STRa(cigar), false, working_buf);
    uint32_t len = MIN_(working_buf->len32, sizeof(out.s)-1);
    memcpy (out.s, working_buf->data, len);
    out.s[len] = 0;

    buf_free (*working_buf);
    return out;
}

// ZIP/PIZ: calculate the expected length of SEQ and QUAL from the CIGAR string
// A CIGAR looks something like: "109S19M23S", See: https://samtools.github.io/hts-specs/SAMv1.pdf 
void sam_cigar_analyze (VBlockSAMP vb, STRp(cigar)/* textual */, bool cigar_is_in_textual_cigar, uint32_t *seq_consumed)
{
    if (IS_ZIP) {
        // if the CIGAR is "*", later sam_seg_CIGAR uses the length from SEQ and store it as eg "151*". 
        // note: In PIZ it will be eg "151*" or "1*" if both SEQ and QUAL are "*", so this condition will be false
        if (cigar[0] == '*') {
            ASSINP (cigar_len == 1, "%s: Invalid CIGAR: %.*s", LN_NAME, STRf(cigar)); // expecting exactly "*"
            goto do_analyze; 
        }
    }

    else { // PIZ
        // case: a CIGAR string starting with '-' indicates missing SEQ 
        if (*cigar == '-') {
            vb->seq_missing = true;
            cigar++; // skip the '-'
            cigar_len--;
        }

        // store original textual CIGAR for use of sam_piz_special_MD, as in BAM it will be translated ; also cigar might point to mate data in ctx->per_line - ctx->per_line might be realloced as we store this line's CIGAR in it 
        if (!cigar_is_in_textual_cigar) {
            buf_add_moreS (VB, &vb->textual_cigar, cigar, "textual_cigar");
            *BAFTc (vb->textual_cigar) = 0; // nul-terminate (buf_add_more allocated space for it)
        }
    }

    // create the BAM-style cigar data in binary_cigar. 
    buf_alloc (vb, &vb->binary_cigar, 0, 1 + cigar_len/2 /* max possible n_cigar_op */, BamCigarOp, 2, "binary_cigar");
    ARRAY (BamCigarOp, bam_ops, vb->binary_cigar);

    uint32_t n=0, op_i=0;
    for (uint32_t i=0; i < cigar_len; i++) {

        char c = cigar[i];

        if (IS_DIGIT(c)) 
            n = n*10 + (c - '0');

        else {
            ASSINP (n, "%s: Invalid CIGAR: operation %c not preceded by a number. CIGAR=\"%.*s\"", LN_NAME, c, STRf(cigar));    

            // convert character CIGAR op to BAM cigar field op: "MIDNSHP=X" -> 012345678 ; * is our private value of BC_NONE
            bam_ops[op_i++] = (BamCigarOp){ .op = cigar_char_to_op[(uint8_t)c], .n = n };
            n = 0;
        }
    }          

    ASSINP (!n, "%s: Invalid CIGAR: expecting it to end with an operation character. CIGAR=\"%.*s\"", LN_NAME, STRf(cigar));

    vb->binary_cigar.len32 = op_i;

do_analyze:
    bam_seg_cigar_analyze (vb, (IS_ZIP ? DATA_LINE (vb->line_i) : NULL), seq_consumed);
}

// analyze the binary cigar 
void bam_seg_cigar_analyze (VBlockSAMP vb, ZipDataLineSAMP dl/*NULL if PIZ*/, uint32_t *seq_consumed)
{
    *seq_consumed = 0; // everything else is initialized in sam_reset_line
    ARRAY (BamCigarOp, cigar, vb->binary_cigar);

    // ZIP case: if the CIGAR is "*", later sam_seg_CIGAR uses the length from SEQ and store it as eg "151*". 
    // note: In PIZ it with a "*" CIGAR, binary_cigar will have a single BC_NONE op, so this condition will be false
    if (!cigar_len) {
        vb->cigar_missing = true;
        return;
    }

    for (uint32_t op_i=0; op_i < cigar_len; op_i++) {

        #define SEQ_CONSUMED     *seq_consumed            += cigar[op_i].n
        #define REF_CONSUMED     vb->ref_consumed         += cigar[op_i].n
        #define SEQ_REF_CONSUMED vb->ref_and_seq_consumed += cigar[op_i].n
        #define COUNT(x)         vb->x                    += cigar[op_i].n
        
        // an H must be the first or last op
        #define VERIFY_H ({ ASSERT(!op_i || op_i==cigar_len-1, "%s: H can only appear as the first or last op in the CIGAR string. cigar=\"%s\"", \
                                   LN_NAME, display_binary_cigar(vb)); })

        // an S must be either the first op (possibly proceeded by an H) or the last op (possibly followed by an H), eg 2H3S5M4S2H
        #define VERIFY_S ({ ASSERT(op_i==0 || (op_i==1 && cigar[0].op==BC_H) || op_i==cigar_len-1 || (op_i==cigar_len-2 && cigar[cigar_len-1].op==BC_H), \
                                   "%s: S can only appear as the first op (possibly proceeded by an H) or the last op (possibly followed by an H) in the CIGAR string. cigar=\"%s\"", \
                                   LN_NAME, display_binary_cigar(vb)); })

        switch (cigar[op_i].op) { 
            case BC_X    : SEQ_CONSUMED ; REF_CONSUMED ; SEQ_REF_CONSUMED ; COUNT(mismatch_bases_by_CIGAR); break ;
            case BC_E    : SEQ_CONSUMED ; REF_CONSUMED ; SEQ_REF_CONSUMED ; COUNT(match_bases_by_CIGAR);    break ;
            case BC_M    : SEQ_CONSUMED ; REF_CONSUMED ; SEQ_REF_CONSUMED       ; break ;
            case BC_I    : SEQ_CONSUMED ; COUNT(insertions)                     ; break ;
            case BC_D    : REF_CONSUMED ; COUNT(deletions)                      ; break ;
            case BC_N    : REF_CONSUMED ; vb->introns++                         ; break ;
            case BC_S    : VERIFY_S ; SEQ_CONSUMED ; COUNT(soft_clip[op_i > 0]) ; break ; // Note: a "121S" (just one op S or H) is considered a left-clip (eg as expected by sam_seg_bsseeker2_XG_Z_analyze)
            case BC_H    : VERIFY_H ; COUNT(hard_clip[op_i > 0])                ; break ;
            case BC_P    :                                                        break ;
            case BC_NONE : SEQ_CONSUMED; vb->binary_cigar.len = 0               ; break ; // PIZ: eg "151*" - CIGAR is "*" and SEQ/QUAL have length 151: seq_consumed will be updated to the length and binary_cigar will be empty

            default      : ASSINP (false, "%s: Invalid CIGAR: invalid operation %u", LN_NAME, cigar[op_i].op);
        }
    }          

    if (dl) { // ZIP
        dl->ref_consumed = vb->ref_consumed; // consumed by sam_seg_predict_TLEN 
        dl->seq_consumed = *seq_consumed;
        dl->hard_clip[0] = vb->hard_clip[0];
        dl->hard_clip[1] = vb->hard_clip[1];
    }

    // PIZ reconstructing: we store ref_consumed in ctx->cigar_anal_history because ctx->history is already taken for storing the CIGAR string
    else if (!vb->preprocessing) 
        *B(CigarAnalItem, CTX(SAM_CIGAR)->cigar_anal_history, vb->line_i) = (CigarAnalItem){
            .seq_len      = *seq_consumed,
            .ref_consumed = vb->ref_consumed,
            .hard_clip    = { vb->hard_clip[0], vb->hard_clip[1] }
        };

    // evidence of not being entirely unmapped: we have !FLAG.unmapped, RNAME, POS and CIGAR in at least one line
    if (segconf_running) {
        if (!dl->FLAG.unmapped && dl->POS && !IS_ASTERISK(vb->chrom_name)) {
            segconf.num_mapped++;
            segconf.sam_is_unmapped = false; 
        }

        if (vb->mismatch_bases_by_CIGAR || vb->match_bases_by_CIGAR)
            segconf.CIGAR_has_eqx = true;
    }

    ASSINP (!seq_consumed || *seq_consumed, "%s: Invalid CIGAR: CIGAR implies 0-length SEQ. CIGAR=\"%s\"", 
            LN_NAME, display_binary_cigar(vb));
}

bool sam_cigar_is_valid (STRp(cigar))
{
    uint32_t i=0;
    while (i < cigar_len) {

        uint32_t num_digits=0;
        for (; i < cigar_len && IS_DIGIT(cigar[i]) ; i++) num_digits++;

        if (!num_digits) return false;

        if (i == cigar_len || !cigar_valid_op[(int)cigar[i++]])
            return false;
    }
    return true;
}

// Squanking - removing the longest number from the CIGAR string if it can be recovered from elsewhere:
bool squank_seg (VBlockP vb, ContextP ctx, STRp(cigar), uint32_t only_if_seq_len/*0=always*/,
                 SeqLenSource seq_len_source, uint32_t add_bytes)
{
    START_TIMER;

    if (segconf_running) return false;
    
    int32_t n=-1, max_n=-1; // -1 to be careful: n=0 is not expected, but IS a valid number by the SAM/BAM spec
    uint32_t start_n=0, segment1_len=0, start_segment2=0, seq_len_plus_H=0;
    
    for (uint32_t i=0; i < cigar_len; i++) {
        char c = cigar[i];
        if (IS_DIGIT(c)) {
            if (n==-1) { // new number
                n = 0;
                start_n = i;
            }
            n = n*10 + c-'0';
        }

        else { // op
            if (M || I || S || E || X || H) { // note: we count H too 
                seq_len_plus_H += n;

                if (n > max_n) {
                    max_n          = n;
                    segment1_len   = start_n;
                    start_segment2 = i; 
                }
            }
            n = -1;
        }
    }

    // case: we can't squank
    if (only_if_seq_len && only_if_seq_len != seq_len_plus_H) {
        COPY_TIMER (squank_seg);
        return false;
    }

    buf_alloc (vb, &ctx->local, cigar_len+1, 0, char, CTX_GROWTH, CTX_TAG_LOCAL);
    char *next = BAFTc (ctx->local);
    
    if (segment1_len) 
        next = mempcpy (next, cigar, segment1_len);
    *next++ = 0;

    // if squanking cuts out an S value, we can also remove the 'S' op as we can deduce it
    if (cigar[start_segment2] == 'S') start_segment2++; 

    if (start_segment2 < cigar_len) 
        next = mempcpy (next, &cigar[start_segment2], cigar_len - start_segment2);
    *next++ = 0;

    ctx->local.len32 = BNUM(ctx->local, next);

    ctx->local_num_words++;

    if (ctx->did_i == SAM_CIGAR) // SAM_CIGAR field - go through CIGAR special first
        seg_special2 (VB, SAM_SPECIAL_CIGAR, SQUANK, seq_len_source, ctx, add_bytes);

    else // SA/XA/OA/MC CIGAR 
        seg_special1 (VB, SAM_SPECIAL_SQUANK, seq_len_source, ctx, add_bytes);
        
    COPY_TIMER (squank_seg);
    return true; // success
}

//---------
// SEG
//---------

void sam_seg_cigar_initialize (VBlockSAMP vb)
{
    // create an "all the same" node for SAM_FQ_AUX
    ctx_create_node (VB, SAM_FQ_AUX, (char[]){ SNIP_SPECIAL, SAM_SPECIAL_FASTQ_CONSUME_AUX }, 2);
}

// seg an arbitrary CIGAR string 
void sam_seg_other_CIGAR (VBlockSAMP vb, ContextP ctx, STRp(cigar), bool squanking_allowed, unsigned add_bytes)
{
    if (squanking_allowed && 
        cigar_len > MAX_CIGAR_LEN_IN_DICT && 
        squank_seg (VB, ctx, STRa(cigar), DATA_LINE(vb->line_i)->SEQ.len + vb->hard_clip[0] + vb->hard_clip[1], SQUANK_BY_MAIN, add_bytes))
        {} // squank succeeded - nothing to do

    // complicated CIGARs are better off in local - anything more than eg 112M39S 
    // note: we set no_stons=true in sam_seg_initialize so we can use local for this rather than singletons
    else if (cigar_len > MAX_CIGAR_LEN_IN_DICT)
        seg_add_to_local_string (VB, ctx, STRa(cigar), LOOKUP_SIMPLE, add_bytes);
 
    // short CIGAR
    else 
        seg_by_ctx (VB, STRa(cigar), ctx, add_bytes);
}

// used for XA, OA, SA, and also CIGAR field in PRIM VBs
bool sam_seg_0A_cigar_cb (VBlockP vb, ContextP ctx, STRp (cigar), uint32_t repeat)
{
    // note: -1==prim cigar - cannot squank as it is used to determine seq_len
    sam_seg_other_CIGAR (VB_SAM, ctx, STRa(cigar), repeat != (uint32_t)-1, cigar_len);

    // bug 1147: update compressed size so that load (in PIZ) can allocate PRECISE memory
    // note: we have no singletons because ltype=LT_STRING so that's not an issue
    // if (ctx->did_i == OPTION_SA_CIGAR && cigar_lan > MAX_CIGAR_LEN_IN_DICT && IS_PRIM(vb))
    //     VB_SAM->comp_cigars_len += nico_compressed_len_textual_cigar (vb, OPTION_SA_CIGAR, STRa(cigar)); 

    return true;
}

// abbreviate a CIGAR string to [S][M][ID][S] following the minimap2 logic in: https://github.com/lh3/minimap2/blob/master/format.c : mm_write_sam3
// pbmm2 example (abbrev with I):    "9927S40=1I33=1D24=1D6=1D6=1D60=5I17=1I19=5I13=5I39=1I55=1I12=1I18=1D47=5S" ➤ "9927S394M15I5S"
// pbmm2 example (abbrev with D):    "7675S5=1D5=1X5=1D5=1X11=1D32=1X5=1I3=1X12=1I27=1D9=1I12=1X5=5D1=1X5=5D1=1X5=5D1=1X51=1D6=1D5=1D12=1D18=1X6=1X6=1X6=1D10=2I5=2I4=1I5=1I5=2I12=1I6=1I6=1I6=1I6=1I6=1X5=1X5=1D3=1X49=1D30=1I6=1I47=1I12=1I59=1X5=1X5=1X11=1X35=1X5=1X5=1X5=1X5=1X5=1X5=1X5=1X5=1X5=1X5=1X4=1961S" ➤ "7675S705M6D1961S"
// winnowmap example (CIGAR with H): "34615H15M2D24M2D1M1D6M3I10M2D9M1D22M1D18M1D16M3I6M1I8M1D9M1I4M1D27M1I23M1D7M1D3M1I2M4D12M2I38M1D3M2I4M6D6M2D2M1D5M6D3M5I23M2D2M1D6M1I9M2I16M1D10M2D24M4I2M1I3M2I1M1I16M3I5M3I10M1I1M2D5M3I6M1I3M1D2M4I3M3D5M2I1M1I6M1D13M6D5M2D9M2I2M2I21M2D11M4I3M1I2M5I2M2I9M3D3M1I4M1I18M1D7M1D16M1I14M1I19M1I11M1D15M2I1M3D2M2D4M1D13M2D20M1D2M1D6M2D21M5I7M1I3M1I13M4I1M1D8M1D21M1D11M1I39M2D10M1I4M3D9M2I13M1I8M3I10M1D20M1I10M1D14M1D5M2I5M1I12M1D16M1D5M1D14M4D17M1I14M1D17M1I4M1D49M2D2M2D1M1D13M2D13M1I26M1D2M3I1M1I6M5D16M1I9M1I3M1I7M1D6M3D26M1I7M1I14M1I1M2D9M1D5M2D6M1D9M1I27M1D7M1I7M2I3M1D20M1D9M1D4M5I12M1D5M4D6M1D4M1D10M1I2M1D2M2D23M2I1M1D35M1D10M1D9M2I23M1D4M4D3M1D8M1I5M3D11M1I22M3I20M1I8M2D19M1I8M1D4M2D19M2D6M2I4M2I21M2D12M1I13M2I17M2I4M1D3M1D8M1D26M1I2M1D12M1D1M2I5M1D7M1D10M1D7M2D1M1D3M3I1M1D13M1D9M4D5M2D9M2I14M1I15M2D6M1D14M2I4M1D10M2I2M3D5M1I9M3D3M1D8M1I10M1I3M1I1M1I5M2D7M1D4M1I3M2D4M1D17M4D1M3D6M2I3M2D6M1D3M2D7M1D4M1D6M2D11M1I26M1D1M1D8M1D2M1D7M1D6M3D1M1D12M1I2M2D3M2D15M2D6M3I2M2D12M1I13M1D7M1I1M1I9M1D1M2D5M2I5M1D1M4I11M1D13M1I15M2D4M1I3M2I4M1I3M1D9M1D8M1I18M1I6M1I2M1D7M5D13M1I5M2I4M4I1M2I7M4I8M5I3M3D1M1D8M1D14M6I32M1I3M2I7M2I6M2I3M2D3M1D9M2I1M1I4M1I4M2I15M2D2M1I3M2I21M3I3M1D5M1I7M2I5M1I6M1I8M3I4M2D8M1I8M2D7M3I21M1I5M1I16M3D12M1I6M5I6M1D12M1I13M1D8M2I7M4D4M1I9M1D3M1I18M1I27M1D12M2I9M1D4M1D40M2I22M1I1M1I14M4D8M1D10M1D6M1I6M1I7M1D3M1D4M1D1M6I2M1I8M2I3M2I4M1D7M2D4M1D5M1I5M1I15M4I2M2D8M2D11M1I5M2D1M1D6M1D5M1D20M1I9M2D5M1I9M2I22M4D13M1I29M2D17M2D5M1I16M4I1M2D5M2I6M2D3M9D8M1D1M3D11M2I5M1I4M1I16M2I20M1I19M4I3M1I6M1I9M2D11M2I3M1I6M1I2M1I9M1D4M1D2M2I2M2D5M1I6M1D7M1I8M3I1M1D4M2I3M1D13M1D22M5I12M2I5M1D11M1D14M1D2M2I7M4I5M1I4M3I4M2I14M1I33M6I5M2D2M3D5M1I10M1I8M2D2M1I12M3I5M2I3M1I3M2I4M3I7M1D12M2D6M1I28M2D9M1D4M2I1M4I2M2I3M3I5M2I4M1I2M2I3M1I6M3I2M1I11M1I15M6I3M1I9M1I5M2I1M1I4M1I4M1I29M1I4M4I2M1D5M2I15M1D3M1I2M1I5M1D9M2I13M1D5M1D1M3D3M1D12M2D8M1I19M1D11M1D12M1I4M2I5M1I6M5D1M1D11M5D5M2D26M1D3M1D16M3I2M1D6M1D7M2D14M1D14M4D11M2D9M1I7M1I8M2I32M5I4M1I7M3D7M4D16M2D13M7I2M1I4M1I43M2I3M3D1M2D13M2D3M1D10M1I7M1I2M1D18M1I20M2I3M1D3M1I2M1I11M1D12M2D2M1D4M1I18M2D9M1D6M1D20M1D14M1D9M1I5M1D24M1D3M1I4M1D20M1D7M1I40M1D4M4I3M1D7M3D19M2D4M3I5M2D5M1D1M1D4M1I9M1D9M1I6M1D4M1I4M1I3M1I5M5I10M1I9M1D14M5I6M3I18M1D10M2D6M5I5M2I7M1D6M7I4M2D3M2D7M1I8M3I2M4I5M1I6M6D10M2I4M1D6M4I8M1D4M1D9M1I4M1I5M2D5M2I11M1I10M4D5M2I2M1D11M3I4M7D13M6D1M1D3M1D5M1D3M1D3M4D5M1I1M1I6M3I3M3D8M3I9M5I5M2I1M1I2M2I2M2I6M3I2M1D10M3I2M3I13M7I5M2I16M2D4M1I19M2D7M2D3M2I4M1D10M3I2M1I4M4D7M3D6M3D3M1D4M2D8M1I2M3D1M1D13M1D11M3I1M1D12M1D4M1I27M1I5M3I5M1I1M1I15M3I6M4I7M4D5M1D4M1I20M4D10M2I4M1D3M2I2M1I17M1D2M3I10M2I4M4D2M1I12M4I9M1D15M1I5M2D6M1I2M2D6M1I6M1D9M6D8M1I9M1I8M2D3M2D1M1D5M1I9M1I17M5D15M2D8M1D6M1D5M1I4M4D6M3I1M1I16M2I6M1D4M2D3M1I3M1D1M1D9M1I3M1I6M2I4M1D6M1D3M1D1M2I6M1D2M1I12M1D3M2D1M1D1M4D2M2D4M2D3M2D11M2D8M1D13M3D4M1D2M1I1M2I11M2I15M3D4M8D9M1I2M3I3M3I3M1I3M2D7M1I10M2D2M1I7M1I5M1I12M3I5M1D7M1I2M2I8M1I5M1D6M2D16M1I3M1D24M1I2M2I4M2D22M1D22M1I7M3I8M2I5M2D9M1I7M2D10M2D1M2D2M2D11M2D2M1I4M1D2M1I12M2I6M2I3M1D28M1D35M2I3M2D10M1I35M5I1M1I6M4I6M2I11M1I6M2I20M1I12M1I5M1I2M1I7M1I6M1D2M1D2M3D3M1D1M1D8M1D10M1I6M1D2M1D10M1I6M2D1M1D14M1I8M2D14M1I3M1D9M3D9M2D5M1D5M1D11M1D4M1D3M1D3M1D5M1D11M5D8M5D1M1D12M3D9M1I1M1I4M1D2M2D7M1D9M3D11M2I6M1D3M1I3M41D2M1D3M6D10M1I6M1D4M1D4M1D2M1I13M2I1M1I14M1D4M3I3M1I2M3D2M1D11M1I4M2D8M2D4M1I16M1D11M5I2M2I7M1I6M1D9M1D7M2D10M1D9M1D23M2I4M2I7M1I5M2D8M1D4M2D15M1I2M3D4M3I6M1I7M2D8M3D38M1I17M1D3M1D2M1D5M2D3M1D18M1D4M1I7M5I6M2I3M1D15M2I7M3D5M2D9M1I9M1D3M1I34M3D12M3D5M1D13M1D10M2D5M2I3M3D10M2I16M1I5M1I6M1D3M6I1M1D7M1I9M1I17M6I20M2D11M4I5M2D8M3D2M2I5M3I3M1D6M1I3M1D8M1I5M4D10M1D3M1D8M1D11M2I22M1I1M3D12M3D18M1D7M2I2M4D5M1D13M7D2M3D18M8I4M3I2M1I7M1D4M1D13M1I3M2I38M1D25M2D6M1I25M4I23M1I2M2D21M2D1M3D18M2I38M1D11M1D14M2I5M1D8M1D3M1I5M1D5M1I11M2I4M1I3M1D24M3D5M3D13M3D10M2D11M1D3M1D3M1I7M1D5M1D8M2I3M3D2M3D6M2I12M1I10M2I17M3D7M1I5M1I10M1I2M3D14M1D3M2I6M1I13M1D6M1I7M1I7M1I11M1I5M1D4M1I3M1D11M2I6M1D2M3D4M1I8M4D3M1D8M1D14M1D18M1I4M4D13M1D4M3D1M1D7M3D15M1D8M1D15M1D2M2D22M1D8M1I2M1I5M1D6M1I3M1D1M1D6M2I14M1D6M6D1M1D11M4I6M2D4M1D11M1I16M2D9M2I1M1D7M1D5M1D5M1I7M2D4M1I10M1D44M1D10M3D9M1I2M4D11M2D17M1D4M2I9M2I10M2D3M2D6M1I6M1D6M1I7M2I3M1D3M2D4M1I12M1I4M1D15M2I6M2D19M1I15M1I4M3I17M1D6M1I2M1I10M1D6M2D7M1I2M1I13M1D10M4I7M4I1M2D12M1I12M3I3M4I5M9I1M5I8M1D1M1D17M1I29M1D7M1D4M1I8M1D5M5D2M1I5M2D10M1D7M1I2M1I25M1D7M1I2M1I3M1I16M7I2M2I7M1D4M4I9M2I5M1I4M3D6M1D13M2I23M1D6M1D4M2I7M1D6M4D5M1I1M3I8M1I2M1D9M6D3M1D3M2D29M2I4M3I10M1D21M4D5M2D9M2D16M1D10M1D1M1D10M2D4M1I4M2I13M1I5M1D10M3I7M1D1M2D5M1D1M1D21M2D15M1D6M1D6M2I4M3D14M2D5M1D6M3I4M4I1M1I3M1D32M1D19M5D2M1I3M1D16M6I7M1D14M1I6M1I6M1D3M1D3M2D7M2D15M4I2M1I3M6D5M6I3M1I5M1D7M1I8M1D33M2D7M2D12M2I35M3I24M3D8M1D3M1D4M2D15M2I12M1D11M3I15M4I10M2D8M1D10M2D20M4D3M1D1M1D11M5D15M1I8M1D26M1I1M1I9M2D24M2D10M2I4M3D3M2I10M1D1M1D16M4I12M2I37M1I4M1D9M2D9M3I3M3I3M2I5M1D2M1D12M3D12M4D7M1I11M1I14M1D1M1D5M1D11M2D28M3D4M1I7M1D1M2I2M1D7M1I12M1D2M3I7M6I1M1D15M3I10M1D2M1D18M2D4M2D4M2D6M1I20M1D3M1D2M2I8M1D7M2D24M2D2M1D4M2D11M1D1M1D10M1I11M2D2M3D15M2I4M1I2M1I6M2I11M3I5M3D3M1D10M2I8M1I14M2I2M1I6M1I6M2D4M1D3M2I6M2D10M3I16M1I5M2D5M1I3M1D3M1D9M2I10M1D4M2I8M1I1M4I15M1I5M1D6M2D3M1I10M2D5M1D4M1D21M1I9M2I2M1D6M1I1M3D4M1D12M1D4M1I5M1I20M1D3M1D9M3I4M2D4M2D1M1D5M1D7M2I5M1D3M1D19M2D15M2I4M1I7M3D6M1D12M1I5M1I3M2I9M1D3M1D14M1D4M3D13M1I2M5I3M1D20M2I1M1I4M1I6M3I6M5I6M2I4M3D12M3D8M1I5M1I7M4D25M2D1M2D11M1D5M2D20M1D69M1I4M1D14M1D3M1D5M1D10M1D5M2D9M1D22M2I3M3D6M1I9M3I2M2I14M2D3M1D2M2I2M1D11M1D11M1I9M1D15M1I6M1D6M1D22M1D17M2I2M2I7M1I14M3I11M4D2M1I13M1I8M1I9M1I2M3D3M1D6M1I3M1D9M2I12M2D1M1I9M1I5M4D8M2I19M1D4M1I19M3D15M1I10M1D12M1I10M1D13M1I3M2D20M1I28M2D17M1I9M5I3M7I4M5D2M1D26M1I7M1I11M1D3M1D26M1D16M1D15M1I3M1I12M1D5M1I7M4D3M1I7M1D12M1D7M1D8M3D3M1I10M1I10M2I7M1D25M1I1M1I4M2D27M1D2M1D14M1I29M1D7M1D11M1I30M1D1M1D10M2D3M4I2M1I5M1D19M1I8M1D4M3I12M1D10M1I3M1I1M3I21M1I4M2D12M1D14M4I21M1I3M2D5M2D7M4D2M4D14M4D6M1I9M1I13M1I1M2I10M1I4M2I10M1I1M1I5M1D6M7D8M3I1M1D2M2I5M1D10M2D26M2I9M6D11M3I13M2D16M1D50M1I3M2D7M2I3M4D27M1I9M1I12M2I1M3D11M1I3M2I16M2D3M1D14M1D7M1D4M1I3M1I4M1I1M1I10M1D15M2D12M1D22M3D7M1D6M1D6M1I7M1I2M4I3M2D7M1D13M2D15M2D9M1D4M3I6M1I4M1D14M1D5M2D5M1I5M3I2M1D24M2D25M1D6M3D4M1D5M1D5M2I1M1I9M1D9M4D2M3D1M1D5M3I8M3I6M3I3M4I2M1I12M1D4M2D11M1I4M1I4M2D6M3D2M1I6M2D7M2D3M1I17M1D4M1D11M1I3M2D4M1D25M1I4M1D3M1D9M2I2M1D6M1D12M1I10M40010H" ➤ "34615S13273M138D40010S"
static void cigar_abbreviate (pSTRp(cigar), uint32_t seq_len, uint32_t ref_consumed, uint32_t soft_clip[2], uint32_t hard_clip[2], StrText *abbrev_cigar)
{
    for (int i=0; i < 2; i++)
        ASSERT (!soft_clip[i] || !hard_clip[i], "only one of soft_clip[%u]=%u or hard_clip[%u]=%u can be set", i, soft_clip[i], i, hard_clip[i]);
    
    // following the logic in https://github.com/lh3/minimap2/blob/master/format.c : mm_write_sam3
    uint32_t l_q = seq_len - soft_clip[0] - soft_clip[1];
    uint32_t l_D=0, l_I=0, l_M=0;
    if (l_q < ref_consumed) l_M = l_q, l_D = ref_consumed - l_M;
    else                    l_M = ref_consumed, l_I = l_q - l_M;
    uint32_t cl5 = soft_clip[0] ? soft_clip[0] : hard_clip[0]; 
    uint32_t cl3 = soft_clip[1] ? soft_clip[1] : hard_clip[1];
    
    *cigar_len = snprintf (abbrev_cigar->s, sizeof (*abbrev_cigar), "%s%s%s%s%s%s%s%s%s%s",
        cond_int (cl5, "", cl5), cl5 ? "S" : "",
        cond_int (l_M, "", l_M), l_M ? "M" : "",
        cond_int (l_I, "", l_I), l_I ? "I" : "",
        cond_int (l_D, "", l_D), l_D ? "D" : "",
        cond_int (cl3, "", cl3), cl3 ? "S" : "");

    *cigar = abbrev_cigar->s;
}

// seg the prim CIGAR that in piz, will be loaded into the SAG and used to reconstructed the SA:Z field in the depn lines
static void sam_cigar_seg_prim_cigar (VBlockSAMP vb, STRp(textual_cigar))
{
    ContextP sa_cigar_ctx = CTX(OPTION_SA_CIGAR);

    sam_seg_0A_cigar_cb (VB, sa_cigar_ctx, STRa(textual_cigar), (uint32_t)-1 /*-1=prim cigar*/);
    sa_cigar_ctx->txt_len      -= textual_cigar_len; // remove "add_bytes" - already accounted for in SAM_CIGAR
    sa_cigar_ctx->counts.count += textual_cigar_len; // count CIGAR field contribution to OPTION_SA_CIGAR, so sam_stats_reallocate can allocate the z_data between CIGAR and SA:Z
}

// tests if textual cigar is in same-vb prim's SA:Z, and that it is in the predicted position within SA:Z
static bool sam_cigar_seg_is_predicted_by_saggy_SA (VBlockSAMP vb, STRp(textual_cigar))
{
    bool is_same = false;
    HtoS htos = {};

    // prediction: our alignment matches the SA:Z item of the primary, in the position
    // which the diffence between our line_i and the primary's
    STR(prim_cigar);
    if (!sam_seg_SA_get_prim_item (vb, SA_CIGAR, pSTRa(prim_cigar))) return false;
    
    // if CIGAR has hard-clips, determine if this is HtoS (i.e. they are converted to soft-clips in SA_CIGAR), it not already known
    if (segconf.SA_HtoS == unknown && (vb->hard_clip[0] || vb->hard_clip[1])) {
        bool has_htos = false;

        if (!str_issame (textual_cigar, prim_cigar)) {
            htos = sam_cigar_H_to_S (vb, (char*)STRa(textual_cigar), false);
            if (str_issame (textual_cigar, prim_cigar)) 
                has_htos = true; // changing H to S indeed made them the same
            else
                goto done; // it is not the same whether or not we HtoS. 
        }

        // at this point we know that the CIGARs match, and we need to update segconf, if not already
        // updated by another thread. A pathological case can occur in which a file has SA:Z with and without
        // HtoS and another threads sets it the "wrong" way for us. In that case, we simply keep is_same=false.
        thool expected = unknown;
        if (__atomic_compare_exchange_n (&segconf.SA_HtoS, &expected, has_htos, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            is_same = true;

        goto done;
    }

    if (segconf.SA_HtoS == yes)
        htos = sam_cigar_H_to_S (vb, (char*)STRa(textual_cigar), false);

    is_same = str_issame (textual_cigar, prim_cigar);
    
done:
    sam_cigar_restore_H (htos);
    return is_same;
}

static void sam_cigar_update_random_access (VBlockSAMP vb, ZipDataLineSAMP dl)
{
    if (segconf.disable_random_acccess || IS_ASTERISK (vb->chrom_name) || dl->POS <= 0) return;

    PosType32 last_pos = dl->POS + vb->ref_consumed - 1;

    if (IS_REF_INTERNAL && last_pos >= 1)
        random_access_update_last_pos (VB, last_pos);

    else { // external ref
        WordIndex ref_index = chrom_2ref_seg_get (VB, vb->chrom_node_index); 
        if (ref_index == WORD_INDEX_NONE) return; // not in reference

        PosType64 LN = ref_contigs_get_contig_length (ref_index, 0, 0, false); // -1 if no ref_index

        if (LN == -1) {}
            
        else if (IN_RANGX (last_pos, 1, LN))
            random_access_update_last_pos (VB, last_pos);
        
        else  // we circled back to the beginning for the chromosome - i.e. this VB RA is the entire chromosome
            random_access_update_to_entire_chrom (VB, 1, LN); 
    }
}

void sam_seg_CIGAR (VBlockSAMP vb, ZipDataLineSAMP dl, uint32_t last_cigar_len, STRp(seq_data), STRp(qual_data), uint32_t add_bytes)
{
    START_TIMER
    
    decl_ctx (SAM_CIGAR);
    ContextP seq_len_ctx;
    
    bool seq_is_available = !IS_ASTERISK (seq_data);

    ASSSEG (!(seq_is_available && *seq_data=='*'), "seq_data=%.*s (seq_len=%u), but expecting a missing seq to be \"*\" only (1 character)", 
            seq_data_len, seq_data, seq_data_len);

    char cigar_snip[last_cigar_len + 50];
    cigar_snip[0] = SNIP_SPECIAL;
    cigar_snip[1] = SAM_SPECIAL_CIGAR;
    uint32_t cigar_snip_len=2;

    // case: SEQ is "*" - we add a '-' to the CIGAR
    if (!seq_is_available) cigar_snip[cigar_snip_len++] = '-';

    // case: CIGAR is "*" - we get the dl->SEQ.len directly from SEQ or QUAL, and add the length to CIGAR eg "151*"
    if (!dl->SEQ.len) { // CIGAR is not available
        ASSSEG (!seq_data_len || vb->qual_missing || seq_data_len==dl->QUAL.len,
                "Bad line: SEQ length is %u, QUAL length is %u, unexpectedly differ. SEQ=%.*s QUAL=%.*s", 
                seq_data_len, dl->QUAL.len, seq_data_len, seq_data, dl->QUAL.len, qual_data);    

        dl->SEQ.len = MAX_(seq_data_len, dl->QUAL.len); // one or both might be not available and hence =1

        // test if we can predict SEQ.len from QNAME's seq_len
        if (segconf.seq_len_dict_id.num) {
            ContextP qname_seq_len_ctx = ECTX(segconf.seq_len_dict_id);

            if (ctx_has_value_in_line_(VB, qname_seq_len_ctx) && dl->SEQ.len == qname_seq_len_ctx->last_value.i)
                cigar_snip[cigar_snip_len++] = COPY_QNAME_LENGTH_NO_CIGAR;
            else 
                goto no_qname_seq_len;
        }

        else no_qname_seq_len:
            cigar_snip_len += str_int (dl->SEQ.len, &cigar_snip[cigar_snip_len]);
    } 
    else { // CIGAR is available - just check the seq and qual lengths
        ASSSEG (!seq_is_available || seq_data_len == dl->SEQ.len,
                "Bad line: according to CIGAR, expecting SEQ length to be %u but it is %u. SEQ=%.*s", 
                dl->SEQ.len, seq_data_len, seq_data_len, seq_data);

        ASSSEG (vb->qual_missing || qual_data_len == dl->SEQ.len,
                "Bad line: according to CIGAR, expecting QUAL length to be %u but it is %u. QUAL=%.*s", 
                dl->SEQ.len, dl->QUAL.len, dl->QUAL.len, qual_data);    
    }

    // store the CIGAR in DataLine for use by a mate MC:Z and SA:Z
    if (!IS_BAM_ZIP) // SAM
        dl->CIGAR = (TxtWord){ .index = BNUMtxt (vb->last_cigar), .len = last_cigar_len }; // in SAM (but not BAM) vb->last_cigar points into txt_data

    else if (line_textual_cigars_used) { // BAM
        dl->CIGAR =(TxtWord){ .index = vb->line_textual_cigars.len32, .len = vb->textual_cigar.len32 }; // in BAM dl->CIGAR points into line_textual_cigars
        buf_append_buf (VB, &vb->line_textual_cigars, &vb->textual_cigar, char, "line_textual_cigars");
    }

    // case: DEPN or PRIM line.
    // Note: in DEPN, cigar already verified in sam_sa_seg_depn_find_sagroup to be the same as in SA alignment
    // Note: in DEPN, if cigar has soft/hard clips, we can only seg this cigar against against sag if soft/hard clips is consistent wtih SA_HtoS
    // Note: in minimap2 and its derivatives SA_CIGAR abbreviated. Therefore:
    // - In PRIM, we store the full unabbreviated CIGAR in SA_CIGAR and abbreviate it in PIZ when reconstructing
    //   SA:Z of the depn lines. The full CIGAR is needed to reconstruct SEQ from the reference during loading.
    // - In DEPN, we seg the unabbreviated CIGAR directly into SAM_CIGAR (i.e. not copying it from SAG) 
     if (sam_seg_has_sag_by_SA (vb) && 
        !(IS_DEPN(vb) && segconf.SA_CIGAR_abbreviated == yes) &&
        !(IS_DEPN(vb) && segconf.SA_HtoS==yes && (vb->soft_clip[0] || vb->soft_clip[1])) && 
        !(IS_DEPN(vb) && segconf.SA_HtoS==no  && (vb->hard_clip[0] || vb->hard_clip[1]))) {

        sam_seg_against_sa_group (vb, ctx, add_bytes); 

        // in PRIM, we also seg it as the first SA alignment (used for PIZ to load alignments to memory, not used for reconstructing SA)
        if (IS_PRIM(vb)) 
            sam_cigar_seg_prim_cigar (vb, vb->last_cigar, last_cigar_len);
    }

    // case: copy from "length=" item of QNAME (only if CIGAR is a simple M)
    else if (segconf.seq_len_dict_id.num                                                   && // QNAME flavor has "length=""
             vb->binary_cigar.len32 == 1 && B1ST(BamCigarOp, vb->binary_cigar)->op == BC_M && // this CIGAR is a single-op M
             ctx_has_value_in_line (vb, segconf.seq_len_dict_id, &seq_len_ctx)) {              // note: if copied from buddy, value is set in sam_seg_QNAME

        // note: often, length= indicates the FASTQ read length, which may be longer than in SAM due to cropping and trimming
        cigar_snip[2] = COPY_QNAME_LENGTH;
        int32_t n = B1ST(BamCigarOp, vb->binary_cigar)->n;
        cigar_snip_len = 3 + (n == seq_len_ctx->last_value.i ? 0 : str_int (seq_len_ctx->last_value.i - B1ST(BamCigarOp, vb->binary_cigar)->n, &cigar_snip[3]));
        seg_by_ctx (VB, STRa(cigar_snip), ctx, add_bytes);
    }

    // case: we mate non-trival CIGARs with MC:Z. We don't mate eg "151M" bc this will add rather than reduce entropy in b250
    else if (last_cigar_len > 4 && sam_has_mate && segconf.has[OPTION_MC_Z] && !segconf_running && 
             cigar_snip_len == 2 && // we don't mate if CIGAR or SEQ are "*"
             str_issame_(vb->last_cigar, last_cigar_len, STRtxt(DATA_LINE (vb->mate_line_i)->MC))) {

        cigar_snip[cigar_snip_len++] = COPY_MATE_MC_Z; // always at cigar_snip[2]        
        seg_by_did (VB, STRa(cigar_snip), SAM_CIGAR, add_bytes); 
    }

    // case: copy from same-vb prim (note: saggy_line_i can only be set in the MAIN component)
    else if (has(SA_Z) && segconf.sam_has_SA_Z && sam_has_prim && sam_line_is_depn (dl) && sam_cigar_seg_is_predicted_by_saggy_SA (vb, vb->last_cigar, last_cigar_len)) {
        cigar_snip[cigar_snip_len++] = COPY_SAGGY_PRIM_SA_CIGAR; // always at cigar_snip[2]
        seg_by_did (VB, STRa(cigar_snip), SAM_CIGAR, add_bytes); 
    }

    // case: long CIGAR and SEQ and CIGAR are not missing, with the segconf.std_seq_len (normally only works for short reads)
    else if (last_cigar_len > MAX_CIGAR_LEN_IN_DICT && cigar_snip_len == 2 && 
             dl->SEQ.len + vb->hard_clip[0] + vb->hard_clip[1] == segconf.std_seq_len)
        squank_seg (VB, ctx, vb->last_cigar, last_cigar_len, 0/*always*/, SQUANK_BY_std_seq_len, add_bytes); 
    
    // case: long CIGAR, and length can be deduced from qname length= (not simple cigar) (15.0.69)
    else if (last_cigar_len > MAX_CIGAR_LEN_IN_DICT && cigar_snip_len == 2 && 
             segconf.seq_len_dict_id.num &&
             ctx_has_value_in_line (vb, segconf.seq_len_dict_id, &seq_len_ctx) && // note: if copied from buddy, value is set in sam_seg_QNAME
             dl->SEQ.len + vb->hard_clip[0] + vb->hard_clip[1] == seq_len_ctx->last_value.i)
        squank_seg (VB, ctx, vb->last_cigar, last_cigar_len, 0/*always*/, SQUANK_BY_QNAME_length, add_bytes); 

    // case: long CIGAR and SEQ or CIGAR are missing or short CIGAR
    else { 
        memcpy (&cigar_snip[cigar_snip_len], vb->last_cigar, last_cigar_len);
        
        cigar_snip_len += last_cigar_len;

        if (last_cigar_len > MAX_CIGAR_LEN_IN_DICT) 
            seg_add_to_local_string (VB, ctx, STRa(cigar_snip), LOOKUP_SIMPLE, add_bytes);

        else 
            seg_by_ctx (VB, STRa(cigar_snip), ctx, add_bytes); 
    }
    
    if (segconf_running) 
        segconf.sam_cigar_len += last_cigar_len;

    else if (dl->POS >= 1 && vb->ref_consumed)
        sam_cigar_update_random_access (vb, dl);

    // chew cigars for piz deep in-memory compression. we do it in vb=1 rather than segconf to get more data.
    if (flag.deep && vb->vblock_i == 1 && !(dl->FLAG.value & (SAM_FLAG_SUPPLEMENTARY | SAM_FLAG_SECONDARY | SAM_FLAG_UNMAPPED))) {
        sam_prepare_deep_cigar (VB, CIG(vb->binary_cigar), false); // note: no need to revesre - we are just collecting stats for nico
        nico_chew_one_cigar (SAM_CIGAR, CIG(ctx->deep_cigar));
        buf_free (ctx->deep_cigar);
    }

    COPY_TIMER(sam_cigar_seg);
}

uint32_t sam_cigar_get_MC_ref_consumed (STRp(mc))
{
    // get ref_and_seq_consumed
    uint32_t n=0;
    uint32_t ref_and_seq_consumed=0;
    for (uint32_t i=0; i < mc_len; i++) {

        char c = mc[i];
        char lookup = cigar_lookup_sam[(uint8_t)c];
        if (!lookup) return 0; // invalid CIGAR - unrecognized character

        lookup &= 0x0f; // remove validity bit

        if (lookup == CIGAR_DIGIT) 
            n = n*10 + (c - '0');
        
        else {
            if (!n) return 0; // invalid CIGAR - no number before op

            if ((lookup & CIGAR_CONSUMES_REFERENCE)) 
                ref_and_seq_consumed += n;

            n=0;
        }
    }
    return ref_and_seq_consumed;
}

// MC:Z "CIGAR string for mate/next segment" (https://samtools.github.io/hts-specs/SAMtags.pdf)
void sam_cigar_seg_MC_Z (VBlockSAMP vb, ZipDataLineSAMP dl, STRp(mc), uint32_t add_bytes)
{
    ZipDataLineSAMP mate_dl = DATA_LINE (vb->mate_line_i); // an invalid pointer if mate_line_i is -1

    ContextP channel_ctx = seg_mux_get_channel_ctx (VB, OPTION_MC_Z, (MultiplexerP)&vb->mux_MC, sam_has_mate);

    if (sam_has_mate && 
        (!IS_BAM_ZIP || line_textual_cigars_used) && // there might be a rare edge case there are no MC:Z lines in the segconf vb, but are after - in which case, in depn/prim VBs, we won't have line_textual_cigars
        str_issame_(STRacigar(mate_dl), STRa(mc)))
        seg_by_ctx (VB, STRa(copy_mate_CIGAR_snip), channel_ctx, add_bytes); // copy MC from earlier-line mate CIGAR
    
    // case: long CIGAR and SEQ and CIGAR are not missing, with the "standard" sam_seq_len (normally only works for short reads)
    else if (mc_len > MAX_CIGAR_LEN_IN_DICT && 
            squank_seg (VB, channel_ctx, STRa(mc), segconf.std_seq_len, SQUANK_BY_std_seq_len, add_bytes)) {}

    else if (mc_len > MAX_CIGAR_LEN_IN_DICT) 
        seg_add_to_local_string (VB, channel_ctx, STRa(mc), LOOKUP_SIMPLE, add_bytes);

    else 
        seg_by_ctx (VB, STRa(mc), channel_ctx, add_bytes);    

    dl->MC = TXTWORD(mc); 

    seg_by_did (VB, STRa(vb->mux_MC.snip), OPTION_MC_Z, 0); // de-multiplexer

    ctx_set_last_value (VB, CTX(OPTION_MC_Z), (ValueType){ .i = sam_cigar_get_MC_ref_consumed (STRa(mc)) } );
}

//---------
// PIZ
//---------

static uint32_t inline sam_cigar_piz_get_seq_from_qname (VBlockSAMP vb)
{
    ContextP len_ctx = ECTX(segconf.seq_len_dict_id);

    // case 1: QNAME/len_ctx was reconstructed in this VB - then last_value was set normally
    // case 2: QNAME/len_ctx was reconstructed when loading PRIM, then copyied to the sag in
    //         sam_load_groups_add_qname, and finally copied back into len_ctx.last_value before
    //         reconstructing CIGAR in 1. sam_load_groups_add_grps for loading CIGAR into the sag
    //         and in 2. sam_piz_special_PRIM_QNAME ahead of reconstructing the line.

    if (!VER(15) || ctx_has_value_in_line_(vb, len_ctx)) 
        return len_ctx->last_value.i;

    // case 3: QNAME is copied from a buddy - buddy_line_i is stored in QNAME.last_value.i in sam_piz_special_COPY_BUDDY
    else if (ctx_has_value_in_line_(vb, CTX(SAM_QNAME))) {
        LineIType buddy_line_i = CTX(SAM_QNAME)->last_value.i;
        return *B64(len_ctx->history, buddy_line_i);
    }
    
    else 
        ABORT_PIZ ("len_ctx=%s has no value in line", len_ctx->tag_name);
}

// CIGAR - calculate vb->seq_len from the CIGAR string, and if original CIGAR was "*" - recover it
SPECIAL_RECONSTRUCTOR_DT (sam_cigar_special_CIGAR)
{
    START_TIMER;

    VBlockSAMP vb = (VBlockSAMP)vb_;
    StoH stoh = {};

    switch (snip[0]) {
        case COPY_MATE_MC_Z: // copy the snip from mate MC:Z
            sam_reconstruct_from_buddy_get_textual_snip (vb, CTX (OPTION_MC_Z), BUDDY_MATE, pSTRa(snip));
            ASSPIZ0 (snip_len, "Unable to find buddy MC:Z in history");
            break;

        case COPY_SAGGY_PRIM_SA_CIGAR: // copy the predicted alignment in same-vb prim line's SA:Z
            sam_piz_SA_get_saggy_prim_item (vb, SA_CIGAR, pSTRa(snip));
            stoh = segconf.SA_HtoS ? sam_cigar_S_to_H (vb, (char*)STRa(snip), false) : (StoH){};
            break;
    
        case SQUANK:
            cigar_special_SQUANK (VB, ctx, (char[]){ snip_len > 1 ? snip[1]/*since 15.0.69*/ : SQUANK_BY_std_seq_len }, 1, NULL/*reconstruct to vb->scratch*/, true); 
            snip     = vb->scratch.data;
            snip_len = vb->scratch.len;
            break;

        case COPY_QNAME_LENGTH: // copy from QNAME item with "length=" or range
        case COPY_QNAME_LENGTH_NO_CIGAR: // 15.0.26
            buf_alloc (vb, &vb->scratch, 0, 16, char, 0, "scratch");

            int32_t delta = (snip_len > 1) ? atoi (&snip[1]) : 0; // introduced 15.0.69 - value if delta != 0
            int32_t M_n = sam_cigar_piz_get_seq_from_qname (vb) - delta;
            
            vb->scratch.len32 = str_int (M_n, B1STc (vb->scratch));
            BNXTc (vb->scratch) = (snip[0] == COPY_QNAME_LENGTH) ? 'M' : '*';

            snip     = vb->scratch.data;
            snip_len = vb->scratch.len;            
            break;

        default: {}
    }

    // calculate seq_len (= l_seq, unless l_seq=0), ref_consumed and (if bam) vb->textual_cigar and vb->binary_cigar
    sam_cigar_analyze (vb, STRa(snip), false, &vb->seq_len); 

    rom txt = BAFTtxt;

    if ((OUT_DT(SAM) || OUT_DT(FASTQ)) && !vb->preprocessing) {

        if (snip[snip_len-1] == '*') // eg "151*" - zip added the "151" to indicate seq_len - we don't reconstruct it, just the '*'
            RECONSTRUCT1 ('*');
        
        else if (snip[0] == '-') // eg "-151M" or "-151*" - zip added the "-" to indicate a '*' SEQ field - we don't reconstruct it
            RECONSTRUCT (snip + 1, snip_len - 1);

        else 
            RECONSTRUCT_snip;    
    }

    // BAM - output vb->binary_cigar generated in sam_cigar_analyze
    else if ((OUT_DT(BAM) || OUT_DT(CRAM)) && !vb->preprocessing) {
        // now we have the info needed to reconstruct bin, l_read_name, n_cigar_op and l_seq
        BAMAlignmentFixedP alignment = (BAMAlignmentFixedP)Btxt (vb->line_start);
        alignment->l_read_name = BAFTtxt - &alignment->read_name[0];
        alignment->n_cigar_op = LTEN16 (vb->binary_cigar.len);
        alignment->l_seq = LTEN32 ((snip[0] == '-') ? 0 : vb->seq_len);

        LTEN_u32_buf (&vb->binary_cigar, NULL);
        RECONSTRUCT (vb->binary_cigar.data, vb->binary_cigar.len * sizeof (BamCigarOp));
        LTEN_u32_buf (&vb->binary_cigar, NULL); // restore

        // if BIN is SAM_SPECIAL_BIN, inst.semaphone is set by bam_piz_special_BIN - a signal to us to calculate
        ContextP sam_bam_bin_ctx = CTX(SAM_BAM_BIN);
        if (sam_bam_bin_ctx->semaphore) {
            sam_bam_bin_ctx->semaphore = false;

            PosType32 pos = CTX(SAM_POS)->last_value.i;
            PosType32 last_pos = last_flags.unmapped ? pos : (pos + vb->ref_consumed - 1);
            
            uint16_t bin = bam_reg2bin (pos, last_pos); // zero-based, half-closed half-open [start,end)
            alignment->bin = LTEN16 (bin); // override the -1 previously set by the translator
        }
    }
    
    // store now (instead of in reconstruct_from_ctx_do) in history if we are not reconstructing (e.g., in --fastq)
    if (!reconstruct && ctx->flags.store_per_line && Ltxt != BNUMtxt(txt)) 
        reconstruct_store_history_rollback_recon (VB, ctx, txt);

    sam_cigar_restore_S (stoh);
    buf_free (vb->scratch);

    COPY_TIMER (sam_cigar_special_CIGAR);

    return NO_NEW_VALUE;
}   

// Lookup squanked CIGAR from local (SAM & FASTQ)
SPECIAL_RECONSTRUCTOR (cigar_special_SQUANK) // new_value=NULL means reconstruct to vb->scratch instead of vb->txt_data
{
    #define SRC(x) (snip[0] == SQUANK_BY_##x)

    bool to_scratch = (new_value == NULL); // reconstruct to vb->scratch even if !reconstruct

    int32_t seq_len_plus_H = SRC(std_seq_len)  ? segconf.std_seq_len // MAIN vs the "standard" seq_len (useful for short reads in which most reads are the same length)
                           : SRC(QNAME_length) ? sam_cigar_piz_get_seq_from_qname (VB_SAM) // peek, since length can come from either line1 or line3
                           : /* MAIN */          vb->seq_len + VB_SAM->hard_clip[0] + VB_SAM->hard_clip[1]; // SA/OA/XA vs MAIN: hard-clips in the MAIN CIGAR are counted as well

    STR(segment1);
    ctx_get_next_snip_from_local (VB, ctx, pSTRa(segment1)); // segment1 of squank
    ctx_get_next_snip_from_local (VB, ctx, pSTRa(snip));     // snip = segment2 of squank

    if (!to_scratch && !reconstruct) goto done; // nothing more to do 

    int32_t segment1_seq_len = sam_cigar_get_seq_len_plus_H (STRa(segment1));
    int32_t segment2_seq_len = sam_cigar_get_seq_len_plus_H (STRa(snip)); 
    int32_t missing_len = seq_len_plus_H - segment1_seq_len - segment2_seq_len;
    ASSPIZ (missing_len >= 0, "Expecting missing_len=%d >= 0. seq_len_plus_H=%d segment1_seq_len=%d segment2_seq_len=%d segment1=\"%.*s\" segment2=\"%.*s\"",
            missing_len, seq_len_plus_H, segment1_seq_len, segment2_seq_len, STRf(segment1), STRf(snip));
            
    // reconstruct always if coming from MAIN - it is needed for sam_cigar_analyze even if reconstruct = false
    BufferP buf = &vb->txt_data;

    if (to_scratch) {
        ASSERTNOTINUSE (vb->scratch);
        buf_alloc (vb, &vb->scratch, 0, segment1_len + snip_len + str_int_len (missing_len) + 1, char, 2, "scratch");
        buf = &vb->scratch;
    }

    char *next = BAFTc (*buf);

    if (segment1_len)  
        next = mempcpy (next, segment1, segment1_len); 

    next += str_int_ex (missing_len, next, false);

    if (!snip_len || IS_DIGIT(snip[0])) 
        *next++ = 'S'; // reconstruct removed S - see squank_seg

    if (snip_len) 
        next = mempcpy (next, snip, snip_len); 

    buf->len32 = BNUM (*buf, next);

done:
    return NO_NEW_VALUE;
}

// reconstruct from buddy (mate or prim) CIGAR. If reconstructing to BAM, we convert the binary CIGAR to textual. Used for:
// 1. Reconstructing always-textual MC:Z from a mate CIGAR (called as SPECIAL) - has other_ctx (with BUDDY_MATE parameter since v14)
// 2. Reconstructing always-textual first (prim) alignment in SA:Z of a depn line, copying from prim CIGAR (called from sam_piz_special_SA_main with BUDDY_SAGGY)
SPECIAL_RECONSTRUCTOR_DT (sam_piz_special_COPY_BUDDY_CIGAR)
{
    VBlockSAMP vb = (VBlockSAMP)vb_;

    if (!reconstruct) return false;

    BuddyType bt = snip_len > 1 ? BUDDY_MATE : BUDDY_SAGGY; // for mate, it is segged with the other_ctx

    // fall back to normal COPY_BUDDY in case of SAM
    if (OUT_DT(SAM)) 
        return sam_piz_special_COPY_BUDDY (VB, ctx, STRa(snip), new_value, reconstruct); 

    // get CIGAR field value previously reconstructed in BAM **BINARY** format
    STR(bam_cigar);
    CTX(SAM_CIGAR)->empty_lookup_ok = true; // in case CIGAR is "*" (i.e. empty in BAM) (was incorrectly missing, added in 15.0.62)
    sam_reconstruct_from_buddy_get_textual_snip (vb, CTX(SAM_CIGAR), bt, pSTRa(bam_cigar));
    
#ifndef GENOZIP_ALLOW_UNALIGNED_ACCESS
    char bam_cigar_copy[bam_cigar_len];
    memcpy (bam_cigar_copy, bam_cigar, bam_cigar_len);
    bam_cigar = bam_cigar_copy;
#endif

    // convert binary CIGAR to textual MC:Z
    bam_cigar_len /= sizeof (uint32_t);
    sam_cigar_binary_to_textual (VB, (BamCigarOp *)STRa(bam_cigar), false, &vb->txt_data);

    return NO_NEW_VALUE; 
}

// called from sam_piz_special_pull_from_sag for reconstructing the main CIGAR field of a PRIM / DEPN line
void sam_reconstruct_main_cigar_from_sag (VBlockSAMP vb, bool do_htos, ReconType reconstruct)
{
    // we generate the CIGAR in vb->scratch. sam_cigar_special_CIGAR will reconstruct it (possibly binary) in txt_data. 
    ASSERTNOTINUSE (vb->scratch);
    const SAAln *a = vb->sa_aln;
    uint32_t cigar_len;

    // case: cigar is stored in dict 
    if (a->cigar.piz.is_word) {
        rom cigar_snip;
        ctx_get_snip_by_word_index_do (CTX(OPTION_SA_CIGAR), a->cigar.piz.index, &cigar_snip, &cigar_len, __FUNCLINE);
        buf_add_more (VB, &vb->scratch, cigar_snip, cigar_len, "scratch");

        // case: we need to replace soft-clipping (S) with hard-clipping (H)
        if (do_htos) 
            sam_cigar_S_to_H (vb, STRb(vb->scratch), false);
    }

    // case: cigar is stored in sag_cigars  
    else {
        buf_alloc (vb, &vb->scratch, 0, a->cigar.piz.len, char, 0, "scratch");

        nico_uncompress_textual_cigar (OPTION_SA_CIGAR, B8(z_file->sag_cigars, a->cigar.piz.index), 
                                       &vb->scratch, a->cigar.piz.len, do_htos);
    }

    sam_cigar_special_CIGAR (VB, CTX(SAM_CIGAR), STRb(vb->scratch), NULL, reconstruct);

    buf_free (vb->scratch);
}

// called from sam_sa_reconstruct_SA_from_SA_Group for reconstructing a CIGAR in an SA:Z field of a PRIM/DEPN line
uint32_t sam_reconstruct_SA_cigar_from_SA_Group (VBlockSAMP vb, SAAln *a, bool abbreviate, bool get_X_bases)
{
    STR(cigar);
    cigar = BAFTtxt;
    uint32_t X_bases=0;

    if (a->cigar.piz.is_word) {
        rom cigarS;
        ctx_get_snip_by_word_index_do (CTX(OPTION_SA_CIGAR), a->cigar.piz.index, &cigarS, &cigar_len, __FUNCLINE);
        RECONSTRUCT (cigarS, cigar_len);
    }

    else {
        cigar_len = a->cigar.piz.len;

        nico_uncompress_textual_cigar (OPTION_SA_CIGAR, B8(z_file->sag_cigars, a->cigar.piz.index), 
                                       &vb->txt_data, a->cigar.piz.len, false);
    }

    if (abbreviate || get_X_bases) {
        // analyze this textual SA_CIGAR
        uint32_t n=0, op_i=0, seq_len=0, ref_consumed=0, soft_clip[2]={}, hard_clip[2]={}; 
        for (rom c=cigar; c < cigar + cigar_len; c++) 
            switch (*c) {
                case '0' ... '9'    : n = n*10 + (*c - '0');                                        break;
                case '=' : case 'M' : seq_len += n; ref_consumed += n;               n = 0; op_i++; break;
                case 'X'            : seq_len += n; ref_consumed += n; X_bases += n; n = 0; op_i++; break;
                case 'I'            : seq_len += n;                                  n = 0; op_i++; break;
                case 'N' : case 'D' : ref_consumed += n;                             n = 0; op_i++; break;
                case 'S'            : seq_len += n; soft_clip[op_i > 0] += n;        n = 0; op_i++; break;
                case 'H'            : hard_clip[op_i > 0] += n;                      n = 0; op_i++; break;
                default             :                                                n = 0; op_i++; break;
            }      

        Ltxt -= cigar_len; // undo

        StrText abbrev_cigar; // memory for abbreviated cigar
        if (abbreviate)
            cigar_abbreviate (pSTRa(cigar), seq_len, ref_consumed, soft_clip, hard_clip, &abbrev_cigar);

        RECONSTRUCT_str (cigar); // reconstruct abbreviated cigar
    }

    RECONSTRUCT1 (',');

    return X_bases;
}

// PIZ: main thread: called from sam_show_sag_one_grp for getting first SA_CIGAR_DISPLAY_LEN characters of alignment cigar
StrText sam_piz_display_aln_cigar (VBlockP vb, const SAAln *a)
{
    StrText s = {};

    if (a->cigar.piz.is_word) {
        decl_zctx (OPTION_SA_CIGAR);

        if (a->cigar.piz.index < zctx->word_list.len) {
            STR(cigarS);
            ctx_get_snip_by_word_index (zctx, a->cigar.piz.index, cigarS);
            memcpy (s.s, cigarS, MIN_(cigarS_len, SA_CIGAR_DISPLAY_LEN));
        }
        else {
            snprintf (s.s, sizeof (s.s), "BAD_WORD(%.30s.len=%u)", zctx->tag_name, zctx->word_list.len32);
        }
    }

    else {
        ASSERTNOTINUSE (vb->scratch);
        buf_alloc (vb, &vb->scratch, 0, a->cigar.piz.len, char, 0, "scratch");

        nico_uncompress_textual_cigar (OPTION_SA_CIGAR, B8(z_file->sag_cigars, a->cigar.piz.index), &vb->scratch, a->cigar.piz.len, false);
        memcpy (s.s, B1STc(vb->scratch), MIN_(SA_CIGAR_DISPLAY_LEN, (uint32_t)a->cigar.piz.len));

        buf_free (vb->scratch);
    }

    return s;
}

// Deep and bamass: in-place collapse of X,= to M ; replaces N with D ; removes H,P - ahead of storing cigar in deep_ents/bamass_ents.
void sam_prepare_deep_cigar (VBlockP vb, ConstBamCigarOpP cigar, uint32_t cigar_len, bool reverse)
{
    ASSERTNOTZERO (cigar_len);

    buf_alloc_exact (vb, CTX(SAM_CIGAR)->deep_cigar, cigar_len, BamCigarOp, "deep_cigar");
    BamCigarOpP deep_cigar = B1ST(BamCigarOp, CTX(SAM_CIGAR)->deep_cigar);
     
    for (int i=0; i < cigar_len; i++) {
        ConstBamCigarOpP op = &cigar[i];
        BamCigarOpType new_op;

        if (!op->n) continue; // drop n=0 ops

        switch (op->op) {
            case BC_E : case BC_X : new_op = BC_M; break;
            case BC_N :             new_op = BC_D; break;
            case BC_H : case BC_P : continue; // drop Hs and Ps
            default:                new_op = op->op;
        }
        
        if (i && new_op == BC_M && deep_cigar[-1].op == BC_M) // merge adjacent Ms
            deep_cigar[-1].n += op->n; // just increase n of last deep_cigar op, without incrementing deep_cigar

        else 
            *deep_cigar++ = (BamCigarOp){ .op = new_op, .n = op->n };
    }

    CTX(SAM_CIGAR)->deep_cigar.len32 = BNUM(CTX(SAM_CIGAR)->deep_cigar, deep_cigar);

    // unlikely edge case where the entire cigar is P/H so fully removed (implying seq_len=0)
    if (!CTX(SAM_CIGAR)->deep_cigar.len32)
        BNXT (BamCigarOp, CTX(SAM_CIGAR)->deep_cigar) = (BamCigarOp){ .op = BC_M, .n = 0 }; // "0M"

    if (reverse)
        sam_reverse_binary_cigar (&CTX(SAM_CIGAR)->deep_cigar, &CTX(SAM_CIGAR)->deep_cigar, NULL);
}

//---------------------------------------------------------------------------------------------------
// CIGAR signature
// Note: the signature is in-memory and is not written to the genozip file, so can be changed at will
//---------------------------------------------------------------------------------------------------

uint64_t cigar_sign (VBlockSAMP vb, 
                     ZipDataLineSAMP dl, // set if field originates from SAM_CIGAR, NULL if it originates from SA:Z 
                     STRp(cigar))
{
    StrText abbrev_cigar; // memory allocation for abbreviated cigar, if needed
    if (dl && segconf.SA_CIGAR_abbreviated == yes) 
        cigar_abbreviate (pSTRa(cigar), dl->SEQ.len, vb->ref_consumed, vb->soft_clip, vb->hard_clip, &abbrev_cigar); // also modifies cigar pointer to point to abbrev_cigar

    return crc64 (0, (bytes)STRa(cigar));
}
