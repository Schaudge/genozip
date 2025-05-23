// ------------------------------------------------------------------
//   chrom.c
//   Copyright (C) 2019-2025 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#include <stdarg.h>
#include "seg.h"
#include "random_access.h"
#include "chrom.h"
#include "zfile.h"
#include "contigs.h"
#include "b250.h"

static ContextP sorter_ctx = NULL; 
static Buffer chrom_sorter = {}; // ZIP/PIZ: index into sorter_ctx->nodes/word_list, sorted alphabetically by snip

// the data of SEC_CHROM2REF_MAP - this is part of the Genozip file format
typedef struct { WordIndex chrom_index, ref_index; } Chrom2Ref; 

//-------------------------
// chrom2ref mapping stuff
//-------------------------

// ZIP of a file with an external reference: 
// The CHROM dictionary includes: first - the txtheader contig names, then reference file contig names that are not in txtheader, 
// and finally names encountered in the file that are not in either the header or the reference (see ctx_populate_zf_ctx_from_contigs()). 
// Since the CHROM context is prepopulated from the txtheader and the reference, often not all the entries are used by the file data.
//
// Here, we create a mapping between those chrom entries that are used (count>0) and the index of the contig in the reference file
// against which this chrom data was compressed. We rely on contigs_get_matching() returning the same result here as it did in Seg.

void chrom_2ref_compress (void)
{
    if (flag.show_chrom2ref) 
        iprint0 ("\nAlternative chrom indices (output of --show-chrom2ref): chroms that are in the file and are mapped to a different index in the reference\n");
    
    ASSERTNOTINUSE (evb->scratch);
    buf_alloc (evb, &evb->scratch, 0, ZCTX(CHROM)->chrom2ref_map.len, Chrom2Ref, 1, "scratch");
    ContextP zctx = ZCTX(DTFZ(chrom));

    for_buf2 (WordIndex, ref_index, chrom_node_index, ZCTX(CHROM)->chrom2ref_map) {
        if (flag.show_chrom2ref) {
            STR(chrom_name);
            ctx_get_z_snip_ex (zctx, chrom_node_index, pSTRa(chrom_name));
            rom ref_name = *ref_index >= 0 ? ref_contigs_get_name (*ref_index, NULL) : "(none)";

            char in_file[MAX_(chrom_name_len + 2, 16)];
            if (! *chrom_name) 
                strcpy (in_file, "(not used)"); // name was removed from dict because this header contig was not used in the file
            else
                snprintf (in_file, MAX_(chrom_name_len + 2, 16), "\"%.*s\"", STRf(chrom_name));

            if (*ref_index != WORD_INDEX_NONE) 
                iprintf ("In file: %s (%d)\tIn reference: \"%s\" (%d)\t%s\n", 
                         in_file, chrom_node_index, ref_name, *ref_index, chrom_node_index != *ref_index ? "INDEX_CHANGE" : "");
            else
                iprintf ("In file: %s (%d)\tNot in reference\n", in_file, chrom_node_index);
        }

        // adds the mapping if not identify and adds -1 if this chrom doesn't map to a ref contig.
        // note: we add only contigs that are used (count>0) except for aligner_available in which case we don't have counts (for REF_EXTERNAL, we have 
        // populated all contigs in zip_initialize, and for REF_EXT_STORE we add contigs with any bit set in is_set) 
        if (*ref_index != chrom_node_index && *ref_index != WORD_INDEX_NONE && (*B64(zctx->counts, chrom_node_index) || flag.aligner_available))
            BNXT (Chrom2Ref, evb->scratch) = (Chrom2Ref){ .chrom_index = BGEN32(chrom_node_index), 
                                                          .ref_index   = BGEN32(*ref_index)       };
    }

    if (evb->scratch.len) {
        evb->scratch.len *= sizeof (Chrom2Ref);
        zfile_compress_section_data_ex (evb, NULL, SEC_CHROM2REF_MAP, &evb->scratch, 0,0, CODEC_LZMA, SECTION_FLAGS_NONE, NULL); // compresses better with LZMA than BZLIB
    }

    buf_free (evb->scratch);
}

void chrom_2ref_load (void)
{
    Section sec = sections_last_sec (SEC_CHROM2REF_MAP, SOFT_FAIL);
    if (!sec) return;

    zfile_get_global_section (SectionHeader, sec, &evb->scratch, "scratch");

    if (flag.show_chrom2ref) 
        iprint0 ("\nAlternative chrom indices (output of --show-chrom2ref): chroms that are in the txt file and are mapped to a different index in the reference\n");

    evb->scratch.len /= sizeof (Chrom2Ref);
    Context *zctx = ZCTX(CHROM);

    // create mapping user index -> reference index
    buf_alloc (evb, &ZCTX(CHROM)->chrom2ref_map, 0, zctx->word_list.len, WordIndex, 1, "ZCTX(CHROM)->chrom2ref_map");
    ZCTX(CHROM)->chrom2ref_map.len = zctx->word_list.len;

    // initialize with unity mapping
    ARRAY (WordIndex, map, ZCTX(CHROM)->chrom2ref_map);
    for (uint32_t i=0; i < zctx->word_list.len32; i++)
        map[i] = i;

    // the indices of chroms that are NOT in the reference (they are only in the user file), will be mapped to ref chroms
    ConstContigPkgP ctgs = ref_get_ctgs(); 
    WordIndex num_ref_contigs = ctgs->contigs.len; // must be signed int

    for_buf2 (Chrom2Ref, ent, i, evb->scratch) {
        WordIndex chrom_index = BGEN32 (ent->chrom_index);
        WordIndex ref_index   = BGEN32 (ent->ref_index);

        ASSERTINRANGE (chrom_index, 0, zctx->word_list.len32);
        ASSERT (!num_ref_contigs /* ref not loaded */ || IN_RANGE (ref_index, -1, num_ref_contigs), 
                "ref_index=%d ∉ [-1,%u) (chrom_index=%u i=%u len=%u)", 
                ref_index, num_ref_contigs, chrom_index, i, evb->scratch.len32);

        map[chrom_index] = ref_index;

        if (flag.show_chrom2ref) {
            rom chrom_name = ctx_get_words_snip (zctx, chrom_index);
            rom ref_name   = ref_index >= 0 ? ref_contigs_get_name (ref_index, NULL) : NULL;
            if (ref_name)
                iprintf ("In file: '%s' (%d)\tIn reference: '%s' (%d)\n", chrom_name, chrom_index, ref_name, ref_index);
            else
                iprintf ("In file: '%s' (%d)\tNot in reference\n", chrom_name, chrom_index);
        }
    }

    if (flag.show_chrom2ref && is_genocat) exit (EXIT_OK); // in genocat this, not the data

    buf_free (evb->scratch);
}

// ZIP: returns the ref index by the chrom index, works only after Segging of CHROM
WordIndex chrom_2ref_seg_get (ConstVBlockP vb, WordIndex chrom_index)
{ 
    ASSSEG (chrom_index >= WORD_INDEX_NONE, "invalid chrom_index=%d", chrom_index);

    decl_const_ctx(CHROM);

    int32_t ol_len = ctx->ol_chrom2ref_map.len32;

    WordIndex ref_index = (chrom_index == WORD_INDEX_NONE)         ? WORD_INDEX_NONE
                        : (chrom_index < ol_len)                   ? *B(WordIndex, ctx->ol_chrom2ref_map, chrom_index)
                        : (chrom_index < ctx->chrom2ref_map.len32) ? *B(WordIndex, ctx->chrom2ref_map, chrom_index - ol_len) // possibly WORD_INDEX_NONE, see chrom_seg_ex
                        :                                            WORD_INDEX_NONE;

    ASSSEG (IN_RANGE (ref_index, WORD_INDEX_NONE, (WordIndex)ref_num_contigs()), 
            "ref_index=%d ∉ [-1,%u) chrom_index=%d", ref_index, ref_num_contigs(), chrom_index);

    return ref_index;
}   

void chrom_calculate_ref2chrom (uint64_t num_ref_contigs)
{
    decl_zctx(CHROM);

    buf_alloc_exact_255 (evb, zctx->ref2chrom_map, num_ref_contigs, WordIndex, "ZCTX(CHROM)->ref2chrom_map");

    ARRAY (WordIndex, r2c, zctx->ref2chrom_map);
    ARRAY (WordIndex, c2r, zctx->chrom2ref_map);
    
    for (unsigned wi=0; wi < c2r_len; wi++)
        if (c2r[wi] != WORD_INDEX_NONE) {
            ASSERT (c2r[wi] >= 0 && c2r[wi] < r2c_len, "expecting 0<= c2r[%u]=%d < r2c_len=%u: ZCTX(CHROM)->node.len=%u ZCTX(CHROM)->chrom2ref_map.len=%u CHROM[%u]=%s", 
                    wi, c2r[wi], (unsigned)r2c_len, zctx->nodes.len32, zctx->chrom2ref_map.len32, wi, ctx_get_z_snip (zctx, wi).s);
            
            // expecting only one chrom a be mapped to any particular ref_contig 
            ASSERT (r2c[c2r[wi]] == WORD_INDEX_NONE, "trying to map ref=\"%s\"(%d) to chrom=%s(%d): but r2c[%u] is already set to %s(%u)", 
                    ref_contigs_get_name (c2r[wi], NULL), c2r[wi], ctx_get_z_snip (zctx, wi).s, wi, c2r[wi], ctx_get_z_snip (zctx, r2c[c2r[wi]]).s, r2c[c2r[wi]]);
            
            r2c[c2r[wi]] = wi;
        }
}

//-------------
// Seg stuff
//-------------

WordIndex chrom_seg_ex (VBlockP vb, Did did_i, 
                        STRp(chrom), 
                        PosType64 LN,     // Optional, if readily known
                        int add_bytes,    // must be signed
                        bool *is_new_out) // optional out
{
    ASSERTNOTZERO (chrom_len);
    decl_ctx (did_i);
    bool is_primary = (did_i == CHROM); // note: possibly not primary, eg SA_RNAME

    WordIndex chrom_node_index = WORD_INDEX_NONE, ref_index = WORD_INDEX_NONE;
    bool is_new, is_alt=false;
    
    chrom_node_index = seg_by_ctx_ex (vb, STRa(chrom), ctx, add_bytes, &is_new); // note: this is not the same as ref_index, bc ctx->nodes contains the header contigs first, followed by the reference contigs that are not already in the header
    
    STR0 (ref_contig);
    if (is_new && (flag.reference & REF_ZIP_LOADED))
        ref_index = ref_contigs_get_matching (STRa(chrom), pSTRa(ref_contig), false, &is_alt, NULL);

    // warn if the file's contigs are called by a different name in the reference (eg 22/chr22)
    static bool once[2]={};
    if (ref_index != WORD_INDEX_NONE && is_alt && // a new chrom that matched to the reference with an alternative name
        is_primary &&
        !segconf_running  &&              // segconf runs with flag.quiet so the user won't see the warning
        !__atomic_test_and_set (&once[is_primary], __ATOMIC_RELAXED))   // skip if we've shown the warning already
            
        WARN ("FYI: Contigs name mismatch between %s and reference file %s. For example: file: \"%.*s\" Reference file: \"%.*s\". This makes no difference for the compression.",
              txt_name, ref_get_filename(), STRf(chrom), STRf(ref_contig));
        // we don't use WARN_ONCE bc we want the "once" to also include ref_contigs_get_matching

    if (is_new_out) *is_new_out = is_new;        

    if (is_primary)
        random_access_update_chrom (vb, chrom_node_index, STRa(chrom)); 

    if (is_primary) {
        vb->chrom_node_index = chrom_node_index;

        if (chrom_node_index != WORD_INDEX_NONE) 
            STRset (vb->chrom_name, chrom);
    }

    if (is_new && chrom_2ref_seg_is_needed (did_i)) { // CHROM context only (not RNEXT etc)
        // note: not all new snips are included in chrom2ref_map - only those segged in this function. Others might be SPECIAL etc.
        uint32_t new_snip_i = chrom_node_index - ctx->ol_chrom2ref_map.len32;
        buf_alloc_255 (vb, &ctx->chrom2ref_map, 0, new_snip_i+1, WordIndex, CTX_GROWTH, "chrom2ref_map");
        ctx->chrom2ref_map.len32 = MAX_(ctx->chrom2ref_map.len32, new_snip_i+1); 

        *B(WordIndex, ctx->chrom2ref_map, new_snip_i) = ref_index; // note: ref_index might be WORD_INDEX_NONE
    }

    return chrom_node_index;
}

WordIndex chrom_seg_no_b250 (VBlockP vb, STRp(chrom_name), bool *is_new)
{
    WordIndex chrom_node_index = chrom_seg_ex (VB, CHROM, STRa(chrom_name), 0, 0, is_new); // also adds to random access etc
    b250_seg_remove_last (vb, CTX(CHROM), chrom_node_index);

    return chrom_node_index;
}

bool chrom_seg_cb (VBlockP vb, ContextP ctx, STRp (chrom), uint32_t repeat)
{
    chrom_seg_ex (vb, ctx->did_i, STRa(chrom), 0, chrom_len, NULL);

    return true; // segged successfully
}

static SORTER (chrom_create_zip_sorter)
{
    uint32_t index_a = *(uint32_t *)a;
    uint32_t index_b = *(uint32_t *)b;

    CtxNode *word_a = B(CtxNode, sorter_ctx->nodes, index_a);
    CtxNode *word_b = B(CtxNode, sorter_ctx->nodes, index_b);

    return strcmp (Bc (sorter_ctx->dict, word_a->char_index),  
                   Bc (sorter_ctx->dict, word_b->char_index));
}

static SORTER (chrom_create_piz_sorter)
{
    uint32_t index_a = *(uint32_t *)a;
    uint32_t index_b = *(uint32_t *)b;

    CtxWordP word_a = B(CtxWord, sorter_ctx->word_list, index_a);
    CtxWordP word_b = B(CtxWord, sorter_ctx->word_list, index_b);
    
    return strcmp (Bc (sorter_ctx->dict, word_a->char_index),
                   Bc (sorter_ctx->dict, word_b->char_index));
}

// ZIP/PIZ MUST be run by the main thread only
void chrom_index_by_name (Did chrom_did_i)
{
    sorter_ctx = ZCTX(chrom_did_i);
    uint32_t num_words = (command==ZIP) ? sorter_ctx->nodes.len32 : sorter_ctx->word_list.len32;

    buf_free (chrom_sorter);

    // chrom_sorter - an array of uint32 of indexes into ZCTX(CHROM)->word_list - sorted by alphabetical order of the snip in ZCTX(CHROM)->dict
    buf_alloc (evb, &chrom_sorter, 0, num_words, uint32_t, 1, "chrom_sorter");
    
    if (IS_ZIP) {
        for_buf2 (CtxNode, node, i, sorter_ctx->nodes)
            if (node->snip_len) BNXT32(chrom_sorter) = i;
    }
    else
        for_buf2 (CtxWord, word, i, sorter_ctx->word_list)
            if (word->snip_len) BNXT32(chrom_sorter) = i;

    qsort (STRb(chrom_sorter), sizeof(uint32_t), IS_ZIP ? chrom_create_zip_sorter : chrom_create_piz_sorter);
}

// binary search for this chrom in ZCTX(CHROM). we count on gcc tail recursion optimization to keep this fast.
static WordIndex chrom_zip_get_by_name_do (rom chrom_name, WordIndex first_sorted_index, WordIndex last_sorted_index)
{
    if (first_sorted_index > last_sorted_index) return WORD_INDEX_NONE; // not found

    WordIndex mid_sorted_index = (first_sorted_index + last_sorted_index) / 2;
    
    STR (snip);
    WordIndex node_index = *B(WordIndex, chrom_sorter, mid_sorted_index);
    ctx_get_z_snip_ex (ZCTX(CHROM), node_index, pSTRa(snip));

    int cmp = strcmp (snip, chrom_name);
    if (cmp < 0) return chrom_zip_get_by_name_do (chrom_name, mid_sorted_index+1, last_sorted_index);
    if (cmp > 0) return chrom_zip_get_by_name_do (chrom_name, first_sorted_index, mid_sorted_index-1);

    return node_index;
}                   

// note: search within the partial set of chroms that existed when chrom_index_by_name was called.
WordIndex chrom_get_by_name (STRp (chrom_name))
{
    ASSERT0 (IS_ZIP, "only works in ZIP"); // if ever needed in PIZ, return chrom_index_by_name call from piz_read_global_area and return chrom_piz_get_by_name_do (see 15.0.68)

    if (!chrom_sorter.len) return WORD_INDEX_NONE;

    // WordIndex wi;
    SAFE_NULT(chrom_name); 
    
    // if (IS_ZIP) 
    WordIndex wi = chrom_zip_get_by_name_do (chrom_name, 0, chrom_sorter.len-1); // not necessarily all of CHROM, just chrom_sorter.len
    
    SAFE_RESTORE;
    return wi;
}

void chrom_finalize (void)
{
    buf_destroy (chrom_sorter);
}
