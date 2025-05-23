// ------------------------------------------------------------------
//   sam_bam_seq.c - functions for handling BAM binary sequence format 
//   Copyright (C) 2020-2025 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited,
//   under penalties specified in the license.

#include "sam_private.h"

// the characters "=ACMGRSVTWYHKDBN" are mapped to BAM 0->15, in this matrix we add 0x80 as a validity bit. All other characters are 0x00 - invalid
static const uint8_t sam2bam_seq_map[256] = { ['=']=0x80, ['A']=0x81, ['C']=0x82, ['M']=0x83, ['G']=0x84, ['R']=0x85, ['S']=0x86, ['V']=0x87, 
                                              ['T']=0x88, ['W']=0x89, ['Y']=0x8a, ['H']=0x8b, ['K']=0x8c, ['D']=0x8d, ['B']=0x8e, ['N']=0x8f };

const char bam_base_codes[16] = "=ACMGRSVTWYHKDBN";

rom bam_seq_display (bytes seq, uint32_t l_seq) // caller should free memory
{
    char *str = MALLOC (l_seq + 2);

    for (uint32_t i=0; i < (l_seq+1)/2; i++) {
        str[i*2]   = bam_base_codes[seq[i] >> 4];
        str[i*2+1] = bam_base_codes[seq[i] & 0xf];
    }

    str[l_seq] = 0;
    return str;
}

// called from sam_zip_prim_ingest_vb, somewhat similar to sam_piz_sam2bam_SEQ
void sam_seq_to_bam (STRp (seq_sam), BufferP seq_bam_buf)
{
    uint8_t *seq_bam = BAFT8 (*seq_bam_buf);
    uint32_t seq_bam_len = (seq_sam_len+1)/2;

    for (uint32_t i=0; i < seq_bam_len; i++, seq_bam++, seq_sam += 2) {
        uint8_t base[2] = { sam2bam_seq_map[(uint8_t)seq_sam[0]], sam2bam_seq_map[(uint8_t)seq_sam[1]] };
        
        // check for invalid characters 
        for (unsigned b=0; b < 2; b++)
            if (!base[b] && !(b==1 && (i+1)*2 > seq_sam_len)) {
                ASSINP (false, "Invalid base: invalid character encountered in sequence: '%c' (ASCII %u). position %u SEQ(first 1000 bases)=\"%s\"", 
                        base[b], base[b], i*2+b, str_to_printable_(seq_sam, MIN_(1000,seq_sam_len)).s);
                base[b] = 0x0f;
            }

        *seq_bam = (base[0] << 4) | (base[1] & 0x0f);
    }

    // if number of bases is odd, zero the last, unused, base
    if (seq_bam_len & 1) 
        seq_bam[seq_bam_len-1] &= 0xf0;

    seq_bam_buf->len += seq_bam_len;
}

// re-writes BAM format SEQ into textual SEQ
void bam_seq_to_sam (VBlockP vb, bytes bam_seq, 
                     uint32_t seq_len,       // bases, not bytes
                     bool start_mid_byte,    // ignore first nibble of bam_seq (seq_len doesn't include the ignored nibble)
                     bool test_final_nibble, // if true, we test that the final nibble, if unused, is 0, and warn if not
                     BufferP out,            // appends to end of buffer - caller should allocate seq_len+1 (+1 for last half-byte) 
                     bool is_from_zip_cb)    // don't account for time when codec-compressing, as the codecs account for their own time
{
    START_TIMER;
        
    ASSERT (out->len32 + seq_len + 2 <= out->size, "%s: out allocation too small", LN_NAME);

    if (!seq_len) {
        BNXTc (*out) = '*';
        return;        
    }

    // we implement "start_mid_byte" by converting the redudant base too, but starting 1 character before in the buffer 
    char save = 0;
    if (start_mid_byte) {
        out->len32--;
        seq_len++;
        save = *BAFTc (*out); // this is the byte we will overwrite, and recover it later. possibly, the fence if the buffer is empty;
    }
    
    char *next = BAFTc (*out);

    uint32_t sam_len = (seq_len+1) / 2;
    for (uint32_t i=0; i < sam_len; i++) {
        *next++ = bam_base_codes[*(uint8_t*)bam_seq >> 4];
        *next++ = bam_base_codes[*(uint8_t*)bam_seq & 0xf];
        bam_seq++;
    }

    if (start_mid_byte) {
        *BAFTc(*out) = save;
        out->len32 += seq_len;      
        seq_len--;
    }
    else
        out->len32 += seq_len;

    ASSERTW (!test_final_nibble || !(seq_len % 2) || (*BAFTc (*out)=='='), 
             "%s: Warning in bam_seq_to_sam: expecting the unused lower 4 bits of last seq byte in an odd-length seq_len=%u to be 0, but its not. This will cause an incorrect digest",
             LN_NAME, seq_len);

    *BAFTc(*out) = 0; // nul-terminate after end of seq
    
    if (!is_from_zip_cb) COPY_TIMER(bam_seq_to_sam);

/* TO DO - not working yet
    uint64_t *next64 = BAFT (uint64_t, *out);

    // highly optimized - the unoptimized loop consumed up to 10% of all seg time in long reads!
    bytes bam_after = bam_seq + (seq_len+1) / 2;

    // 8 bases at a time (4 bytes in the bam seq, 8 bytes in the sam seq)
    uint32_t *bam_seq32;
    for (bam_seq32 = (uint32_t *)bam_seq; (bam_after - (uint8_t*)bam_seq32) >= 4 ; bam_seq32++) {
#if defined __LITTLE_ENDIAN__
        uint32_t bs32 = *bam_seq32;
        *next64++ = ((uint64_t)bam_base_codes[(bs32 >> 4)  & 0xf] << 0 ) | ((uint64_t)bam_base_codes[(bs32 >> 0 ) & 0xf] << 8 ) | 
                    ((uint64_t)bam_base_codes[(bs32 >> 12) & 0xf] << 16) | ((uint64_t)bam_base_codes[(bs32 >> 8 ) & 0xf] << 24) | 
                    ((uint64_t)bam_base_codes[(bs32 >> 20) & 0xf] << 32) | ((uint64_t)bam_base_codes[(bs32 >> 16) & 0xf] << 40) | 
                    ((uint64_t)bam_base_codes[(bs32 >> 28) & 0xf] << 48) | ((uint64_t)bam_base_codes[(bs32 >> 24) & 0xf] << 56) ; 
    }
#else
        #error TO DO
#endif
        // *next++ = bam_base_codes[*(uint8_t*)bam_seq >> 4];
        // *next++ = bam_base_codes[*(uint8_t*)bam_seq & 0xf];

    // last 1-3 bam bytes = 2-6 sam bases
    if ((uint8_t*)bam_seq32 < bam_after) {
        uint8_t *next8 = (uint8_t *)next64;
        for (uint8_t *bam_seq8 = ((uint8_t *)bam_seq32); bam_seq8 < bam_after; bam_seq8++) {
            uint8_t bs8 = *bam_seq8;
            *next8++ = bam_base_codes[bs8 >> 4];
            *next8++ = bam_base_codes[bs8 & 0xf];
        }
    }
    */
}

/*
// compare a sub-sequence to a full sequence and return true if they're the same. Sequeneces in BAM format.
bool bam_seq_has_sub_seq (bytes full_seq, uint32_t full_seq_len, 
                          bytes sub_seq,  uint32_t sub_seq_len, uint32_t start_base) // lengths are in bases, not bytes
{
    // easy case: similar byte alignment
    if (!(start_base & 1)) {
        if (memcmp (sub_seq, &full_seq[start_base/2], sub_seq_len/2)) return false; // mismatch in first even number of bases
        if (!(sub_seq_len & 1)) return true; // even number of bases

        uint8_t last_base_full = full_seq[(start_base + sub_seq_len - 1)/2] >> 4;
        uint8_t last_base_sub  = sub_seq[(sub_seq_len - 1)/2] >> 4;
        return last_base_full == last_base_sub;
    }

    // not byte-aligned
    else { 
        for (uint32_t sub_base_i=0; sub_base_i < sub_seq_len; sub_base_i++) {
    
            uint8_t base_sub = (sub_base_i % 2) ? (sub_seq[sub_base_i/2] & 15) : (sub_seq[sub_base_i/2] >> 4);
                        
            uint32_t full_base_i = start_base + sub_base_i;
            uint8_t base_full = (full_base_i % 2) ? (full_seq[full_base_i/2] & 15) : (full_seq[full_base_i/2] >> 4);

            if (base_sub != base_full) return false;
        }
        return true; // all bases are the same
    }
}
*/
static void bam_seq_copy (uint8_t *dst, bytes src, 
                          uint32_t src_start_base, uint32_t n_bases) // bases, not bytes
{
    src += src_start_base / 2;

    if (src_start_base & 1) {
        for (uint32_t i=0; i < n_bases / 2; i++) 
            *dst++ = ((src[i] & 0x0f) << 4) | (src[i+1] >> 4); 
        
        if (n_bases & 1)
            *dst = (src[n_bases / 2] & 0x0f) << 4;
    }
    else {
        memcpy (dst, src, (n_bases+1)/2);

        if (n_bases & 1)
            dst[n_bases/2] &= 0xf0; // keep the high nibble only (the first BAM base of this byte)
    }
}

// C<>G A<>T ; IUPACs: R<>Y K<>M B<>V D<>H W<>W S<>S N<>N
static void bam_seq_revcomp_in_place (uint8_t *seq, uint32_t n_bases)
{                                 // Was:  =    A    C    M    G    R    S    V    T    W    Y    H    K    D    B    N                          
                                  // Comp: =    T    G    K    C    Y    S    B    A    W    R    D    M    H    V    N
    static const uint8_t bam_comp[16] = { 0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe, 0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf };

    for (int32_t i=0, j=n_bases-1; i < n_bases/2; i++, j--) {
        
        uint8_t b1 = (i&1) ? (seq[i/2] & 0xf) : (seq[i/2] >> 4);
        uint8_t b1c = bam_comp[b1];

        uint8_t b2 = (j&1) ? (seq[j/2] & 0xf) : (seq[j/2] >> 4);
        uint8_t b2c = bam_comp[b2];

        seq[i/2] = (i&1) ? (b2c | (seq[i/2] & 0xf0)) : ((b2c << 4) | (seq[i/2] & 0x0f));
        seq[j/2] = (j&1) ? (b1c | (seq[j/2] & 0xf0)) : ((b1c << 4) | (seq[j/2] & 0x0f));
    }
}

// returns length in bytes 
uint32_t sam_seq_copy (char *dst, rom src, uint32_t src_start_base, uint32_t n_bases, 
                       bool revcomp, bool is_bam_format)
{
    if (!is_bam_format) {
        if (revcomp)
            str_revcomp (dst, src, n_bases);
        else
            memcpy (dst, src, n_bases);
    }
    
    else {
        bam_seq_copy ((uint8_t*)dst, (const uint8_t*)src, src_start_base, n_bases);

        if (revcomp)
            bam_seq_revcomp_in_place ((uint8_t*)dst, n_bases);
    } 

    return is_bam_format ? (n_bases + 1) / 2 : n_bases;
}

/*
// like bam_seq_has_sub_seq, but sub_seq is reverse complemented, and start_base is relative to END of full_seq
bool bam_seq_has_sub_seq_revcomp (bytes full_seq, uint32_t full_seq_len, 
                                  bytes sub_seq,  uint32_t sub_seq_len, uint32_t start_base)
{
    bam_seq_revcomp_in_place ((uint8_t*)STRa(sub_seq));

    bool same = bam_seq_has_sub_seq (STRa(full_seq), STRa(sub_seq), full_seq_len - start_base - sub_seq_len);

    // revcomp-back, if we have to
    bam_seq_revcomp_in_place ((uint8_t*)STRa(sub_seq));

    return same;
}
*/