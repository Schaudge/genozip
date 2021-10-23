// ------------------------------------------------------------------
//   sam_bam.c
//   Copyright (C) 2020-2021 Black Paw Ventures Limited
//   Please see terms and conditions in the file LICENSE.txt

#include "genozip.h"
#include "profiler.h"
#include "digest.h"
#include "buffer.h"
#include "vblock.h"
#include "txtfile.h"
#include "file.h"
#include "endianness.h"
#include "sam_private.h"
#include "seg.h"
#include "strings.h"
#include "random_access.h"
#include "dict_id.h"
#include "codec.h"
#include "flags.h"
#include "profiler.h"
#include "context.h"
#include "kraken.h"
#include "segconf.h"

void bam_seg_initialize (VBlock *vb)
{
    sam_seg_initialize (vb);

    if (!segconf.running && segconf.has_MC)
        buf_alloc (vb, &VB_SAM->buddy_textual_cigars, 0, segconf.sam_cigar_len * vb->lines.len, char, CTX_GROWTH, "buddy_textual_cigars");
}

// returns the length of the data at the end of vb->txt_data that will not be consumed by this VB is to be passed to the next VB
// if first_i > 0, we attempt to heuristically detect the start of a BAM alignment.
int32_t bam_unconsumed (VBlockP vb, uint32_t first_i, int32_t *i)
{
    ASSERT (*i >= 0 && *i < vb->txt_data.len, "*i=%d is out of range [0,%"PRIu64"]", *i, vb->txt_data.len);

    *i = MIN_(*i, vb->txt_data.len - sizeof(BAMAlignmentFixed));

    // find the first alignment in the data (going backwards) that is entirely in the data - 
    // we identify and alignment by l_read_name and read_name
    for (; *i >= (int32_t)first_i; (*i)--) {
        const BAMAlignmentFixed *aln = (const BAMAlignmentFixed *)ENT (char, vb->txt_data, *i);

        uint32_t block_size = LTEN32 (aln->block_size);
        uint32_t l_seq      = LTEN32 (aln->l_seq);
        uint16_t n_cigar_op = LTEN16 (aln->n_cigar_op);

        // test to see block_size makes sense
        if ((uint64_t)*i + (uint64_t)block_size + 4 > (uint64_t)vb->txt_data.len || // 64 bit arith to catch block_size=-1 that will overflow in 32b
            block_size + 4 < sizeof (BAMAlignmentFixed) + 4*n_cigar_op  + aln->l_read_name + l_seq + (l_seq+1)/2)
            continue;

        // test to see l_read_name makes sense
        if (LTEN32 (aln->l_read_name) < 2 ||
            &aln->read_name[aln->l_read_name] > AFTERENT (char, vb->txt_data)) continue;

        // test pos
        int32_t pos = LTEN32 (aln->pos);
        if (pos < -1) continue;

        // test read_name    
        if (aln->read_name[aln->l_read_name-1] != 0 || // nul-terminated
            !str_is_in_range (aln->read_name, aln->l_read_name-1, '!', '~')) continue;  // all printable ascii (per SAM spec)

        // test l_seq vs seq_len implied by cigar
        if (aln->l_seq && n_cigar_op) {
            uint32_t seq_len_by_cigar=0;
            uint32_t *cigar = (uint32_t *)((uint8_t *)(aln+1) + aln->l_read_name);
            for (uint16_t cigar_op_i=0; cigar_op_i < n_cigar_op; cigar_op_i++) {
                uint8_t cigar_op = *(uint8_t *)&cigar[cigar_op_i] & 0xf; // LSB by Little Endian - take 4 LSb
                uint32_t op_len = cigar[cigar_op_i] >> 4;
                if (cigar_lookup_bam[cigar_op] & CIGAR_CONSUMES_QUERY) seq_len_by_cigar += op_len; 
            }
            if (l_seq != seq_len_by_cigar) continue;
        }

        // Note: we don't use add aln->bin calculation because in some files we've seen data that doesn't
        // agree with our formula. see comment in bam_reg2bin

        // all tests passed - this is indeed an alignment
        return vb->txt_data.len - (*i + LTEN32 (aln->block_size) + 4); // everything after this alignment is "unconsumed"
    }

    return -1; // we can't find any alignment - need more data (lower first_i)
}

static bool bam_seg_get_MD (VBlockSAM *vb, const char *aux, const char *after_aux, pSTRp(md))
{
    while (aux < after_aux) {

        if (!memcmp (aux, "MDZ", 3)) {
            *md = aux+3;
            SAFE_NUL (after_aux);
            *md_len =strlen (aux+3);
            SAFE_RESTORE;

            return true;
        }
    
        static unsigned const size[256] = { ['A']=1, ['c']=1, ['C']=1, ['s']=2, ['S']=2, ['i']=4, ['I']=4, ['f']=4 };

        if (aux[2] == 'Z' || aux[2] == 'H') {
            SAFE_NUL (after_aux);
            aux += strlen (aux+3) + 4; // add tag[2], type and \0
            SAFE_RESTORE;
        }
        else if (aux[2] == 'B') {
            aux += 8 + GET_UINT32 (aux+4) * size[(int)aux[3]]; 
        }   
        else if (size[(int)aux[2]])
            aux += 3 + size[(int)aux[2]];
        else
            ABORT ("vb=%u line_i=%"PRIu64" Unrecognized aux type '%c' (ASCII %u)", vb->vblock_i, vb->line_i, aux[2], aux[2]);
    }

    return false; // MD:Z not found        
}

void bam_seg_BIN (VBlockSAM *vb, ZipDataLineSAM *dl, uint16_t bin /* used only in bam */, PosType this_pos)
{
    bool is_bam = IS_BAM;

    PosType last_pos = dl->FLAG.bits.unmapped ? this_pos : (this_pos + vb->ref_consumed - 1);
    uint16_t reg2bin = bam_reg2bin (this_pos, last_pos); // zero-based, half-closed half-open [start,end)

    if (!is_bam || (last_pos <= MAX_POS_SAM && reg2bin == bin))
        seg_by_did_i (VB, ((char []){ SNIP_SPECIAL, SAM_SPECIAL_BIN }), 2, SAM_BAM_BIN, is_bam ? sizeof (uint16_t) : 0);
    
    else {
#ifdef DEBUG // we show this warning only in DEBUG because I found actual files that have edge cases that don't work with our formula (no harm though)
        WARN_ONCE ("FYI: bad bin value in vb=%u vb->line_i=%"PRIu64": this_pos=%"PRId64" ref_consumed=%u flag=%u last_pos=%"PRId64": bin=%u but reg2bin=%u. No harm. This warning will not be shown again for this file.",
                    vb->vblock_i, vb->line_i, this_pos, vb->ref_consumed, dl->FLAG.value, last_pos, bin, reg2bin);
#endif
        seg_integer (vb, SAM_BAM_BIN, bin, is_bam);
        CTX(SAM_BAM_BIN)->flags.store = STORE_INT;
    }
}

static inline void bam_seg_ref_id (VBlockP vb, DidIType did_i, int32_t ref_id, int32_t compare_to_ref_i)
{
    ASSERT (ref_id >= -1 && ref_id < (int32_t)sam_hdr_contigs->contigs.len, "vb=%u line_i=%"PRIu64": encountered ref_id=%d but header has only %u contigs",
            vb->vblock_i, vb->line_i, ref_id, (uint32_t)sam_hdr_contigs->contigs.len);

    // get snip and snip_len
    STR0(snip);
    if (ref_id >= 0) {
        if (ref_id == compare_to_ref_i) {
            snip = "=";
            snip_len = 1;
        }
        else 
            snip = contigs_get_name (sam_hdr_contigs, ref_id, &snip_len);
    }
    else { 
        snip = "*";
        snip_len = 1;
    }

    sam_seg_RNAME_RNEXT (vb, did_i, STRa(snip), sizeof (int32_t));
}

// re-writes BAM format SEQ into textual SEQ in vb->textual_seq
static inline void bam_rewrite_seq (VBlockSAM *vb, uint32_t l_seq, const char *next_field)
{
    buf_alloc (vb, &vb->textual_seq, 0, l_seq+1 /* +1 for last half-byte */, char, 1.5, "textual_seq");

    if (!l_seq) {
        NEXTENT (char, vb->textual_seq) = '*';
        return;        
    }

    char *next = FIRSTENT (char, vb->textual_seq);

    for (uint32_t i=0; i < (l_seq+1) / 2; i++) {
        static const char base_codes[16] = "=ACMGRSVTWYHKDBN";

        *next++ = base_codes[*(uint8_t*)next_field >> 4];
        *next++ = base_codes[*(uint8_t*)next_field & 0xf];
        next_field++;
    }

    vb->textual_seq.len = l_seq;

    ASSERTW (!(l_seq % 2) || (*AFTERENT(char, vb->textual_seq)=='='), 
            "Warning in bam_rewrite_seq vb=%u: expecting the unused lower 4 bits of last seq byte in an odd-length seq_len=%u to be 0, but its not. This will cause an incorrect MD5",
             vb->vblock_i, l_seq);
}

// Rewrite the QUAL field - add +33 to Phred scores to make them ASCII
static inline bool bam_rewrite_qual (uint8_t *qual, uint32_t l_seq)
{
    if (qual[0] == 0xff) return false; // in case SEQ is present but QUAL is omitted, all qual is 0xFF

    for (uint32_t i=0; i < l_seq; i++)
        qual[i] += 33;

    return true;
}

static inline const char *bam_rewrite_one_optional_number (VBlockSAM *vb, const char *next_field, uint8_t type)
{
    static const char special_float[2] = { SNIP_SPECIAL, SAM_SPECIAL_FLOAT };

    switch (type) {
        case 'c': { uint8_t  n = NEXT_UINT8 ; vb->textual_opt.len += str_int ((int8_t )n, AFTERENT (char, vb->textual_opt)); break; }
        case 'C': { uint8_t  n = NEXT_UINT8 ; vb->textual_opt.len += str_int (         n, AFTERENT (char, vb->textual_opt)); break; }
        case 's': { uint16_t n = NEXT_UINT16; vb->textual_opt.len += str_int ((int16_t)n, AFTERENT (char, vb->textual_opt)); break; }
        case 'S': { uint16_t n = NEXT_UINT16; vb->textual_opt.len += str_int (         n, AFTERENT (char, vb->textual_opt)); break; }
        case 'i': { uint32_t n = NEXT_UINT32; vb->textual_opt.len += str_int ((int32_t)n, AFTERENT (char, vb->textual_opt)); break; }
        case 'I': { uint32_t n = NEXT_UINT32; vb->textual_opt.len += str_int (         n, AFTERENT (char, vb->textual_opt)); break; }
        case 'f': { buf_add (&vb->textual_opt, special_float, 2); 
                    uint32_t n = NEXT_UINT32; // n is the 4 bytes of the little endian float, construct and int, and switch to machine endianity 
                    n = LTEN32 (n);           // switch back to Little Endian as it was in the BAM file
                    /* integer as text */     vb->textual_opt.len += str_int (         n, AFTERENT (char, vb->textual_opt)); break; }
        default: ABORT ("Error in bam_rewrite_one_optional_number: enrecognized Optional field type '%c' (ASCII %u) in vb=%u", 
                        type, type, vb->vblock_i);
    }    

    return next_field;
} 

const char *bam_get_one_optional (VBlockSAM *vb, const char *next_field,
                                  const char **tag, char *type, const char **value, unsigned *value_len) // out
{
    *tag  = next_field;
    *type = next_field[2]; // c, C, s, S, i, I, f, A, Z, H or B
    next_field += 3;

    if (*type == 'Z' || *type == 'H') {
        *value = next_field;
        *value_len = strlen (*value);
        return next_field + *value_len + 1; // +1 for \0
    }
    
    else if (*type == 'A') {
        *value = next_field;
        *value_len = 1;
        return next_field + 1;
    } 

    uint32_t max_len = (*type == 'B') ? (12 * GET_UINT32 (next_field) + 10) : // worst case scenario for item: "-1000000000,"
                       30;
    buf_alloc (vb, &vb->textual_opt, 0, max_len, char, 2, "textual_opt"); // a rather inefficient max in case of arrays, to do: tighten the calculation

    if (*type != 'B')
        next_field = bam_rewrite_one_optional_number (vb, next_field, *type);

    else { // 'B'
        char subtype = *next_field++; // type of elements of array
        uint32_t num_elements = NEXT_UINT32;

        NEXTENT (uint8_t, vb->textual_opt) = subtype;
        
        for (uint32_t i=0; i < num_elements; i++) {
            NEXTENT (char, vb->textual_opt) = ',';
            next_field = bam_rewrite_one_optional_number (vb, next_field, subtype);
        }
    }    

    *value     = vb->textual_opt.data;
    *value_len = vb->textual_opt.len;

    return next_field;
}

const char *bam_seg_txt_line (VBlock *vb_, const char *alignment /* BAM terminology for one line */,
                              uint32_t remaining_txt_len, bool *has_13_unused)   
{
    VBlockSAM *vb = (VBlockSAM *)vb_;
    ZipDataLineSAM *dl = DATA_LINE (vb->line_i);
    const char *next_field = alignment;
    // *** ingest BAM alignment fixed-length fields ***
    uint32_t block_size = NEXT_UINT32;
    vb->buddy_line_i = NO_BUDDY; // initialize

    WordIndex prev_line_chrom = vb->chrom_node_index;
    PosType prev_line_pos = vb->last_int (SAM_POS);

    // a non-sensical block_size might indicate an false-positive identification of a BAM alignment in bam_unconsumed
    ASSERT (block_size + 4 >= sizeof (BAMAlignmentFixed) && block_size + 4 <= remaining_txt_len, 
            "vb=%u line_i=%"PRIu64" (block_size+4)=%u is out of range - too small, or goes beyond end of txt data: remaining_txt_len=%u",
            vb->vblock_i, vb->line_i, block_size+4, remaining_txt_len);

    const char *after = alignment + block_size + sizeof (uint32_t);

    int32_t ref_id      = (int32_t)NEXT_UINT32;     // corresponding to CHROMs in the BAM header
    PosType this_pos    = 1 + (int32_t)NEXT_UINT32; // pos in BAM is 0 based, -1 for unknown 
    uint8_t l_read_name = NEXT_UINT8;               // QNAME length
    uint8_t mapq        = NEXT_UINT8;
    uint16_t bin        = NEXT_UINT16;
    uint16_t n_cigar_op = NEXT_UINT16;
    dl->FLAG.value      = NEXT_UINT16;              // not to be confused with our global var "flag"
    uint32_t l_seq      = NEXT_UINT32;              // note: we stick with the same logic as SAM for consistency - dl->seq_len is determined by CIGAR 
    int32_t next_ref_id = (int32_t)NEXT_UINT32;     // corresponding to CHROMs in the BAM header
    PosType next_pos    = 1 + (int32_t)NEXT_UINT32; // pos in BAM is 0 based, -1 for unknown
    int32_t tlen        = (int32_t)NEXT_UINT32;

    // seg QNAME first, as it will find the buddy
    sam_seg_QNAME (vb, dl, next_field, l_read_name-1, 2); // QNAME. account for \0 and l_read_name
    next_field += l_read_name; // inc. \0

    bam_seg_ref_id (VB, SAM_RNAME, ref_id, -1); // ref_id (RNAME)

    // note: pos can have a value even if ref_id=-1 (RNAME="*") - this happens if a SAM with a RNAME that is not in the header is converted to BAM with samtools
    sam_seg_POS (vb, dl, 0, 0, this_pos, prev_line_chrom, prev_line_pos, sizeof (uint32_t)); // POS
    
    if (ref_id >= 0) sam_seg_verify_RNAME_POS (VB, NULL, this_pos);

    sam_seg_MAPQ (VB, dl, 0, 0, mapq, sizeof (mapq));

    sam_seg_FLAG (vb, dl, 0, 0, sizeof (uint16_t));
    
    bam_seg_ref_id (VB, SAM_RNEXT, next_ref_id, ref_id); // RNEXT

    sam_seg_PNEXT (vb, dl, 0, 0, next_pos, prev_line_pos, sizeof (uint32_t));

    // *** ingest & segment variable-length fields ***

    // CIGAR
    sam_cigar_binary_to_textual (vb, n_cigar_op, (uint32_t*)next_field, &vb->textual_cigar); // re-write BAM format CIGAR as SAM textual format in vb->textual_cigar
    sam_cigar_analyze (vb, vb->textual_cigar.data, vb->textual_cigar.len, &dl->seq_len);
    next_field += n_cigar_op * sizeof (uint32_t);

    // Segment BIN after we've gathered bin, flags, pos and vb->ref_confumed (and before sam_seg_SEQ which ruins vb->ref_consumed)
    bam_seg_BIN (vb, dl, bin, this_pos);

    // we search forward for MD:Z now, as we will need it for SEQ if it exists
    if (segconf.has_MD && !segconf.running) {
        STR(md); 
        if (bam_seg_get_MD (vb, next_field + (l_seq+1)/2 + l_seq, after, pSTRa(md)))
            sam_md_analyze (vb, STRa(md), this_pos, FIRSTENT(char, vb->textual_cigar));
    }

    // SEQ - calculate diff vs. reference (denovo or loaded)
    bam_rewrite_seq (vb, l_seq, next_field);

    ASSERT (dl->seq_len == l_seq || vb->last_cigar[0] == '*' || !l_seq, 
            "seq_len implied by CIGAR=%s is %u, but actual SEQ length is %u, SEQ=%.*s", 
            vb->last_cigar, dl->seq_len, l_seq, l_seq, vb->textual_seq.data);

    sam_seg_SEQ (vb, SAM_SQBITMAP, vb->textual_seq.data, vb->textual_seq.len, this_pos, vb->last_cigar, vb->ref_consumed, vb->ref_and_seq_consumed, 
                       0, vb->textual_seq.len, vb->last_cigar, (l_seq+1)/2 + sizeof (uint32_t) /* account for l_seq and seq fields */);
    next_field += (l_seq+1)/2; 

    // QUAL
    bool has_qual=false;
    if (l_seq)
        has_qual = bam_rewrite_qual ((uint8_t *)next_field, l_seq); // add 33 to Phred scores to make them ASCII
    
    if (has_qual) // case we have both SEQ and QUAL
        sam_seg_QUAL (vb, dl, next_field, l_seq, l_seq /* account for qual field */ );

    else { // cases 1. were both SEQ and QUAL are '*' (seq_len=0) and 2. SEQ exists, QUAL not (bam_rewrite_qual returns false)
        *(char *)alignment = '*'; // overwrite as we need it somewhere in txt_data
        sam_seg_QUAL (vb, dl, alignment, 1, l_seq /* account of l_seq 0xff */);
    }
    next_field += l_seq; 

    // finally we can segment the textual CIGAR now (including if n_cigar_op=0)
    sam_cigar_seg_binary (vb, dl, l_seq, n_cigar_op);

    // OPTIONAL fields - up to MAX_FIELDS of them
    next_field = sam_seg_optional_all (vb, dl, next_field, 0,0,0, after);
    
    // TLEN - must be after OPTIONAL as we might need data from MC:Z
    sam_seg_TLEN (vb, dl, 0, 0, (int64_t)tlen, ref_id == next_ref_id); // TLEN

    buf_free (&vb->textual_cigar);
    buf_free (&vb->textual_seq);

    return next_field;
}
