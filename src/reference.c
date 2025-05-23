// ------------------------------------------------------------------
//   reference.c
//   Copyright (C) 2020-2025 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#include <errno.h>
#include "ref_private.h"
#include "dispatcher.h"
#include "zfile.h"
#include "random_access.h"
#include "seg.h"
#include "piz.h"
#include "sections.h"
#include "filename.h"
#include "regions.h"
#include "refhash.h"
#include "compressor.h"
#include "ref_iupacs.h"
#include "chrom.h"
#include "crypt.h"
#include "arch.h"
#include "zriter.h"
#include "tip.h"
#include "sorter.h"

RefStruct gref = { .ctgs.name = "gref" }; // reference - accessible to via ref_private only 
const PosType64 *p_genome_nbases = &gref.genome_nbases; // globally accessible for convenience

#define CHROM_GENOME 0
#define CHROM_NAME_GENOME "GENOME"

#define CHROM_GENOME_REV 1
#define CHROM_NAME_GENOME_REV "GENOME_REV"

#define ref_is_range_used(r) ((r)->ref.nbits && ((r)->is_set.nbits || flag.make_reference))

// get / set functions 
rom ref_get_filename (void)            { return gref.filename;               }
rom ref_get_fasta_name (void)          { return gref.ref_fasta_name;         }
uint8_t ref_get_genozip_version (void) { return gref.genozip_version;        }
BufferP ref_get_stored_ra (void)       { return &gref.stored_ra;             }
Digest ref_get_genome_digest (void)    { return gref.genome_digest;          }
bool ref_is_digest_adler (void)        { return gref.is_adler;               }
rom ref_get_digest_name (void)         { return digest_name_(gref.is_adler); }
ContigPkgP ref_get_ctgs (void)         { return &gref.ctgs;                  }
uint32_t ref_num_contigs (void)        { return gref.ctgs.contigs.len32;     }
BitsP ref_get_genome_is_set (void)     { return gref.genome_is_set;          }
static void reoverlay_ranges_on_loaded_genome (int64_t delta_bytes);

void ref_get_genome (const Bits **genome, const Bits **emoneg, PosType64 *genome_nbases)
{
    ASSERT0 (!genome || gref.genome, "Reference file not loaded");
    
    if (genome) *genome = gref.genome;
    if (emoneg) *emoneg = refhash_get_emoneg();
    if (genome_nbases) *genome_nbases = gref.genome_nbases;
}

// caller allocates: requirement: 3 bytes before and 3 bytes after ref must be writtable
// returns ref;
// if revcomp: seq generated is still between [gpos, (gpos + seq_len - 1)], just revcomped 
rom ref_get_textual_seq (PosType64 gpos, STRc(ref), bool revcomp)
{
    static union { 
        char b4[256][4];      // 256 possibilities of 4 bases in a 2bit-representation.
        uint32_t b4_int[256]; // integer access to b4. also forces s to be 4B-aligned.
    } s = { .b4 = { 
        // last base A
        "AAAA", "CAAA", "GAAA", "TAAA",   "ACAA", "CCAA", "GCAA", "TCAA", // last 2 bases: AA
        "AGAA", "CGAA", "GGAA", "TGAA",   "ATAA", "CTAA", "GTAA", "TTAA",   
        "AACA", "CACA", "GACA", "TACA",   "ACCA", "CCCA", "GCCA", "TCCA", // last 2 bases: CA
        "AGCA", "CGCA", "GGCA", "TGCA",   "ATCA", "CTCA", "GTCA", "TTCA",   
        "AAGA", "CAGA", "GAGA", "TAGA",   "ACGA", "CCGA", "GCGA", "TCGA", // last 2 bases: GA 
        "AGGA", "CGGA", "GGGA", "TGGA",   "ATGA", "CTGA", "GTGA", "TTGA",   
        "AATA", "CATA", "GATA", "TATA",   "ACTA", "CCTA", "GCTA", "TCTA", // last 2 bases: TA
        "AGTA", "CGTA", "GGTA", "TGTA",   "ATTA", "CTTA", "GTTA", "TTTA",   
        // last base C
        "AAAC", "CAAC", "GAAC", "TAAC",   "ACAC", "CCAC", "GCAC", "TCAC", // last 2 bases: AC
        "AGAC", "CGAC", "GGAC", "TGAC",   "ATAC", "CTAC", "GTAC", "TTAC",   
        "AACC", "CACC", "GACC", "TACC",   "ACCC", "CCCC", "GCCC", "TCCC", // last 2 bases: CC
        "AGCC", "CGCC", "GGCC", "TGCC",   "ATCC", "CTCC", "GTCC", "TTCC",   
        "AAGC", "CAGC", "GAGC", "TAGC",   "ACGC", "CCGC", "GCGC", "TCGC", // last 2 bases: GC 
        "AGGC", "CGGC", "GGGC", "TGGC",   "ATGC", "CTGC", "GTGC", "TTGC",   
        "AATC", "CATC", "GATC", "TATC",   "ACTC", "CCTC", "GCTC", "TCTC", // last 2 bases: TC
        "AGTC", "CGTC", "GGTC", "TGTC",   "ATTC", "CTTC", "GTTC", "TTTC",   
        // last base G
        "AAAG", "CAAG", "GAAG", "TAAG",   "ACAG", "CCAG", "GCAG", "TCAG", // last 2 bases: AG
        "AGAG", "CGAG", "GGAG", "TGAG",   "ATAG", "CTAG", "GTAG", "TTAG",   
        "AACG", "CACG", "GACG", "TACG",   "ACCG", "CCCG", "GCCG", "TCCG", // last 2 bases: CG
        "AGCG", "CGCG", "GGCG", "TGCG",   "ATCG", "CTCG", "GTCG", "TTCG",   
        "AAGG", "CAGG", "GAGG", "TAGG",   "ACGG", "CCGG", "GCGG", "TCGG", // last 2 bases: GG 
        "AGGG", "CGGG", "GGGG", "TGGG",   "ATGG", "CTGG", "GTGG", "TTGG",   
        "AATG", "CATG", "GATG", "TATG",   "ACTG", "CCTG", "GCTG", "TCTG", // last 2 bases: TG
        "AGTG", "CGTG", "GGTG", "TGTG",   "ATTG", "CTTG", "GTTG", "TTTG",   
        // last base T
        "AAAT", "CAAT", "GAAT", "TAAT",   "ACAT", "CCAT", "GCAT", "TCAT", // last 2 bases: AT
        "AGAT", "CGAT", "GGAT", "TGAT",   "ATAT", "CTAT", "GTAT", "TTAT",   
        "AACT", "CACT", "GACT", "TACT",   "ACCT", "CCCT", "GCCT", "TCCT", // last 2 bases: CT
        "AGCT", "CGCT", "GGCT", "TGCT",   "ATCT", "CTCT", "GTCT", "TTCT",   
        "AAGT", "CAGT", "GAGT", "TAGT",   "ACGT", "CCGT", "GCGT", "TCGT", // last 2 bases: GT 
        "AGGT", "CGGT", "GGGT", "TGGT",   "ATGT", "CTGT", "GTGT", "TTGT",   
        "AATT", "CATT", "GATT", "TATT",   "ACTT", "CCTT", "GCTT", "TCTT", // last 2 bases: TT
        "AGTT", "CGTT", "GGTT", "TGTT",   "ATTT", "CTTT", "GTTT", "TTTT",  
    } };

    static union {
        char b4[256][4];
        uint32_t b4_int[256];
    } s_rev = { .b4 = {
        // first base T
        "TTTT", "TTTG", "TTTC", "TTTA",   "TTGT", "TTGG", "TTGC", "TTGA", // first 2 bases: TT
        "TTCT", "TTCG", "TTCC", "TTCA",   "TTAT", "TTAG", "TTAC", "TTAA", 
        "TGTT", "TGTG", "TGTC", "TGTA",   "TGGT", "TGGG", "TGGC", "TGGA", // first 2 bases: TG
        "TGCT", "TGCG", "TGCC", "TGCA",   "TGAT", "TGAG", "TGAC", "TGAA", 
        "TCTT", "TCTG", "TCTC", "TCTA",   "TCGT", "TCGG", "TCGC", "TCGA", // first 2 bases: TC
        "TCCT", "TCCG", "TCCC", "TCCA",   "TCAT", "TCAG", "TCAC", "TCAA", 
        "TATT", "TATG", "TATC", "TATA",   "TAGT", "TAGG", "TAGC", "TAGA", // first 2 bases: TA
        "TACT", "TACG", "TACC", "TACA",   "TAAT", "TAAG", "TAAC", "TAAA", 
        // first base G
        "GTTT", "GTTG", "GTTC", "GTTA",   "GTGT", "GTGG", "GTGC", "GTGA", // first 2 bases: GT
        "GTCT", "GTCG", "GTCC", "GTCA",   "GTAT", "GTAG", "GTAC", "GTAA", 
        "GGTT", "GGTG", "GGTC", "GGTA",   "GGGT", "GGGG", "GGGC", "GGGA", // first 2 bases: GG
        "GGCT", "GGCG", "GGCC", "GGCA",   "GGAT", "GGAG", "GGAC", "GGAA", 
        "GCTT", "GCTG", "GCTC", "GCTA",   "GCGT", "GCGG", "GCGC", "GCGA", // first 2 bases: GC
        "GCCT", "GCCG", "GCCC", "GCCA",   "GCAT", "GCAG", "GCAC", "GCAA", 
        "GATT", "GATG", "GATC", "GATA",   "GAGT", "GAGG", "GAGC", "GAGA", // first 2 bases: GA
        "GACT", "GACG", "GACC", "GACA",   "GAAT", "GAAG", "GAAC", "GAAA", 
        // first base C
        "CTTT", "CTTG", "CTTC", "CTTA",   "CTGT", "CTGG", "CTGC", "CTGA", // first 2 bases: CT
        "CTCT", "CTCG", "CTCC", "CTCA",   "CTAT", "CTAG", "CTAC", "CTAA", 
        "CGTT", "CGTG", "CGTC", "CGTA",   "CGGT", "CGGG", "CGGC", "CGGA", // first 2 bases: CG
        "CGCT", "CGCG", "CGCC", "CGCA",   "CGAT", "CGAG", "CGAC", "CGAA", 
        "CCTT", "CCTG", "CCTC", "CCTA",   "CCGT", "CCGG", "CCGC", "CCGA", // first 2 bases: CC
        "CCCT", "CCCG", "CCCC", "CCCA",   "CCAT", "CCAG", "CCAC", "CCAA", 
        "CATT", "CATG", "CATC", "CATA",   "CAGT", "CAGG", "CAGC", "CAGA", // first 2 bases: CA
        "CACT", "CACG", "CACC", "CACA",   "CAAT", "CAAG", "CAAC", "CAAA", 
        // first base A
        "ATTT", "ATTG", "ATTC", "ATTA",   "ATGT", "ATGG", "ATGC", "ATGA", // first 2 bases: AT
        "ATCT", "ATCG", "ATCC", "ATCA",   "ATAT", "ATAG", "ATAC", "ATAA", 
        "AGTT", "AGTG", "AGTC", "AGTA",   "AGGT", "AGGG", "AGGC", "AGGA", // first 2 bases: AG
        "AGCT", "AGCG", "AGCC", "AGCA",   "AGAT", "AGAG", "AGAC", "AGAA", 
        "ACTT", "ACTG", "ACTC", "ACTA",   "ACGT", "ACGG", "ACGC", "ACGA", // first 2 bases: AC
        "ACCT", "ACCG", "ACCC", "ACCA",   "ACAT", "ACAG", "ACAC", "ACAA", 
        "AATT", "AATG", "AATC", "AATA",   "AAGT", "AAGG", "AAGC", "AAGA", // first 2 bases: AA
        "AACT", "AACG", "AACC", "AACA",   "AAAT", "AAAG", "AAAC", "AAAA", 
    } };

    // sanity
    ASSERT0 (gref.genome, "Reference file not loaded");
    ASSERTINRANGE (gpos, 0, gref.genome->nbits / 2);

    // save 3 flanking bytes on each side that might be temporarily over-written
    char save_before[3] = { ref[-3], ref[-2], ref[-1] };
    char save_after [3] = { ref[ref_len], ref[ref_len+1], ref[ref_len+2] };

    // calculate new_gpos and new_ref_len to be the minimal sequence that is divisible by 4
    // possibly expanding the request ref to the left and right    
    PosType64 after = ROUNDUP4 (gpos + ref_len);
    int right_extra_bases = after - (gpos + ref_len);

    int left_extra_bases = gpos % 4;
    PosType64 new_gpos = gpos - left_extra_bases; // beginning of full byte

    char *new_ref = ref - (revcomp ? right_extra_bases : left_extra_bases);       
    uint32_t new_ref_len = ref_len + left_extra_bases + right_extra_bases; 

    // generate the expanded sequence - writing 4 bases at a time
    bytes b = &((bytes)gref.genome->words)[new_gpos / 4];
    if (!revcomp) 
        for (uint32_t i=0; i < new_ref_len; i += 4)
            *(unaligned_uint32_t *)&new_ref[i] = s.b4_int[b[i/4]]; 

    else 
        for (uint32_t i=0; i < new_ref_len; i += 4)
            *(unaligned_uint32_t *)&new_ref[new_ref_len - i - 4] = s_rev.b4_int[b[i/4]]; 

    // restore possibly over-written flanking data
    memcpy (&ref[-3], save_before, 3);
    memcpy (&ref[ref_len], save_after, 3);
    
    return ref;
}

void ref_set_genome_is_used (PosType64 gpos, uint32_t len)
{
    if (len == 1)
        bits_set (gref.genome_is_set, gpos); 
    else 
        bits_set_region (gref.genome_is_set, gpos, len);
}

static inline bool ref_has_is_set (void)
{
    return (primary_command == PIZ && !IS_REF_EXTERNAL) || 
           (primary_command == ZIP && (IS_REF_EXT_STORE || IS_REF_INTERNAL));
}

void ref_get_is_set_bytemap (VBlockP vb, PosType64 gpos, uint32_t num_bases, bool rev_comp, BufferP is_set, rom buf_name)
{
    ASSERTNOTINUSE (*is_set);
    buf_alloc_exact (vb, *is_set, num_bases, uint8_t, buf_name);

    bits_bit_to_byte (B1ST8(*is_set), gref.genome_is_set, gpos, num_bases); 
    
    if (rev_comp)
        str_reverse_in_place (STRb(*is_set));
}

// free memory allocations between files, when compressing multiple non-bound files or decompressing multiple files
void ref_unload_reference (void)
{
    // case: the reference has been modified and we can't use it for the next file
    if (IS_REF_INTERNAL || IS_REF_EXT_STORE || IS_REF_STORED_PIZ) {
        buf_free (gref.genome_buf);
        buf_free (gref.ranges);
        buf_free (gref.iupacs_buf);
        buf_free (gref.ref_external_ra);
        buf_free (gref.ref_file_section_list);
        buf_free (gref.genome_is_set_buf);
        FREE (gref.ref_fasta_name);
        ref_lock_free();
        contigs_free (&gref.ctgs);
        gref.genome_nbases = 0;
    }
        
    buf_free (gref.region_to_set_list);
    buf_free (gref.stored_ra);

    gref.external_ref_is_loaded = false;
}

void ref_destroy_reference (void)
{
    if (flag.make_reference) 
        for_buf (Range, r, gref.ranges)
            FREE (r->ref.words);

    if (flag.show_cache && gref.genome_buf.type == BUF_SHM) 
        iprint0 ("show-cache: destroy genome_buf attached to shm\n");

    buflist_sort (evb, false);

    // locks stuff
    ref_lock_free();
    buf_destroy (gref.genome_muteces);
    buf_destroy (gref.genome_mutex_names);
    
    buf_destroy (gref.ranges);
    buf_destroy (gref.genome_buf);
    buf_destroy (gref.genome_is_set_buf);
    buf_destroy (gref.region_to_set_list);
    buf_destroy (gref.ref_external_ra);
    buf_destroy (gref.stored_ra);
    buf_destroy (gref.ref_file_section_list);
    buf_destroy (gref.iupacs_buf);
    FREE (gref.ref_fasta_name);

    refhash_destroy(); // must be before ref_cache_detach

    ref_cache_detach(); // after destroying genome_buf and refhash_buf

    // note: we keep gref.filename, in case it needs to be loaded again
    rom save_filename  = gref.filename;
    rom save_ctgs_name = gref.ctgs.name; 

    // ref contig stuff
    contigs_destroy (&gref.ctgs);

    memset (&gref, 0, sizeof (RefStruct));
    gref.filename  = save_filename;
    gref.ctgs.name = save_ctgs_name; 
}

void ref_finalize (bool also_free_filename)
{
    ref_destroy_reference(); 
    if (also_free_filename) FREE (gref.filename);
}

// PIZ: returns a range which is the entire contig
ConstRangeP ref_piz_get_range (VBlockP vb, FailType soft_fail)
{
    ASSERT0 (IS_PIZ, "this is a PIZ-side function");

    ASSERTISALLOCED (gref.ranges);

    // caching
    if (vb->prev_range && vb->prev_range_chrom_node_index == vb->chrom_node_index)
        return vb->prev_range;

    ASSPIZ0 (vb->chrom_node_index != WORD_INDEX_NONE, "Unexpectedly, vb->chrom_node_index=WORD_INDEX_NONE");

    // gets the index of the matching chrom in the reference - either its the chrom itself, or one with an alternative name
    // eg 'chr22' instead of '22'
    WordIndex ref_contig_index = chrom_2ref_piz_get (vb->chrom_node_index);

    ASSPIZ (ref_contig_index != WORD_INDEX_NONE || soft_fail, "Requested a reference range of a contig \"%s\" (%d) with no reference.", 
            ctx_get_words_snip (ZCTX(CHROM), vb->chrom_node_index), vb->chrom_node_index);
    if (ref_contig_index == WORD_INDEX_NONE) return NULL; // soft fail
    
    ASSPIZ (ref_contig_index < gref.ranges.len32 || soft_fail, "Expecting ref_contig_index=%d < gref.ranges.len=%"PRIu64 " in %s. FYI: ZCTX(CHROM)->chrom2ref_map.len=%"PRIu64, 
            ref_contig_index, gref.ranges.len, ref_get_filename(), ZCTX(CHROM)->chrom2ref_map.len); // 64 bit printing on error
    if (ref_contig_index >= gref.ranges.len32) return NULL; // soft fail

    RangeP r = B(Range, gref.ranges, ref_contig_index);
    ASSPIZ (r->ref.nwords || soft_fail, "No reference data for chrom=\"%s\"", vb->chrom_name);
    if (!r->ref.nwords) return NULL; // this can ligitimately happen if entire chromosome is verbatim in SAM, eg. unaligned (pos=4) or SEQ or CIGAR are unavailable

    vb->prev_range = r;
    vb->prev_range_chrom_node_index = vb->chrom_node_index;

    return r;
}

// -------------------------------------------------------------------------------------------------------
// PIZ: read and uncompress stored ranges (originally produced with --REFERENCE or SAM internal reference)
// -------------------------------------------------------------------------------------------------------

// PIZ: uncompact a region within ref - called by compute thread of reading the reference
static void ref_uncompact_ref (RangeP r, int64_t first_bit, int64_t last_bit, const Bits *compacted)
{
    uint64_t start_1_offset=first_bit, start_0_offset, len_1; // coordinates into r->is_set (in nucleotides)
    uint64_t next_compacted=0; // coordinates into compacted (in nucleotides)

    while (1) {
        // find length of set region
        bool has_any_bit = bits_find_next_clear_bit (&r->is_set, start_1_offset, &start_0_offset);
        if (!has_any_bit || start_0_offset > last_bit) 
            start_0_offset = last_bit + 1; // this is the last region of 1s

        len_1 = start_0_offset - start_1_offset;
        ASSERT (len_1 > 0, "len_1 is not positive: start_0_offset=%"PRId64" start_1_offset=%"PRId64" first_bit=%"PRId64" last_bit=%"PRId64, 
                start_0_offset, start_1_offset, first_bit, last_bit);

        // do actual uncompacting
        bits_copy (&r->ref, start_1_offset * 2, compacted, next_compacted * 2, len_1 * 2);

        next_compacted += len_1;

        if (start_0_offset > last_bit) break; // we're done (we always end with a region of 1s because we removed the flanking 0s during compacting)

        // skip the clear region
        has_any_bit = bits_find_next_set_bit (&r->is_set, start_0_offset, &start_1_offset); 
        ASSERT0 (has_any_bit, "cannot find next set bit");
        ASSERT (start_1_offset <= last_bit, "expecting start_1_offset(%"PRId64") <= last_bit(%"PRId64")",
                start_1_offset, last_bit); // we removed the flanking regions, so there is always an 1 after a 0 within the region
    }

    ASSERT (next_compacted * 2 == compacted->nbits, "expecting next_compacted(%"PRId64") * 2 == compacted->nbits(%"PRId64")",
            next_compacted, compacted->nbits);
}

// Compute thread: called by ref_uncompress_one_range
RangeP ref_get_range_by_chrom (WordIndex chrom, rom *chrom_name)
{
    decl_zctx (CHROM);
    ASSERTINRANGE (chrom, 0, zctx->word_list.len32);

    if (chrom_name)
        *chrom_name = ctx_get_words_snip (zctx, chrom);

    ASSERT (chrom < gref.ranges.len, "expecting chrom=%d < ranges.len=%"PRIu64, chrom, gref.ranges.len);
    
    RangeP r = B(Range, gref.ranges, chrom); // in PIZ, we have one range per chrom
    return r;
}

RangeP ref_get_range_by_ref_index (VBlockP vb, WordIndex ref_contig_index)
{
    if (ref_contig_index == WORD_INDEX_NONE || ref_contig_index >= gref.ranges.len) return NULL;

    return B(Range, gref.ranges, ref_contig_index); // in PIZ, we have one range per chrom
}

// Print this array to a file stream.  Prints '0's and '1'.  Doesn't print newline.
static void ref_print_bases (FILE *file, const Bits *bitarr, 
                             uint64_t start_base, uint64_t num_of_bases, bool is_forward)
{
    static const char fwd[2][2] = { { 'A', 'C' }, {'G', 'T'} };
    static const char rev[2][2] = { { 'T', 'G' }, {'C', 'A'} };

#define BASES_PER_LINE 100

    if (is_forward)
        for (uint64_t i=start_base*2; i < (start_base + num_of_bases)*2; i+=2) {
            if (!flag.sequential && (i-start_base*2) % (BASES_PER_LINE*2) == 0)
                fprintf (file, "%8"PRIu64": ", i/2);
            fputc (fwd[bits_get(bitarr, i+1)][bits_get(bitarr, i)], file);
            if (!flag.sequential && ((i-start_base*2) % (BASES_PER_LINE*2) == 2*(BASES_PER_LINE-1))) fputc ('\n', file);
        }
    
    else 
        for (int64_t i=(start_base+num_of_bases-1)*2; i >= start_base*2; i-=2) { // signed type
            fputc (rev[bits_get(bitarr, i+1)][bits_get(bitarr, i)], file);
            if (!flag.sequential && (((start_base+num_of_bases-1)*2-i) % (BASES_PER_LINE*2) == (BASES_PER_LINE-1)*2)) fputc ('\n', file);
        }
    
    fputc ('\n', file);
}

static void ref_show_sequence (void)
{
    for (uint32_t range_i=0; range_i < gref.ranges.len32; range_i++) {
        RangeP r = B(Range, gref.ranges, range_i);

        // get first pos and last pos, potentially modified by --regions
        PosType64 first_pos, last_pos;
        bool revcomp; // to do: implement
        if (!r->ref.nbits ||
            !regions_get_range_intersection (r->chrom, r->first_pos, r->last_pos, 0, &first_pos, &last_pos, &revcomp)) continue;

        if (r->ref.nbits) {
            iprintf ("%.*s\n", STRf (r->chrom_name));
            ref_print_bases (info_stream, &r->ref, first_pos-1, last_pos-first_pos, true);
        }
    }

    if (is_genocat) exit_ok;  // in genocat this, not the data
}

// entry point of compute thread of reference decompression. this is called when pizzing a file with a stored reference,
// including reading the reference file itself.
// vb->z_data contains a SEC_REFERENCE section and sometimes also a SEC_REF_IS_SET section
static void ref_uncompress_one_range (VBlockP vb)
{
    START_TIMER;

    if (!vb->z_data.len) goto finish; // we have no data in this VB because it was skipped due to --regions or genocat --show-headers

    SectionHeaderReferenceP header = (SectionHeaderReferenceP)B1ST8(vb->z_data);

    WordIndex chrom            = (WordIndex)BGEN32 (header->chrom_word_index);
    uint32_t uncomp_len        = BGEN32 (header->data_uncompressed_len);
    PosType64 ref_sec_pos      = (PosType64)BGEN64 (header->pos);
    PosType64 ref_sec_gpos     = (PosType64)BGEN64 (header->gpos); // this is equal to sec_start_gpos. However up to 12.0.3 we had a bug in case of compacted ranges in a SAM/BAM DENOVO reference with start flanking regions - GPOS in the section header of a didn't reflect the flanking removal, so header->gpos cannot be trusted as correct for older SAM/BAM DENOVO reference files
    PosType64 ref_sec_len      = (PosType64)BGEN32 (header->num_bases);
    PosType64 ref_sec_last_pos = ref_sec_pos + ref_sec_len - 1;
    PosType64 compacted_ref_len=0, initial_flanking_len=0, final_flanking_len=0; 

    ASSERT0 (chrom != WORD_INDEX_NONE, "Unexpected reference section with chrom=WORD_INDEX_NONE");

    rom chrom_name;
    RangeP r = ref_get_range_by_chrom (chrom, &chrom_name);
    ASSERT (r->last_pos, "unexpectedly, r->last_pos=0: r=%s", ref_display_range (r).s);

    PosType64 sec_start_within_contig = ref_sec_pos - r->first_pos;
    PosType64 sec_start_gpos          = r->gpos + sec_start_within_contig;
    PosType64 sec_end_within_contig   = sec_start_within_contig + ref_sec_len - 1;

    ASSERT (!VER(14) || ref_sec_gpos == sec_start_gpos, 
            "Expecting SEC_REFERENCE.gpos=%"PRIu64" == sec_start_gpos=%"PRIu64"(== r->gpos=%"PRIu64" + sec_start_within_contig=%"PRIu64")."
            "header=(chrom=%d pos=%"PRId64" gpos=%"PRId64" num_bases=%"PRId64") r=%s", 
            ref_sec_gpos, sec_start_gpos, r->gpos, sec_start_within_contig, 
            chrom, ref_sec_pos, ref_sec_gpos, ref_sec_len, ref_display_range (r).s);

    // In reference files generated up to 13.0.20 reference sections of small contigs might be out-of-order, 
    // and as a result gpos value is different in SEC_REFERENCE.gpos vs SEC_REF_CONTIGS 
    if (ref_sec_gpos != sec_start_gpos && IS_REF_EXTERNAL) 
        WARN_ONCE ("\nWARNING: reference file %s which was generated with Genozip version %u is affected by an old bug (defect-2022-08-21). "
                   "As a result, files compressed with this reference file MUST also be decompressed with it, and it is not possible to re-generate "
                   "this reference file if it is lost.\n\n", ref_get_filename(), z_file->genozip_version);
    
    bool is_compacted = HEADER_IS(REF_IS_SET); // we have a SEC_REF_IS_SET if SEC_REFERENCE was compacted

    if (flag.show_reference && primary_command == PIZ && r)  // in ZIP, we show the compression of SEC_REFERENCE into z_file, not the uncompression of the reference file
        iprintf ("vb_i=%u Uncompressing %-14s chrom=%u ('%.*s') gpos=%"PRId64" pos=%"PRId64" num_bases=%u comp_bytes=%u\n", 
                 vb->vblock_i, st_name (header->section_type), BGEN32 (header->chrom_word_index), STRf (r->chrom_name), BGEN64 (header->gpos), 
                 BGEN64 (header->pos), BGEN32 (header->num_bases), BGEN32 (header->data_compressed_len) + (uint32_t)sizeof (SectionHeaderReference));

    // initialization of is_set:
    // case 1: ZIP (reading an external reference) - we CLEAR is_set, and let seg set the bits that are to be
    //    needed from the reference for pizzing (so we don't store the others)
    //    note: in case of ZIP with REF_INTERNAL, we CLEAR the bits in ref_seg_get_range
    // case 2: PIZ, reading an uncompacted (i.e. complete) reference section - which is always the case when
    //    reading an external reference and sometimes when reading a stored one - we SET all the bits as they are valid for pizzing
    //    we do this in ref_load_stored_reference AFTER all the SEC_REFERENCE/SEC_REF_IS_SEC sections are uncompressed,
    //    so that in case this was an REF_EXT_STORE compression, we first copy the contig-wide IS_SET sections (case 3) 
    //    (which will have 0s in the place of copied FASTA sections), and only after do we set these regions to 1.
    //    note: in case of PIZ, entire contigs are initialized to clear in ref_initialize_ranges as there might be
    //    regions missing (not covered by SEC_REFERENCE sections)
    // case 3: PIZ, reading a compacted reference - we receive the correct is_set in the SEC_REF_IS_SET section and don't change it


    // case: if compacted, this SEC_REF_IS_SET sections contains r->is_set and its first/last_pos contain the coordinates
    // of the range, while the following SEC_REFERENCE section contains only the bases for which is_set is 1, 
    // first_pos=0 and last_pos=(num_1_bits_in_is_set-1)
    if (is_compacted) {

        // if compacted, the section must be within the boundaries of the contig (this is not true if the section was copied with ref_copy_one_compressed_section)
        ASSERT (sec_start_within_contig >= 0 && ref_sec_last_pos <= r->last_pos, 
                "section range out of bounds for chrom=%d \"%s\": in SEC_REFERENCE being uncompressed: first_pos=%"PRId64" last_pos=%"PRId64" but in reference contig as initialized: first_pos=%"PRId64" last_pos=%"PRId64,
                chrom, r->chrom_name, ref_sec_pos, ref_sec_last_pos, r->first_pos, r->last_pos);

        ASSERT (uncomp_len == roundup_bits2bytes64 (ref_sec_len), "when uncompressing SEC_REF_IS_SET: uncomp_len=%u inconsistent with ref_sec_len=%"PRId64" (roundup_bits2bytes64 (ref_sec_len)=%"PRId64")", 
                uncomp_len, ref_sec_len, roundup_bits2bytes64 (ref_sec_len)); 

        // uncompress into r->is_set, via vb->scratch
        ASSERTNOTINUSE (vb->scratch);
        zfile_uncompress_section (vb, (SectionHeaderP)header, &vb->scratch, "scratch", 0, SEC_REF_IS_SET);

        Bits *is_set = buf_zfile_buf_to_bits (&vb->scratch, ref_sec_len);

        // note on locking: while different threads uncompress regions of the range that are non-overlapping, 
        // there might be a 64b word that is split between two ranges
        RefLock lock = ref_lock (sec_start_gpos, ref_sec_len + 63); // +63 to ensure lock covers entire last word
    
        bits_copy (&r->is_set, sec_start_within_contig, is_set, 0, ref_sec_len); // initialization of is_set - case 3
        ref_unlock (&lock);

        buf_free (vb->scratch);

        // display contents of is_set if user so requested
        if (flag.show_is_set && !strcmp (chrom_name, flag.show_is_set)) 
            ref_print_is_set (r, -1, info_stream);

        // prepare for uncompressing the next section - which is the SEC_REFERENCE
        header = (SectionHeaderReferenceP)B8(vb->z_data, *B32(vb->z_section_headers, 1));

        if (flag.show_reference && primary_command == PIZ && r) 
            iprintf ("vb_i=%u Uncompressing %-14s chrom=%u ('%.*s') gpos=%"PRId64" pos=%"PRId64" num_bases=%u comp_bytes=%u\n", 
                     vb->vblock_i, st_name (header->section_type), BGEN32 (header->chrom_word_index), STRf (r->chrom_name), BGEN64 (header->gpos), 
                     BGEN64 (header->pos), BGEN32 (header->num_bases), BGEN32 (header->data_compressed_len) + (uint32_t)sizeof (SectionHeaderReference));

        compacted_ref_len  = (PosType64)BGEN32(header->num_bases);
        uncomp_len         = BGEN32 (header->data_uncompressed_len);

        ASSERT (uncomp_len == roundup_bits2bytes64 (compacted_ref_len*2), 
                "uncomp_len=%u inconsistent with compacted_ref_len=%"PRId64, uncomp_len, compacted_ref_len); 

        ASSERT0 (BGEN32 (header->chrom_word_index) == chrom && BGEN64 (header->pos) == ref_sec_pos && BGEN64 (header->gpos) == ref_sec_gpos, // chrom should be the same between the two sections
                 "header mismatch between SEC_REF_IS_SET and SEC_REFERENCE sections");
    }
    
    // case: not compacted means that entire range is set
    else {
        ASSERT (uncomp_len == roundup_bits2bytes64 (ref_sec_len*2), "uncomp_len=%u inconsistent with ref_len=%"PRId64, uncomp_len, ref_sec_len); 

        if (primary_command == ZIP && IS_REF_EXT_STORE) { // initialization of is_set - case 1
            RefLock lock = ref_lock (sec_start_gpos, ref_sec_len + 63); // +63 to ensure lock covers entire last word
            bits_clear_region (&r->is_set, sec_start_within_contig, ref_sec_len); // entire range is cleared
            ref_unlock (&lock);
        }

        else if (primary_command == PIZ && ref_has_is_set()) { // initialization of is_set - case 2

            // it is possible that the section goes beyond the boundaries of the contig, this can happen when we compressed with --REFERENCE
            // and the section was copied in its entirety from the .ref.genozip file (in ref_copy_one_compressed_section)
            // even though a small amount of flanking regions is not set. in this case, we copy from the section only the part needed
            initial_flanking_len = (sec_start_within_contig < 0)    ? -sec_start_within_contig       : 0; // nucleotides in the section that are before the start of our contig
            final_flanking_len   = (ref_sec_last_pos > r->last_pos) ? ref_sec_last_pos - r->last_pos : 0; // nucleotides in the section that are after the end of our contig

            uint64_t start = MAX_(sec_start_within_contig, 0);
            uint64_t len   = ref_sec_len - initial_flanking_len - final_flanking_len;
            ASSERT (IN_RANGX (len, 0, ref_sec_len), "expecting ref_sec_len=%"PRIu64" >= initial_flanking_len=%"PRIu64" + final_flanking_len=%"PRIu64,
                    ref_sec_len, initial_flanking_len, final_flanking_len);

            RefLock lock = ref_lock (start + r->gpos, len + 63); 
            bits_set_region (&r->is_set, start, len);
            ref_unlock (&lock);

            if (flag.debug) {
                // save the region we need to set, we will do the actual setting in ref_load_stored_reference
                spin_lock (gref.region_to_set_list_spin);
                RegionToSet *rts = &BNXT (RegionToSet, gref.region_to_set_list);
                spin_unlock (gref.region_to_set_list_spin);
                rts->is_set    = &r->is_set;
                rts->first_bit = MAX_(sec_start_within_contig, 0);
                rts->len       = ref_sec_len - initial_flanking_len - final_flanking_len;
            }
        }
   
        if (!uncomp_len) return;  // empty header - if it appears, it is the final header (eg in case of an unaligned SAM file)
    }

    // uncompress into r->ref, via vb->scratch
    ASSERTNOTINUSE (vb->scratch);
    zfile_uncompress_section (vb, (SectionHeaderP)header, &vb->scratch, "scratch", 0, SEC_REFERENCE);

    // lock - while different threads uncompress regions of the range that are non-overlapping, they might overlap at the bit level
    RefLock lock;
    if (is_compacted) {
        const Bits *compacted = buf_zfile_buf_to_bits (&vb->scratch, compacted_ref_len * 2);

        lock = ref_lock (sec_start_gpos, ref_sec_len + 63); // +63 to ensure lock covers entire last word
    
        ref_uncompact_ref (r, sec_start_within_contig, sec_end_within_contig, compacted);
    }

    else {
        BitsP ref = buf_zfile_buf_to_bits (&vb->scratch, ref_sec_len * 2);

        lock = ref_lock (sec_start_gpos, ref_sec_len + 63); // +63 to ensure lock covers entire last word

        // copy the section, excluding the flanking regions
        bits_copy (&r->ref, MAX_(sec_start_within_contig, 0) * 2, // dst
                   ref, initial_flanking_len * 2, // src
                   (ref_sec_len - initial_flanking_len - final_flanking_len) * 2); // len
    }

    ref_unlock (&lock);

    buf_free (vb->scratch);

finish:
    vb_set_is_processed (vb); // tell dispatcher this thread is done and can be joined. 

    if (flag.debug_or_test) buflist_test_overflows(vb, __FUNCTION__); 

    COPY_TIMER (ref_uncompress_one_range);
}

static Section sec = NULL; // NULL -> first call to this sections_get_next_ref_range() will reset cursor 
static void ref_read_one_range (VBlockP vb)
{
    START_TIMER;

    if (!sections_next_sec2 (&sec, SEC_REFERENCE, SEC_REF_IS_SET)) return;  // no more reference sections
        
    unsigned header_len = (Z_DT(SAM) && z_file->z_flags.dts_ref_internal) ? crypt_padded_len (sizeof (SectionHeaderReference)) // REF_INTERNAL sections are encrypted if --password
                                                                          : sizeof (SectionHeaderReference); // data originating from a reference file is never encrypted
    if (((sec+1)->offset - sec->offset) == header_len) return; // final, terminating header-only section sometimes exists (see ref_compress_ref)
    
    // note since v14 - sec->vblock_i != vb->vblock_i because REFERENCE sections can be written out-of-order

    // if the user specified --regions, check if this ref range is needed
    bool range_is_included = true;
    RAEntry *ra = NULL;
    if (IS_PIZ && flag.regions &&
        !(Z_DT(REF) || Z_DT(SAM) || Z_DT(BAM) || Z_DT(FASTQ))) { // SAM/BAM/FASTQ: we need the reference even for excluded contigs, so recon can consume FASTQ_SEQMIS/SAM_SEQMIS (also always when reading external referene, as we don't yet know which files it will be use for)
        if (sec->vblock_i > gref.stored_ra.len32) return; // we're done - no more ranges to read, per random access (this is the empty section)

        ra = B(RAEntry, gref.stored_ra, vb->vblock_i-1);
        ASSERT (ra->vblock_i == sec->vblock_i, "expecting ra->vblock_i(%u) == sec->vblock_i(%u)", ra->vblock_i, sec->vblock_i);

        range_is_included = regions_is_ra_included (ra);
    }

    if (range_is_included) { 
        buf_alloc (vb, &vb->z_section_headers, 0, 2, int32_t, 0, "z_section_headers"); // room for 2 section headers  
        ASSERT0 (vb->z_section_headers.len < 2, "unexpected 3rd recursive entry");

        BNXT (int32_t, vb->z_section_headers) = zfile_read_section (z_file, vb, sec->vblock_i, &vb->z_data, "z_data", sec->st, sec);
    }

    // if this is SEC_REF_IS_SET, read the SEC_REFERENCE section now (even if its not included - we need to advance the cursor)
    if (sec->st == SEC_REF_IS_SET) 
        ref_read_one_range (vb);

    if (flag.only_headers) 
        vb->z_data.len = 0; // roll back if we're only showing headers

    vb->dispatch = READY_TO_COMPUTE; // to simplify the code, we will dispatch the thread even if we skip the data, but we will return immediately. 

    if (flag.debug_or_test) buflist_test_overflows(vb, __FUNCTION__); 

    COPY_TIMER (ref_read_one_range);
}

// PIZ: loading a reference stored in the genozip file - this could have been originally stored as REF_INTERNAL or REF_EXT_STORE
// or this could be a .ref.genozip file (called from load_external->piz_one_txt_file, if reference is not cached yet)
bool ref_load_stored_reference (void)
{
    START_TIMER;

    ASSERTNOTINUSE (gref.ranges);

    if (!flag.only_headers)
        ref_initialize_ranges (RT_LOADED);

    sec = NULL; // NULL -> first call to this sections_get_next_ref_range() will reset cursor 

    if (primary_command == PIZ && flag.debug) {
        spin_initialize (gref.region_to_set_list_spin);
        buf_alloc (evb, &gref.region_to_set_list, 0, sections_count_sections (SEC_REFERENCE), RegionToSet, 1, "region_to_set_list");
    }

    // decompress reference using Dispatcher
    bool external = (bool)(flag.reference != REF_STORED);
    bool populating = ref_cache_is_populating();

    // If not already present in shm, load the reference
    bool loaded_reference = populating || !ref_cache_is_cached();
    if (loaded_reference) { 
        dispatcher_fan_out_task ("load_ref", NULL, 
                                 0, external ? (populating ? "Caching reference file" : "Reading reference file") : NULL, // same message as in refhash_load
                                 true, flag.test, false, 0, 100, true,
                                 ref_read_one_range, 
                                 ref_uncompress_one_range, 
                                 NO_CALLBACK);

        if (flag.show_cache) iprint0 ("show-cache: done reading genome from disk\n");
    }

    // calculate in-memory digest of loaded genome, and compare it to the value calculated by
    // --make-reference, stored in GENOZIP_HEADER.genome_digest
    if (flag.reading_reference && VER(15)) {
        START_TIMER;
        Digest digest = digest_do (STRb(gref.genome_buf), gref.is_adler, "genome"); 
        COPY_TIMER_EVB (ref_load_digest);

        // verify that digest of genome is memory is as calculated by make-reference (since v15). If not, its a bug.
        if (!digest_is_equal (digest, gref.genome_digest)) {
            if (!ref_cache_is_cached()) 
                ABORT ("Bad reference file: In-memory digest of genome is %s, different than calculated by make-reference: %s",
                       digest_display_(digest, gref.is_adler).s, digest_display_(gref.genome_digest, gref.is_adler).s);

            // case: bad existing cache. this should never happen as this condition ^ should prevent cache from being
            // marked as "is_populated". This is just for extra safety.
            else {
                ref_cache_remove_do (true, false);
                ABORTINP ("Error: Found bad reference cached in memory. It has now been removed. Please try again. %s", report_support());
            }
        }
        else
            if (flag.show_cache) iprint0 ("show-cache: verified genome digest\n");
    }

    // if we're populating the cache, also load refhash (even if not needed for compressing current file)
    if (populating) {
        flag.aligner_available = true; // so that emoneg is generated too - to keep consistency that refhash and emoneg exist or don't exist together
        
        refhash_load();

        // finalizing loading the genome and refhash, and detaching read-write shm.
        ref_cache_done_populating(); 
        int64_t delta_bytes = gref.cache->genome_data - gref.genome_buf.data; // address delta

        // re-attach to read-only shm
        buf_attach_bits_to_shm (evb, &gref.genome_buf, gref.cache->genome_data, gref.genome_nbases * 2, "genome_buf");
        if (flag.show_cache) iprintf ("show-cache: re-attached genome_buf (%"PRIu64" bases) to READONLY shm\n", gref.genome_nbases);
        reoverlay_ranges_on_loaded_genome (delta_bytes); 
        
        buf_attach_to_shm (evb, &refhash_buf, gref.cache->genome_data + gref.genome_buf.size, refhash_buf.len, "refhash_buf");
        refhash_buf.len = refhash_buf.size;
        if (flag.show_cache) iprintf ("show-cache: re-attached refhash_buf (len=%"PRIu64") to READONLY shm\n", refhash_buf.len);
        refhash_update_layers (delta_bytes);
    }

    // case: using REF_EXT_STORE with a cached reference file: generate a private genome, as it will be compacted for writing to SEC_REFERENCE
    if (flag.reading_reference && IS_REF_EXT_STORE && gref.genome_buf.type == BUF_SHM) {
        buf_free (gref.genome_buf); // free SHM buffer
        buf_alloc_bits_exact (evb, &gref.genome_buf, gref.genome_nbases * 2, NOINIT, 0, "gref.genome_buf");

        memcpy (B1ST8(gref.genome_buf), gref.cache->genome_data, gref.genome_buf.nwords * sizeof(uint64_t));

        if (flag.show_cache) iprint0 ("show-cache: REF_EXT_STORE: allocating genome_buf and copying genome from shm into it\n");

        // re-overlay the ranges on the writeable copy of the genome
        reoverlay_ranges_on_loaded_genome (gref.genome_buf.data - gref.cache->genome_data); 
    }

    if (flag.only_headers) return loaded_reference;

    if (flag.show_ref_seq) ref_show_sequence();

    if (flag.show_ranges) ref_display_all_ranges();
    
    if (primary_command == PIZ) {
        if (flag.debug) {
            // in DEBUG, we will use is_set for verifying reconstruction accesses only is_set bases
            // now we can safely set the is_set regions originating from non-compacted ranges. we couldn't do it before, because
            // copied-from-FASTA ranges appear first in the genozip file, and after them could be compacted ranges that originate
            // from a full-contig range in EXT_STORE, whose regions copied-from-FASTA are 0s.
            ARRAY (RegionToSet, rts, gref.region_to_set_list);
            for (uint32_t i=0; i < gref.region_to_set_list.len32; i++)
                bits_set_region (rts[i].is_set, rts[i].first_bit, rts[i].len);
        }

        else {
            buf_free (gref.genome_is_set_buf); 
            gref.genome_is_set = NULL;

            for_buf (Range, r, gref.ranges) 
                memset (&r->is_set, 0, sizeof (r->is_set)); // un-overlay
        }
    }

    COPY_TIMER_EVB (ref_load_stored_reference);

    return loaded_reference;
}

// ------------------------------------
// ZIP side
// ------------------------------------

// ZIP: returns a range that includes pos and is locked for (pos,len)
// case 1: ZIP: in SAM with REF_INTERNAL, when segging a SEQ field ahead of committing it to the reference
// case 2: ZIP: SAM and VCF with REF_EXTERNAL: when segging a SAM_SEQ or VCF_REFALT field
// if range is found, returns a locked range, and its the responsibility of the caller to unlock it. otherwise, returns NULL
RangeP ref_seg_get_range (VBlockP vb, WordIndex chrom, STRp(chrom_name), 
                          PosType64 pos, uint32_t ref_consumed, 
                          WordIndex ref_index, // if known (mandatory if not chrom), WORD_INDEX_NONE if not                                
                          RefLock *lock) // optional if RT_LOADED
{
    // sanity checks
    ASSERT0 (vb->chrom_name, "vb->chrom_name=NULL");
    
    ASSERT0 (IS_ZIP, "this is a ZIP-side function");

    // case: no external reference and no header contigs
    ASSINP (gref.ranges.rtype, "No contigs specified in the %s header, use --reference", z_dt_name());
             
    if (ref_index == WORD_INDEX_NONE)
        ref_index = IS_REF_INTERNAL ? chrom : chrom_2ref_seg_get (vb, chrom);

    if (ref_index == WORD_INDEX_NONE) return NULL;

    ASSSEG (ref_index < gref.ranges.len32, "ref_index=%d out of range: gref.ranges.len=%u", ref_index, gref.ranges.len32);
    RangeP range = B(Range, gref.ranges, ref_index);

    // when using an external refernce, pos has to be within the reference range
    // note: in SAM, if a read starts within the valid range, it can overflow beyond it if its a circular chromosome. 
    // we even observed BAM files in the wild with chrM POS=16572 (with the contig len = 16571) - which is why this is a warning and not an error
    if (pos < range->first_pos || (pos + ref_consumed - 1) > range->last_pos) {
        if (pos > range->last_pos) // warn only if entire SEQ is outside of range - i.e. don't warn for a read at the end of a circular contig, but still return NULL
            WARN_ONCE ("FYI: %s: contig=\"%.*s\"(%u) pos=%"PRIu64" ref_consumed=%u exceeds contig.last_pos=%"PRIu64". This might be an indication that the wrong reference file is being used (this message will appear only once)",
                    LN_NAME, STRf(range->chrom_name), ref_index, pos, ref_consumed, range->last_pos);
        return NULL;
    }

    if (lock) {        
        PosType64 gpos = range->gpos + (pos - range->first_pos);
        *lock = ref_lock (gpos, ref_consumed);
    }

    vb->prev_range = range;
    vb->prev_range_chrom_node_index = chrom; // the chrom that started this search, leading to this range

    return range; 
}

// ----------------------------------------------
// Compressing ranges into SEC_REFERENCE sections
// ----------------------------------------------

// ZIP main thread
static void ref_copy_one_compressed_section (FileP ref_file, const RAEntry *ra)
{
    ASSERTNOTINUSE (evb->scratch);

    // get section list entry from ref_file_section_list - which will be used by zfile_read_section to seek to the correct offset
    CLEAR_FLAG (show_headers);

    ASSERT (ra->vblock_i <= gref.ref_file_section_list.len32, "ra->vblock_i=%d, but reference file has only %u vb_i's",
            ra->vblock_i, gref.ref_file_section_list.len32);

    Section sec = B(SectionEnt, gref.ref_file_section_list, ra->vblock_i-1); // buffer contains all, and only, SEC_REFERENCE sections and is sorted by vblock_i
    zfile_read_section (ref_file, evb, ra->vblock_i, &evb->scratch, "scratch", SEC_REFERENCE, sec);

    RESTORE_FLAG (show_headers);

    SectionHeaderReferenceP header = (SectionHeaderReferenceP)B1ST8(evb->scratch);
    header->magic = BGEN32 (GENOZIP_MAGIC); // restore magic (it was changed to section_i after reading)

    WordIndex ref_index = BGEN32 (header->chrom_word_index);
    ASSERT0 (ref_index == ra->chrom_index && BGEN64 (header->pos) == ra->min_pos, "RA and Section don't agree on chrom or pos");

    // update header->chrom_word_index, currently refering to gref.contigs, to refer to ZCTX(CHROM)
    STR(contig_name);
    contig_name = ref_contigs_get_name (ref_index, &contig_name_len);

    WordIndex chrom_word_index = chrom_get_by_name (STRa(contig_name));
    ASSERT (chrom_word_index != WORD_INDEX_NONE, "Cannot find reference contig \"%.*s\" in CHROM context", STRf(contig_name));

    header->chrom_word_index = BGEN32 (chrom_word_index);

    // some minor changes to the header...
    header->vblock_i = 0; // we don't belong to any VB and there is no encryption of external ref

    // "manually" add the reference section to the section list - normally it is added in comp_compress()
    sections_add_to_list (evb, (SectionHeaderP)header);

    // Write header and body of the reference to z_file
    // Note on encryption: reference sections originating from an external reference are never encrypted - not
    // by us here, and not in the source reference fasta (because with disallow --make-reference in combination with --password)
    zriter_write (&evb->scratch, &evb->section_list, -1, true);

    if (flag.show_reference) {
        decl_zctx (CHROM);
        CtxNodeP node = B(CtxNode, zctx->nodes, BGEN32 (header->chrom_word_index));
        iprintf ("Copying SEC_REFERENCE from %s: chrom=%u (%s) gpos=%"PRId64" pos=%"PRId64" num_bases=%u section_size=%u\n", 
                 gref.filename, BGEN32 (header->chrom_word_index), 
                 Bc (zctx->dict, node->char_index), 
                 BGEN64 (header->gpos), BGEN64 (header->pos), 
                 BGEN32 (header->num_bases), 
                 BGEN32 (header->data_compressed_len) + st_header_size(SEC_REFERENCE));
    }

    buf_free (evb->scratch);
}

// ZIP copying parts of external reference to fine - called by main thread from zip_write_global_area->ref_compress_ref
static void ref_copy_compressed_sections_from_reference_file (void)
{
    START_TIMER;

    ASSERT (primary_command == ZIP && IS_REF_EXT_STORE, 
            "not expecting to be here: primary_command=%u flag.reference=%u", primary_command, flag.reference);

    FileP ref_file = file_open_z_read (gref.filename);

    // note: in a FASTA file compressed with --make-reference, there is exactly one RA per VB (a contig or part of a contig)
    // and, since this is ZIP with EXT_STORE, also exactly one range per contig. We loop one RA at a time and:
    // 1. If 95% of the ref file RA is set in the zfile contig range - we copy the compressed reference section directly from the ref FASTA
    // 2. If we copied from the FASTA, we mark those region covered by the RA as "is_set=0", so that we don't compress it later
    ARRAY (RAEntry, sec_reference, gref.ref_external_ra);

    chrom_index_by_name (CHROM);

    // note: use 'genocat --show-index <file.ref.genozip>' to see ref_external_ra
    for (uint32_t i=0; i < gref.ref_external_ra.len32; i++) {

        RangeP contig_r = B(Range, gref.ranges, sec_reference[i].chrom_index);
        PosType64 SEC_REFERENCE_start_in_contig_r = sec_reference[i].min_pos - contig_r->first_pos; // the start of the SEC_REFERENCE section (a bit less than 1MB) within the full-contig range

        PosType64 SEC_REFERENCE_len = sec_reference[i].max_pos - sec_reference[i].min_pos + 1;
        PosType64 bits_is_set = contig_r->is_set.nbits ? bits_num_set_bits_region (&contig_r->is_set, SEC_REFERENCE_start_in_contig_r, SEC_REFERENCE_len) : 0;

        // if this at least 95% of the RA is covered, just copy the corresponding FASTA section to our file, and
        // mark all the ranges as is_set=false indicating that they don't need to be compressed individually
        if ((float)bits_is_set / (float)SEC_REFERENCE_len >= 0.95) {
            ref_copy_one_compressed_section (ref_file, &sec_reference[i]);
            bits_clear_region (&contig_r->is_set, SEC_REFERENCE_start_in_contig_r, SEC_REFERENCE_len);

            if (contig_r->num_set != -1) {
                ASSERT (bits_is_set <= contig_r->num_set, "Expecting bits_is_set=%"PRId64" <= %u(%.*s)->num_set=%"PRId64,
                        bits_is_set, contig_r->chrom, STRf(contig_r->chrom_name), contig_r->num_set);
                
                contig_r->num_set -= bits_is_set;
            }
        }
    }

    file_close (&ref_file);

    zriter_wait_for_bg_writing(); // complete writing copied sections before moving on

    COPY_TIMER_EVB (ref_copy_compressed_sections_from_reference_file);
}

// remove the unused parts of a range and the beginning and end of the range, and update first/last_pos.
static bool ref_remove_flanking_regions (RangeP r, uint64_t *start_flanking_region_len /* out */)
{
// threshold - if the number of clear bits (excluding flanking regions) is beneath this, we will not copmact, as the cost in
// z_file size of a SEC_REF_IS_SET section needed if compacting will be more than what we save in compression of r->ref
#define THRESHOLD_FOR_COMPACTING 470 

    uint64_t end_flanking_region_len, last_1;
    
    // note: ref_prepare_range_for_compress is responsible not to send us 0-bit ranges
    bool has_any_bit = bits_find_first_set_bit (&r->is_set, start_flanking_region_len);

    char bits[65];
    ASSERT (has_any_bit, "range %u (%s) has no bits set in r->is_set but r->num_set=%"PRId64" (r->is_set.nbits=%"PRIu64"). is_set(first 64 bits)=%s", 
            BNUM (gref.ranges, r), r->chrom_name, r->num_set, r->is_set.nbits, bits_to_substr (&r->is_set, 0, 64, bits)); 

    has_any_bit = bits_find_prev_set_bit (&r->is_set, r->is_set.nbits, &last_1);
    ASSERT (has_any_bit, "range %u (%s) has no bits set in r->is_set (#2)", BNUM (gref.ranges, r), r->chrom_name); // this should definitely never happen, since we already know the range has bits
    end_flanking_region_len = r->is_set.nbits - last_1 - 1;

    uint64_t num_clear_bits_excluding_flanking_regions = 
        r->is_set.nbits - r->num_set - *start_flanking_region_len - end_flanking_region_len;

    // remove flanking regions - will allow a smaller allocation for the reference in PIZ 
    r->gpos      += *start_flanking_region_len;
    r->first_pos += *start_flanking_region_len;
    r->last_pos  -= end_flanking_region_len;

    ASSERT (r->last_pos >= r->first_pos, "bad removal of flanking regions: r->first_pos=%"PRId64" r->last_pos=%"PRId64,
            r->first_pos, r->last_pos);

    bits_remove_flanking (&r->is_set, *start_flanking_region_len, end_flanking_region_len);

    // if all we're doing is removing the flanking regions, we update ref now. if not, ref_compact_ref will update it
    bool is_compact_needed = num_clear_bits_excluding_flanking_regions >= THRESHOLD_FOR_COMPACTING;
    if (!is_compact_needed) 
        bits_remove_flanking (&r->ref, *start_flanking_region_len * 2, end_flanking_region_len * 2);

    // return true if compacting is needed
    return is_compact_needed;
}

// we compact one range by squeezing together all the bases that have is_set=1. return true if compacted
static bool ref_compact_ref (RangeP r)
{
    if (!r || !r->num_set) return false;

    ASSERT0 (r->is_set.nbits, "r->is_set.nbits=0");

    ASSERT (r->num_set >= 0 && r->num_set <= r->is_set.nbits, "range %u (%s) r->num_set=%"PRId64" ∉ [0,%"PRIu64"]",
            BNUM (gref.ranges, r), r->chrom_name, r->num_set, r->is_set.nbits); 

    // remove flanking regions
    uint64_t start_flanking_region_len;
    bool is_compact_needed = ref_remove_flanking_regions (r, &start_flanking_region_len);
    if (!is_compact_needed) return false;

    uint64_t start_1_offset=0, start_0_offset=0, compact_len=0;
    while (1) {
        
        // find length of set region
        bool has_any_bit = bits_find_next_clear_bit (&r->is_set, start_1_offset, &start_0_offset);
        uint64_t len_1 = (has_any_bit ? start_0_offset : r->is_set.nbits) - start_1_offset;

        // do actual compacting - move set region to be directly after the previous set region (or at the begining if its the first)
        bits_copy (&r->ref, compact_len * 2, &r->ref, (start_flanking_region_len + start_1_offset) * 2, len_1 * 2);
        compact_len += len_1;

        if (!has_any_bit) break; // case: we're done- this 1 region goes to the end the range - there are no more clear regions

        // find length of clear region
        has_any_bit = bits_find_next_set_bit (&r->is_set, start_0_offset, &start_1_offset);
        ASSERT0 (has_any_bit, "cannot find set bits"); // this should never happen as we removed the flanking regions
    }

    // set length of ref - this is the data that will be compressed
    r->ref.nbits  = compact_len * 2;
    r->ref.nwords = roundup_bits2words64 (r->ref.nbits); 

    return true;
}

static void ref_compress_one_range (VBlockP vb)
{
    START_TIMER;

    RangeP r = vb->range; // will be NULL if we're being asked to write a final, empty section

    // remove flanking regions, and if beneficial also compact it further by removing unused nucleotides 
    bool is_compacted = flag.make_reference ? false : ref_compact_ref (r); // true if it is compacted beyong just the flanking regions

    // get the index into the ZCTX(CHROM) dictionary
    WordIndex chrom_word_index = !r               ? WORD_INDEX_NONE
                               : IS_REF_CHROM2REF ? *B(WordIndex, ZCTX(CHROM)->ref2chrom_map, r->chrom)
                               :                  r->chrom;

    ASSERT (!r || chrom_word_index != WORD_INDEX_NONE, "Range %s invalidly has chrom_word_index==WORD_INDEX_NONE",
            ref_display_range (r).s);

    SectionHeaderReference header = { .vblock_i          = BGEN32 (vb->vblock_i),
                                      .magic             = BGEN32 (GENOZIP_MAGIC),
                                      .chrom_word_index  = BGEN32 (chrom_word_index),
                                      .pos               = r ? BGEN64 (r->first_pos) : 0,
                                      .gpos              = r ? BGEN64 (r->gpos)      : 0 };

    vb->z_data.param = vb->vblock_i;

    // First, SEC_REF_IS_SET section (but not needed if the entire range is used)
    if (r && is_compacted) {

        LTEN_bits (&r->is_set);

        header.section_type          = SEC_REF_IS_SET;  // most of the header is the same as ^
        header.codec                 = CODEC_BZ2;
        header.data_uncompressed_len = BGEN32 (r->is_set.nwords * sizeof (uint64_t));
        header.num_bases             = BGEN32 ((uint32_t)ref_size (r)); // full length, after flanking regions removed
        comp_compress (vb, NULL, &vb->z_data, &header, (char *)r->is_set.words, NO_CALLBACK, "SEC_REF_IS_SET");

        if (flag.show_reference && r) 
            iprintf ("vb_i=%u Compressing SEC_REF_IS_SET chrom=%u (%.*s) gpos=%"PRIu64" pos=%"PRIu64" num_bases=%u section_size=%u bytes\n", 
                     vb->vblock_i, BGEN32 (header.chrom_word_index), STRf (r->chrom_name),
                     BGEN64 (header.gpos), BGEN64 (header.pos), BGEN32 (header.num_bases), 
                     BGEN32 (header.data_compressed_len) + (uint32_t)sizeof (SectionHeaderReference));
    }

    // Second, SEC_REFERENCE
    if (r) LTEN_bits (&r->ref);

    header.section_type          = SEC_REFERENCE;
    header.codec                 = (flag.make_reference || flag.fast) ? CODEC_RANB : CODEC_LZMA; // LZMA compresses a bit better, but RANS decompresses *much* faster, so better for reference files
    header.data_uncompressed_len = r ? BGEN32 (r->ref.nwords * sizeof (uint64_t)) : 0;
    header.num_bases             = r ? BGEN32 (r->ref.nbits / 2) : 0; // less than ref_size(r) if compacted
    comp_compress (vb, NULL, &vb->z_data, &header, r ? (char *)r->ref.words : NULL, NO_CALLBACK, "SEC_REFERENCE");

    if (flag.show_reference && r) 
        iprintf ("vb_i=%u Compressing SEC_REFERENCE chrom=%u (%.*s) %s gpos=%"PRIu64" pos=%"PRIu64" num_bases=%u section_size=%u bytes\n", 
                 vb->vblock_i, BGEN32 (header.chrom_word_index), STRf (r->chrom_name), is_compacted ? "compacted " : "",
                 BGEN64 (header.gpos), BGEN64 (header.pos), BGEN32 (header.num_bases), 
                 BGEN32 (header.data_compressed_len) + (uint32_t)sizeof (SectionHeaderReference));

    // store the stored_ra data for this range
    if (r) {
        ASSERT (r->chrom >= 0, "Invalid r->chrom=%d for contig \"%.*s\"", r->chrom, STRf (r->chrom_name));

        spin_lock (gref.stored_ra_spin);
        BNXT (RAEntry, gref.stored_ra) = (RAEntry){ .vblock_i    = vb->vblock_i, 
                                                     .chrom_index = IS_REF_EXT_STORE ? *B(WordIndex, ZCTX(CHROM)->ref2chrom_map, r->chrom) : r->chrom,
                                                     .min_pos     = r->first_pos,
                                                     .max_pos     = r->last_pos };
        spin_unlock (gref.stored_ra_spin);
    }

    // insert this range sequence into the ref_hash (included in the reference file, for use to compress of FASTQ, unaligned SAM and FASTA)
    if (flag.make_reference)
        refhash_calc_one_range (vb, r, BISLST (gref.ranges, r) ? NULL : r+1);

    vb_set_is_processed (vb); // tell dispatcher this thread is done and can be joined.

    COPY_TIMER(ref_compress_one_range);
}

// main thread: compress the reference - one section per range, using Dispatcher to do them in parallel 
// note: this is not called in make_reference - instead, ref_make_prepare_one_range_for_compress is called
static void ref_prepare_range_for_compress (VBlockP vb)
{
    static uint32_t next_range_i=0;
    if (vb->vblock_i == 1) next_range_i = 0;

    // find next occupied range
    for (; next_range_i < gref.ranges.len ; next_range_i++) {
        RangeP r = B(Range, gref.ranges, next_range_i);

        if (r->num_set == -1)  // calculate num_set if not already calculated
            r->num_set = bits_num_set_bits (&r->is_set);

        if (r->num_set) {
            vb->range    = r; // range to compress
            vb->dispatch = READY_TO_COMPUTE;
            next_range_i++;
            return;
        }

        r->is_set.nbits = 0; // nothing to with this range - perhaps copied and cleared in ref_copy_compressed_sections_from_reference_file
    }
    
    vb->dispatch = DATA_EXHAUSTED;
}

// ZIP: compress and write reference sections. either compressed by us, or copied directly from the reference file.
void ref_compress_ref (void)
{
    if (!buf_is_alloc (&gref.ranges)) return;

    START_TIMER;

    // calculate ref2chrom_map, the inverse of chrom2ref_map
    if (IS_REF_CHROM2REF)
        chrom_calculate_ref2chrom (ref_num_contigs());

    // remove unused contigs
    if (IS_REF_INTERNAL || IS_REF_EXT_STORE) 
        for_buf2 (Range, r, range_i, gref.ranges) 
            if (bits_is_fully_clear (&r->is_set)) 
                r->is_set.nbits = 0; // unused contig

    if (gref.ranges.rtype != RT_MAKE_REF)
        ref_contigs_compress_stored();  

    // initialize Range.num_set (must be before ref_copy_compressed_sections_from_reference_file)
    if (!flag.make_reference) {
        for_buf (Range, r, gref.ranges) 
            if (!r->num_set) 
                r->num_set = -1; // if not already set by ref_contigs_populate_aligned_chroms
    }

    else 
        ref_make_prepare_ranges_for_compress();

    // copy already-compressed SEC_REFERENCE sections from the genozip reference file, but only such sections that are almost entirely
    // covered by ranges with is_set=true. we mark these ranges affected as is_set=false.
    if (gref.ranges.rtype == RT_LOADED)
        ref_copy_compressed_sections_from_reference_file();

    buf_alloc (evb, &gref.stored_ra, 0, gref.ranges.len, RAEntry, 1, "stored_ra");
    gref.stored_ra.len = 0; // re-initialize, in case we read the external reference into here
    
    spin_initialize (gref.stored_ra_spin);

    // compression of reference doesn't output % progress
    SAVE_FLAGS;
    
    if (flag.show_reference) flag.quiet = true; // show references instead of progress

    uint64_t z_size_before = z_file->disk_so_far;
               
    // proceed to compress all ranges that have still have data in them after copying
    Dispatcher dispatcher = 
        dispatcher_fan_out_task ("compress_ref", NULL, 0, "Writing reference...", true, false, false, 0, 5000, false,
                                 flag.make_reference ? ref_make_prepare_one_range_for_compress : ref_prepare_range_for_compress, 
                                 ref_compress_one_range, 
                                 zfile_output_processed_vb);

    uint32_t num_vbs_dispatched = dispatcher_get_next_vb_i (dispatcher);
    FREE (dispatcher);

    RESTORE_FLAGS;
    
    // SAM require at least one reference section, but if the SAM is unaligned, there will be none - create one empty section
    // (this will also happen if SAM has just only reference section, we will just needlessly write another tiny section - no harm)
    // incidentally, this empty section will also be written in case of a small (one vb) reference - no harm
    if (Z_DT(SAM) && num_vbs_dispatched==1) {
        evb->range = NULL;
        ref_compress_one_range (evb); // written with vb_i=0, section header only (no body)
    }

    // compress reference random access (note: in case of a reference file, SEC_REF_RAND_ACC will be identical to SEC_RANDOM_ACCESS. That's ok, its tiny)
    random_access_finalize_entries (&gref.stored_ra); // sort in order of vb_i

    if ((Z_DT(SAM) || Z_DT(BAM)) && flag.reference == REF_INTERNAL) {
        uint64_t ref_bytes = z_file->disk_so_far - z_size_before;
        int ref_pc = 100 * ref_bytes / z_file->disk_so_far;

        if (ref_pc >= 10)
            TIP ("Compressing this %s file using a reference will save at least %s (%u%%).\n"
                 "Use: \"%s --reference <ref-file> %s\". ref-file may be a FASTA file or a .ref.genozip file.\n",
                 dt_name (txt_file->data_type), str_size (ref_bytes).s, ref_pc, arch_get_argv0(), txt_file->name);
    }

    // range data needed for contigs is set in ref_make_prepare_one_range_for_compress, called from dispatcher_fan_out_task
    if (flag.make_reference) {
        ref_contigs_compress_ref_make(); 

        ref_make_calculate_digest();
    }

    COPY_TIMER_EVB (ref_compress_ref);
}

// -------------------------------
// Loading an external reference
// -------------------------------

void ref_set_reference (rom filename, ReferenceType ref_type, bool is_explicit)
{
    if (!is_explicit && gref.filename) return; // already set explicitly

    ASSINP (!filename || filename[0] != '-', "expecting a the name a reference file after --reference, but found \"%s\"", filename); // catch common error of a command line option instead of a ref filename
    
    rom env = getenv ("GENOZIP_REFERENCE");
    unsigned filename_len;
    StrTextLong alt_name;

    if (!filename) {
        if (!env || !env[0] || file_is_dir (env)) return; // nothing to set

        filename     = env;
        filename_len = strlen (env);
        WARN ("Note: Using the reference file \"%.*s\" set in $GENOZIP_REFERENCE. You can override this with --reference", STRf(filename));
    }

    // explicit filename, relative to GENOZIP_REFERENCE directory
    else if (is_explicit && 
             env && env[0] && // have GENOZIP_REFERENCE
             filename[0] != '/' && filename[0] != '\\' && // filename is a relative path
             !file_exists (filename) &&
             file_is_dir (env) &&
             strlen (filename) + strlen(env) + 2 <= sizeof (alt_name)) {
        
        char *next = strpcpy (alt_name.s, env);
        if (next[-1] != '/') *next++ = '/';
        next = strpcpy (next, filename);

        filename = alt_name.s;
        filename_len = next - alt_name.s;
    }

    else 
        filename_len = strlen (filename);

    static int num_explicit = 0; // user can have up to 1 --reference arguments
    ASSINP0 (!is_explicit || (++num_explicit <= 1), "More than one --reference argument");
    
    // case: pizzing subsequent files with implicit reference (reference from file header)
    if (!is_explicit && gref.filename) {
        if (!strcmp (filename, gref.filename)) return; // same file - we're done

        // in case a different reference is loaded - destroy it
        ref_destroy_reference();
    }

    flag.reference    = ref_type; 
    flag.explicit_ref = is_explicit;

    gref.filename = CALLOC (filename_len + 1);
    memcpy ((char*)gref.filename, filename, filename_len);

    // note: usually, Windows agrees to open paths with either / or \ - but when running in gdb, only / is accepted
    if (flag.is_windows)
        str_replace_letter ((char*)gref.filename, filename_len, '\\', '/');
}

// called when loading an external reference
void ref_set_ref_file_info (Digest genome_digest, bool is_adler, rom fasta_name, uint8_t genozip_version)
{
    gref.genome_digest   = genome_digest;
    gref.is_adler        = is_adler;
    gref.genozip_version = genozip_version;

    if (fasta_name[0] && !gref.ref_fasta_name) { // possibly already allocated & set if function called twice
        gref.ref_fasta_name = MALLOC (strlen (fasta_name) + 1); 
        strcpy (gref.ref_fasta_name, fasta_name);
    }
}

// display the reference:
// show a subset of the reference if --regions is specified - but only up to one region per chromosome
void ref_display_ref (void)
{
    static Context chrom_ctx = {}; // static because the contained buffers cannot be on the stack as they are added to buf_list
    ref_load_external_reference (&chrom_ctx);

    if (flag.regions) regions_make_chregs (&chrom_ctx);

    decl_acgt_decode;
    for (ConstRangeP r = B1ST (Range, gref.ranges); r < BAFT (Range, gref.ranges); r++) {

        unsigned num_intersections = regions_get_num_range_intersections (r->chrom);
        if (!num_intersections) continue;

        if (!flag.no_header && !flag.gpos) printf ("%.*s\n", STRf (r->chrom_name)); // note: this runs stand-alone, so we always output to stdout, not info_stream
        
        for (unsigned inter_i=0; inter_i < num_intersections; inter_i++) {
         
            PosType64 display_first_pos, display_last_pos;
            bool revcomp;
            
            ASSERT0 (regions_get_range_intersection (r->chrom, r->first_pos, r->last_pos, inter_i, &display_first_pos, &display_last_pos, &revcomp), 
                     "unexpectedly, no intersection");

            int64_t adjust = flag.gpos ? r->gpos - r->first_pos : 0;

            // case: normal sequence
            if (!revcomp) {
                if (display_first_pos == display_last_pos)
                    iprintf ("%"PRIu64"\t", display_first_pos + adjust);
                else
                    iprintf ("%"PRIu64"-%"PRIu64"\t", display_first_pos + adjust, display_last_pos + adjust);

                for (PosType64 pos=display_first_pos, next_iupac_pos=display_first_pos ; pos <= display_last_pos ; pos++) {
                    char iupac = (pos==next_iupac_pos) ? ref_iupacs_get (r, pos, false, &next_iupac_pos) : 0;
                    char base = iupac ? iupac : ref_base_by_pos (r, pos);
                    fputc (base, stdout);
                }
            }

            // case: reverse complement
            else {
                if (display_first_pos == display_last_pos)
                    iprintf ("COMPLEM %"PRIu64"\t", display_first_pos + adjust);
                else 
                    iprintf ("COMPLEM %"PRIu64"-%"PRIu64"\t", display_last_pos + adjust, display_first_pos + adjust);

                for (PosType64 pos=display_last_pos, next_iupac_pos=display_last_pos; pos >= display_first_pos; pos--) {
                    char iupac = (pos==next_iupac_pos) ? ref_iupacs_get (r, pos, true, &next_iupac_pos) : 0;
                    char base = iupac ? iupac : ref_base_by_pos (r, pos);
                    fputc (COMPLEM[(int)base], stdout);
                }        
            }

            fputc ('\n', stdout);
        }
    }

    buflist_free_ctx (evb, &chrom_ctx);
}

bool ref_is_external_loaded (void)
{
    return gref.external_ref_is_loaded;
}

// do we have a reference (EXTERNAL, STORED, ....)
bool ref_is_loaded (void)
{
    return gref.genome && gref.genome->nbits;
}

// ZIP & PIZ: import external reference
void ref_load_external_reference (ContextP chrom_ctx)
{
    ASSERTNOTNULL (gref.filename);
    SAVE_FLAGS_AUX ("reference");

    flag.reading_reference = true; // tell file.c, fasta.c and ref_fasta_to_ref that this is a reference
    flag.no_writer = flag.no_writer_thread = true;
    flag.show_contigs = false;
    flag.show_gheader = false;
    flag.show_time_comp_i = COMP_NONE;
    flag.t_offset = flag.t_size = 0; 
    ASSERTISNULL (z_file);
    z_file = file_open_z_read (gref.filename);    
    
    TEMP_VALUE (command, PIZ);

    // the reference file has no components or VBs - it consists of only a global area including reference sections
    ASSERT0 (piz_read_global_area() == DT_REF, "Failed to read reference file"); // if error, detailed error was already outputted
    
    // case: we are requested the chrom context - word_list and dict
    if (chrom_ctx) {
        buf_copy (evb, &chrom_ctx->word_list, &ZCTX(REF_CONTIG)->word_list, CtxWord, 0, 0, "contexts->word_list");
        buf_copy (evb, &chrom_ctx->dict, &ZCTX(REF_CONTIG)->dict, char, 0, 0, "contexts->word_list");
    }

    file_close (&z_file);
    file_close (&txt_file); // close the txt_file object we created (even though we didn't open the physical file). it was created in file_open_z called from txtheader_piz_read_and_reconstruct.

    gref.external_ref_is_loaded = true;

    bool save_no_cache = flag.no_cache;

    // recover globals
    RESTORE_VALUE (command);
    RESTORE_FLAGS;

    flag.no_cache = save_no_cache; // in case we could not cache - if ZIP with --test - this will be passed to PIZ
}

// update addresses of r->ref.words if genome has moved
static void reoverlay_ranges_on_loaded_genome (int64_t delta_bytes)
{
    ASSERT (delta_bytes % 8 == 0, "expecting delta_bytes=%"PRIu64" to be a multiple of 8", delta_bytes);

    for_buf (Range, r, gref.ranges)
        if (r->ref.words) 
            r->ref.words += delta_bytes / sizeof (uint64_t);


    if (flag.show_cache) iprint0 ("show-cache: reoverlaying ranges after genome address changed\n");
}

static void overlay_ranges_on_loaded_genome (RangesType type)
{
    if (flag.show_cache) iprint0 ("show-cache: overlaying ranges on genome\n");

    // overlay all chromosomes (range[i] goes to chrom_index=i) - note some chroms might not have a contig in 
    // which case their range is not initialized
    for_buf (Range, r, gref.ranges) {
        r->chrom = BNUM (gref.ranges, r);
        const Contig *rc = (type == RT_DENOVO) ? B(Contig, gref.ctgs.contigs, r->chrom)
                                               : ref_contigs_get_contig_by_ref_index (r->chrom, SOFT_FAIL); // binary-search

        if (rc) { // this chromosome has reference data 
            r->gpos           = rc->gpos;
            r->first_pos      = rc->min_pos;
            r->last_pos       = rc->max_pos;
            r->chrom_name_len = rc->snip_len;
            r->chrom_name     = Bc(gref.ctgs.dict, rc->char_index);

            if (type == RT_DENOVO) 
                r->chrom_name = ref_contigs_get_name (r->range_id, &r->chrom_name_len);

            else
                ref_contigs_get_name_by_ref_index (r->chrom, pSTRa(r->chrom_name), NULL);

            PosType64 nbases = rc->max_pos - rc->min_pos + 1;

            ASSERT (r->gpos + nbases <= gref.genome->nbits / 2, "adding range \"%s\": r->gpos(%"PRId64") + nbases(%"PRId64") (=%"PRId64") is beyond gref.genome->nbits/2=%"PRIu64" (genome_nbases=%"PRId64")",
                    r->chrom_name, r->gpos, nbases, r->gpos+nbases, gref.genome->nbits/2, gref.genome_nbases);

            bits_overlay (&r->ref, gref.genome, r->gpos*2, nbases*2);

            if (ref_has_is_set()) 
                bits_overlay (&r->is_set, gref.genome_is_set, r->gpos, nbases);
        }
    }
}

static ASCENDING_SORTER (sort_by_vb_i, SectionEnt, vblock_i);

// case 1: in case of ZIP with external reference, called by ref_load_stored_reference during piz_read_global_area of the reference file with RT_LOADED
// case 2: in case of PIZ: also called from ref_load_stored_reference with RT_LOADED
// case 3: in case of ZIP of SAM using internal reference - called from sam_header_inspect with RT_DENOVO
// note: ranges allocation must be called by the main thread as it adds a buffer to evb buf_list
void ref_initialize_ranges (RangesType type)
{
    START_TIMER;

    if (flag.reading_reference) {
        buf_copy (evb, &gref.ref_external_ra, &z_file->ra_buf, RAEntry, 0, 0, "ref_external_ra");
        
        // copy section list, keeping only SEC_REFERENCE sections
        buf_copy (evb, &gref.ref_file_section_list, &z_file->section_list, SectionEnt, 0, 0, "ref_file_section_list");
        buf_remove_items_except (struct SectionEnt, gref.ref_file_section_list, st, SEC_REFERENCE);
        
        // sort sections - ascending by vblock_i
        qsort (B1ST(struct SectionEnt, gref.ref_file_section_list), gref.ref_file_section_list.len32, sizeof (SectionEnt), sort_by_vb_i);

        // verify
        for_buf2 (SectionEnt, sec, sec_i, gref.ref_file_section_list)
            ASSERT (sec->vblock_i == sec_i + 1, "Expecting sec->vblock_i=%u == sec_i=%u + 1", sec->vblock_i, sec_i);
    }

    // notes on ranges.len:
    // 1. PIZ: in case stored reference originates from REF_INTERNAL, we have a contig for every chrom for which there was a sequence,
    // but there might be additional chroms in the SAM file (eg in RNEXT) that don't have a sequence. Since we're indexing ranges by chrom,
    // we need a range for each chrom, even if it doesn't have data 
    // 2. in case stored reference originates from REF_EXT_STORE, we have contigs for all the chroms in the original reference file,
    // but our CHROM contexts also includes alternate chrom names that aren't in the original reference that appear after the reference
    // chroms. we need to make sure ranges.len does not include alternate chroms as that's how we know a chrom is alternate in ref_piz_get_range
    // 3. in case loading from a reference file, the number of contigs will match the number of chroms, so no issues.
    gref.ranges.len = IS_REF_INTERNAL_PIZ ? ZCTX(CHROM)->word_list.len : gref.ctgs.contigs.len;

    buf_alloc_exact_zero (evb, gref.ranges, gref.ranges.len, Range, "ranges");     
    gref.ranges.rtype = type;

    ContextP chrom_ctx = ZCTX(CHROM);

    for_buf2 (Range, r, range_id, gref.ranges) {
        r->chrom = r->range_id = range_id; 

        if (IS_REF_STORED_PIZ) 
            ctx_get_snip_by_word_index (chrom_ctx, r->chrom, r->chrom_name);
        
        else if (type == RT_DENOVO) 
            r->chrom_name = ref_contigs_get_name (range_id, &r->chrom_name_len);

        else
            ref_contigs_get_name_by_ref_index (r->chrom, pSTRa(r->chrom_name), &r->gpos); // binary search
    }

    // note: genome_nbases must be full words, so that bits_reverse_complement doesn't need to shift
    // note: mirrors setting num_gap_bytes in ref_make_prepare_one_range_for_compress
    gref.genome_nbases = ROUNDUP64 (ref_contigs_get_genome_nbases()) + 64; // round up to the nearest 64 bases, and add one word, needed by aligner_update_best for bit shifting overflow

    if (ref_has_is_set()) 
        gref.genome_is_set = buf_alloc_bits_exact (evb, &gref.genome_is_set_buf, gref.genome_nbases, CLEAR, 0, "genome_is_set_buf");

    // we protect genome->ref while uncompressing reference data, and genome->is_set while segging
    ref_lock_initialize();

    // either get shm, or allocate process memory for the genome
    if (flag.no_cache || !flag.reading_reference || !ref_cache_initialize_genome()) {
        // if the genome allocated in previous file is way bigger - destroy the buffer first to save memory
        if (gref.genome_buf.size > gref.genome_nbases / 4 * 1.25)
            buf_destroy (gref.genome_buf);

        gref.genome_buf.can_be_big = true; // supress warning in case of an extra large genome (eg plant genomes)
        // initialize genome to 0 (since 14.0.33) - this is required, because there are small gaps between and after the contigs so that each
        // contig's GPOS is a multiple of 64. Our aligner might include gaps in the alignments. See also: defect "2023-03-10 uninitialized edge of external reference.txt"    
        gref.genome = buf_alloc_bits_exact (evb, &gref.genome_buf, gref.genome_nbases * 2, CLEAR, 0, "gref.genome_buf");
    }

    overlay_ranges_on_loaded_genome (type);

    COPY_TIMER_EVB (ref_initialize_ranges);
}

typedef struct { PosType64 min_pos, max_pos; } MinMax;

//---------------------------------------
// Printing
//---------------------------------------
StrTextLong ref_display_range (ConstRangeP r)
{
    StrTextLong s;

    snprintf (s.s, sizeof (s.s), "chrom=\"%.*s\"(%d) ref.num_bits=%"PRIu64" is_set.num_bits=%"PRIu64" first_pos=%"PRId64
             " last_pos=%"PRId64" gpos=%"PRId64,
             STRf(r->chrom_name), r->chrom, r->ref.nbits, r->is_set.nbits, r->first_pos, r->last_pos, r->gpos);
    return s;
}

void ref_display_all_ranges (void)
{
    ARRAY (Range, r, gref.ranges);

    iprint0 ("\n\nList or all ranges:\n");

    for (uint64_t range_i=0; range_i < r_len; range_i++)
        if (r[range_i].ref.nbits || IS_PIZ)
            iprintf ("%s\n", ref_display_range (&r[range_i]).s);

    if (!r_len)
        iprint0 ("reference has no ranges\n");
}

void ref_print_subrange (rom msg, ConstRangeP r, PosType64 start_pos, PosType64 end_pos, FILE *file) /* start_pos=end_pos=0 if entire ref */
{
    uint64_t start_idx = start_pos ? start_pos - r->first_pos : 0;
    uint64_t end_idx   = (end_pos ? MIN_(end_pos, r->last_pos) : r->last_pos) - r->first_pos;
    decl_acgt_decode;

    fprintf (file, "%s: %.*s %"PRId64" - %"PRId64" (len=%u): ", msg, STRf (r->chrom_name), start_pos, end_pos, (uint32_t)(end_pos - start_pos + 1));
    for (uint64_t idx = start_idx; idx <= end_idx; idx++) 
        fputc (ref_base_by_idx (r, idx) + (32 * !ref_is_nucleotide_set (r, idx)), file); // uppercase if set, lowercase if not

    fputc ('\n', file);
}

// outputs in seq, a nul-terminated string of up to (len-1) bases
char *ref_dis_subrange (ConstRangeP r, PosType64 start_pos, PosType64 len, char *seq, bool revcomp) // in_reference allocated by caller to length len
{
    decl_acgt_decode;
    if (!revcomp) {
        PosType64 next_iupac_pos, pos, end_pos = MIN_(start_pos + len - 1 - 1, r->last_pos);  // -1 to leave room for \0
        decl_acgt_decode;
        for (pos=start_pos, next_iupac_pos=start_pos ; pos <= end_pos; pos++) {
            char iupac = (pos==next_iupac_pos) ? ref_iupacs_get (r, pos, false, &next_iupac_pos) : 0;
            seq[pos - start_pos] = iupac ? iupac : ref_base_by_pos (r, pos);
        }
        seq[pos - start_pos] = 0;
    }

    // revcomp: display the sequence starting at start_pos and going backwards - complemented
    else {
        PosType64 next_iupac_pos, pos, end_pos = MAX_(start_pos - (len - 1) + 1, r->first_pos);  // -1 to leave room for \0
        for (pos=start_pos, next_iupac_pos=start_pos ; pos >= end_pos; pos--) {
            char iupac = (pos==next_iupac_pos) ? ref_iupacs_get (r, pos, true, &next_iupac_pos) : 0;
            seq[start_pos - pos] = iupac ? iupac : COMPLEM[(int)ref_base_by_pos (r, pos)];
        }
        seq[start_pos - pos] = 0;
    }

    return seq;
}

void ref_print_is_set (ConstRangeP r,
                       PosType64 around_pos,  // display around this neighborhoud ; -1 means entire range
                       FILE *file)
{
#   define neighborhood (PosType64)10000

    fprintf (file, "\n\nRegions set for ref_index %d \"%.*s\" [%"PRId64"-%"PRId64"] according to range.is_set (format- \"first_pos-last_pos (len)\")\n", 
             r->chrom, STRf(r->chrom_name), r->first_pos, r->last_pos);
    fprintf (file, "In the neighborhood of about %u bp around pos=%"PRId64"\n", (unsigned)neighborhood, around_pos);

    if (!r->is_set.nbits)
        fprintf (file, "No data: r->is_set.nbits=0\n");

    if (around_pos < r->first_pos || around_pos > r->last_pos)
        fprintf (file, "No data: pos=%"PRId64" is outside of [first_pos=%"PRId64" - last_pos=%"PRId64"]\n", around_pos, r->first_pos, r->last_pos);

    uint64_t next;
    for (uint64_t i=0; i < r->is_set.nbits; ) {
        
        bool found = bits_find_next_clear_bit (&r->is_set, i, &next);
        if (!found) next = r->is_set.nbits;

        bool in_neighborhood = (around_pos - (PosType64)(r->first_pos+i) > -neighborhood) && (around_pos - (PosType64)(r->first_pos+i) < neighborhood);
        if (next > i && (around_pos == -1 || in_neighborhood)) {
            if (next - i > 1)
                fprintf (file, "%"PRId64"-%"PRIu64"(%u)\t", r->first_pos + i, r->first_pos + next-1, (uint32_t)(next - i));
            else
                fprintf (file, "%"PRId64"(1)\t", r->first_pos + i);
        }                   
        if (!found) break;

        i = next;

        found = bits_find_next_set_bit (&r->is_set, i, &next);
        if (!found) next = r->is_set.nbits;

        i = next;
    }
    fprintf (file, "\n");
}

rom ref_type_name (void)
{
    switch (flag.reference) {
        case REF_NONE       : return "NONE"; 
        case REF_INTERNAL   : return "INTERNAL"; 
        case REF_EXTERNAL   : return "EXTERNAL";
        case REF_EXT_STORE  : return "EXT_STORE";
        default             : return "Invalid_reference_type"; 
    }
}

bool ref_buf_locate (void *dummy, ConstBufferP buf)
{
    return (char*)buf >= (char*)&gref && 
           (char*)buf <  (char*)(&gref + 1);
}

void ref_verify_organism (VBlockP vb)
{
    double percent_aligned = vb->lines.len32 ? 100.0 * (double)(vb->num_aligned + (VB_DT(FASTQ) ? fastq_get_num_deeped (vb): 0)) / (double)vb->lines.len32 : 0;

    if (percent_aligned < 80)
        TIP ("Only %2.1f%% of the %s reads processed so far match the reference file. Using a reference file more representative of the organism(s) in the data will result in much better compression (tested: %s).", 
             percent_aligned, str_int_commas (vb->lines.len32).s, VB_NAME);
}