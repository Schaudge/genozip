// ------------------------------------------------------------------
//   sam_sa.c
//   Copyright (C) 2020-2025 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited,
//   under penalties specified in the license.

// ---------------------------------------------------------
// SA:Z "Other canonical alignments in a chimeric alignment"
//
// SA format is: (rname, pos, strand, CIGAR, mapQ, NM ;)+ 
// Example SA:Z:chr13,52863337,-,56S25M70S,0,0;chr6,145915118,+,97S24M30S,0,0;chr18,64524943,-,13S22M116S,0,0;chr7,56198174,-,20M131S,0,0;chr7,87594501,+,34S20M97S,0,0;chr4,12193416,+,58S19M74S,0,0;
// See: https://samtools.github.io/hts-specs/SAMtags.pdf
// ---------------------------------------------------------

#include "sam_private.h"
#include "chrom.h"
#include "b250.h"
#include "zip_dyn_int.h"

//---------
// SEG
//---------

bool sam_zip_is_valid_SA (rom sa, uint32_t char_limit, bool is_bam)
{
    MINIMIZE (char_limit, 1000); // recognizes SA strings that are up to 1000 characters long

    uint32_t sa_len;
    if (is_bam) {
        rom nul = memchr (sa, 0, char_limit); // search for end of string
        if (!nul) return false;

        sa_len = nul - sa;
    }
    else {
        SAFE_NUL (sa + char_limit);
        sa_len = strcspn (sa, "\t\n\r");
        SAFE_RESTORE;

        if (sa_len == char_limit) return false;
    }

    str_split (sa, sa_len, 0, ';', aln, false);

    for (int i=0; i < n_alns; i++) {
        str_split (alns[i], aln_lens[i], NUM_SA_ITEMS, ',', item, true);
        if (n_items != NUM_SA_ITEMS) return false;
    }

    return true;
}

// Seg: get item from prim line's SA:Z - of the alignment predicted to correspond to the current line
bool sam_seg_SA_get_prim_item (VBlockSAMP vb, int sa_item_i, pSTRp(out))
{
    // in collated files, normally the location of a depn with a SA:Z string is mirrored in its line following the prim line.
    // we seg with COPY_BUDDY if this prediction is correct. in sorted files, we just guess that it's first.
    int predicted_aln_i = segconf.is_sorted ? 0 : (vb->line_i - vb->saggy_line_i) - 1; // mirrors prediction in sam_seg_SA_get_prim_item

    // split SA:Z into alignments
    ZipDataLineSAMP prim_dl = DATA_LINE (vb->saggy_line_i);
    if (!prim_dl->SA.len) return false; // prim line has no SA:Z

    str_split (Btxt (prim_dl->SA.index), prim_dl->SA.len, MAX_SA_NUM_ALNS, ';', prim_aln, false);
    
    // check if SA:Z even contains enough alignment for our prediction
    if (n_prim_alns < predicted_aln_i+2) return false; // note: n_prim_alns also includes a final empty string due to terminal ';'

    // split predicted alignment into its items
    str_split (prim_alns[predicted_aln_i], prim_aln_lens[predicted_aln_i], NUM_SA_ITEMS, ',', item, true);
    if (n_items != NUM_SA_ITEMS) return false;

    *out = items[sa_item_i];
    *out_len = item_lens[sa_item_i];
    return true;
}

bool sam_seg_is_item_predicted_by_prim_SA (VBlockSAMP vb, int sa_item_i, int64_t value)
{
    STR(sa_item_str); // POS within the SA alignment corresponding to this depn line, in the SA:Z of prim line
    int64_t sa_item;

    return sam_seg_SA_get_prim_item (vb, sa_item_i, pSTRa(sa_item_str)) &&
           str_get_int (STRa(sa_item_str), &sa_item) &&
           sa_item == value;
}

static inline bool sam_seg_SA_field_is_line_matches_aln (VBlockSAMP vb, ZipDataLineSAMP dl, STRp(aln), bool is_bam)
{
    if (dl->RNAME == WORD_INDEX_NONE) return false; // RNAME="*" for this line

    str_split (aln, aln_len, NUM_SA_ITEMS, ',', item, true);
    if (n_items != NUM_SA_ITEMS) return false;

    // revcomp (test this item first as it is the fastest)
    bool aln_revcomp = *items[SA_STRAND] == '-';
    if (aln_revcomp != dl->FLAG.rev_comp) return false;

    // rname (pre-populated from sam header)
    STR(line_rname);
    ctx_get_vb_snip_ex (CTX(SAM_RNAME), dl->RNAME, pSTRa(line_rname));

    if (!str_issame_(STRa(line_rname), STRi(item, SA_RNAME))) return false; // RNAME is different
    
    // pos, mapq, nm
    int64_t aln_pos, aln_mapq, aln_nm;
    if (!str_get_int (STRi(item, SA_POS),  &aln_pos)  || aln_pos  != dl->POS  ||
        !str_get_int (STRi(item, SA_MAPQ), &aln_mapq) || aln_mapq != dl->MAPQ ||
        !str_get_int (STRi(item, SA_NM),   &aln_nm)   || aln_nm   != dl->NM) return false; // note: if NM doesn't exist in dl, it is taken as 0. Eg novoalign omits the NM:i field if is is 0

    // cigar
    return str_issame_(STRacigar(dl), STRi(item, SA_CIGAR));
}

static inline void sam_seg_set_depn_clip_hard (VBlockSAMP vb)
{
    if (vb->depn_clipping_type != DEPN_CLIP_UNKNOWN) return; // already set

    if (vb->hard_clip[0] || vb->hard_clip[1]) {
        vb->depn_clipping_type = DEPN_CLIP_HARD;
        CTX(OPTION_SA_Z)->flags.depn_clip_hard = true; // if a depn in this VB has a clipping, it is a H clip.
    }

    else if (vb->soft_clip[0] || vb->soft_clip[1]) {
        vb->depn_clipping_type = DEPN_CLIP_SOFT;
        CTX(OPTION_SA_Z)->flags.depn_clip_hard = false; // if a depn in this VB has a clipping, it is a S clip.
    }

    // note: CIGAR cannot have both S and H - this is blocked in sam_cigar_analyze (even though valid per SAM specification)

    // note: if CIGAR has neither S or H, depn_clipping_type remains DEPN_CLIP_UNKNOWN
}

static bool sam_seg_SA_field_is_depn_from_prim (VBlockSAMP vb, ZipDataLineSAMP dl, STRp(depn_sa))
{
    bool is_bam = IS_BAM_ZIP;

    // parse primary SA
    ZipDataLineSAMP prim_dl = DATA_LINE (vb->saggy_line_i);
    str_split (Btxt (prim_dl->SA.index), prim_dl->SA.len, MAX_SA_NUM_ALNS, ';', prim_aln, false);
    if (n_prim_alns < 2) return false;
    n_prim_alns--; // remove last empty alignment due terminal ';'

    // parse depn SA (i.e. of current line)
    str_split (depn_sa, depn_sa_len, MAX_SA_NUM_ALNS, ';', depn_aln, false);
    if (n_depn_alns < 2) return false;
    n_depn_alns--;

    if (n_prim_alns != n_depn_alns) return false; // different number of alignments

    sam_seg_set_depn_clip_hard (vb); 

    // case: depn line has unexpected type of clipping - can't seg it against prim
    if ((vb->depn_clipping_type == DEPN_CLIP_SOFT && (vb->hard_clip[0] || vb->hard_clip[1])) || 
        (vb->depn_clipping_type == DEPN_CLIP_HARD && (vb->soft_clip[0] || vb->soft_clip[1])))
            return false; 

    // Step 1: verify that the prim line matches the first alignment in the depn SA
    if (!sam_seg_SA_field_is_line_matches_aln (vb, prim_dl, STRi(depn_aln, 0), is_bam))
        return false;

    // Step 2: verify that one of the alignments of the prim SA matches the depn line

    // temporarily replace H with S in this (depn) line's CIGAR
    HtoS htos = sam_cigar_H_to_S (vb, STRacigar(dl), false); // points to txt_data in SAM, line_textual_cigars in BAM

    // to do: improve effeciency by testing first the predicted alignment (vb->line-vb->saggy_line_i-1)
    int depn_i=0; // index of depn alignment within prim SA
    for (; depn_i < n_prim_alns; depn_i++)
        if (sam_seg_SA_field_is_line_matches_aln (vb, dl, STRi(prim_aln, depn_i), is_bam)) break; // found

    sam_cigar_restore_H (htos); // restore

    if (depn_i == n_prim_alns) return false; // not found

    // Step 3: Verify that all other alignments, except for the prim and this depn line, are the same
    for (int pi=0, di=1; pi < n_prim_alns; di++, pi++) {
        if (pi == depn_i) pi++; // skip depn alignment in prim SA
        if (!str_issame_(STRi(prim_aln, pi), STRi(depn_aln, di))) return false;
    }

    return true; // indeed, this depn line is in the same SA group as the prim line
}

static bool sam_SA_cigar_maybe_abbreviated (STRp(cigar))
{
    uint32_t n_Ms = str_count_char (STRa(cigar), 'M');
    uint32_t n_Is = str_count_char (STRa(cigar), 'D');
    uint32_t n_Ds = str_count_char (STRa(cigar), 'I');
    uint32_t n_Es = str_count_char (STRa(cigar), '=');
    uint32_t n_Xs = str_count_char (STRa(cigar), 'X');
    uint32_t n_Hs = str_count_char (STRa(cigar), 'H');
    uint32_t n_Ns = str_count_char (STRa(cigar), 'N');
    uint32_t n_Ps = str_count_char (STRa(cigar), 'P');

    rom p_M  = n_Ms ? memchr (cigar, 'M', cigar_len) : NULL;
    rom p_DI = n_Is ? memchr (cigar, 'I', cigar_len) : n_Ds ? memchr (cigar, 'D', cigar_len) : NULL;

    return n_Ms <= 1 && (n_Is + n_Ds) <= 1 && !n_Es && !n_Xs && !n_Hs && !n_Ns && !n_Ps &&
           (!p_M || !p_DI || p_M < p_DI); // M appears before D/I
}

// called for exactly one SA:Z when segging a MAIN VB if SA_CIGAR abbreviation is plausible, but no SA:Zs were encountered during segconf 
bool sam_test_SA_CIGAR_abbreviated (STRp(sa))
{
    str_split (sa, sa_len, 0, ';', sa, false);
    if (n_sas) n_sas--;       // remove final empty item
    if (!n_sas) return false; // empty (invalid) SA:Z

    for (uint32_t sa_i=0; sa_i < n_sas; sa_i++) {
        STR(cigar);
        if (!str_item_i (sas[sa_i], sa_lens[sa_i], ',', 3, pSTRa(cigar)) || // no SA_CIGAR item: mal-formated SA:Z
            !sam_SA_cigar_maybe_abbreviated (STRa(cigar))) // not abbreviated
            return false;
    } 

    return true; // no evidence that SA:Z is not abbreviated, so we (speculatively) declare that it is
}

static bool sam_segconf_SA_cigar_cb (VBlockP vb, ContextP ctx, STRp (cigar), uint32_t repeat)
{
    if (segconf.SA_CIGAR_abbreviated == unknown && !sam_SA_cigar_maybe_abbreviated (STRa(cigar)))
        segconf.SA_CIGAR_abbreviated = no; // found evidence that SA:Z is not abbreviated

    return sam_seg_0A_cigar_cb (vb, ctx, STRa(cigar), repeat);
}

void sam_seg_SA_Z (VBlockSAMP vb, ZipDataLineSAMP dl, STRp(sa), unsigned add_bytes)
{
    START_TIMER;

    static const MediumContainer container_SA = { .nitems_lo = NUM_SA_ITEMS,      
                                                  .repsep    = { ';' }, // including on last repeat    
                                                  .items     = { { .dict_id = { _OPTION_SA_RNAME  }, .separator = {','} },  
                                                                 { .dict_id = { _OPTION_SA_POS    }, .separator = {','} },  
                                                                 { .dict_id = { _OPTION_SA_STRAND }, .separator = {','} },  
                                                                 { .dict_id = { _OPTION_SA_CIGAR  }, .separator = {','} },  
                                                                 { .dict_id = { _OPTION_SA_MAPQ   }, .separator = {','} },  
                                                                 { .dict_id = { _OPTION_SA_NM     },                  } } };

    decl_ctx (OPTION_SA_Z);
    bool has_prim = sam_has_prim; // has a saggy, and that saggy is a prim line

    if (!IS_SAG_SA) goto fallback;

    switch (vb->comp_i) {

    // MAIN: special snip with two modes: "normal" if either line has no prim, or line has a matching prim, 
    // or "abnormal", is line has prim, but it is not matching. Container, if needed goes into OPTION_SA_MAIN 
    // note: usually, we expect all lines to be normal, hence making the context an "all the same"  
    case SAM_COMP_MAIN: fallback: {
        bool has_matching_prim = has_prim && sam_seg_SA_field_is_depn_from_prim (vb, dl, STRa(sa));

        bool abnormal = has_prim && !has_matching_prim; // abnormal if has unmatching prim line

        seg_special1 (VB, SAM_SPECIAL_SA_main, '0' + !abnormal, ctx, 0);

        // we seg a container into OPTION_SA_MAIN if there is no prim line, or if the prim line doesn't match (=abnormal)
        if (abnormal || !has_prim) {
            SegCallback callbacks[NUM_SA_ITEMS] = { 
                [SA_RNAME] = chrom_seg_cb, 
                [SA_POS]   = seg_pos_field_cb, 
                [SA_CIGAR] = (segconf_running && segconf.SA_CIGAR_abbreviated == unknown) ? sam_segconf_SA_cigar_cb : sam_seg_0A_cigar_cb, 
                [SA_MAPQ]  = sam_seg_0A_mapq_cb 
            };

            seg_array_of_struct (VB, CTX(OPTION_SA_MAIN), container_SA, STRa(sa), callbacks, 
                                 segconf.sam_semcol_in_contig ? sam_seg_correct_for_semcol_in_contig : NULL,
                                 add_bytes); 
        }

        // special function can reconstruct depn SA from prim SA - no need to seg anything else
        else {
            ctx->txt_len += add_bytes;
            CTX(OPTION_NM_i)->local_always = true; // carry STORE_INT even if no NM:i in file (required so sam_piz_special_COPY_BUDDY considers a historical NM:i to be 0 if non-existent). In novoalign, NM:i=0 might be omitted.
        }

        break;
    }

    // PRIM - seg against SA Group for reconstruction, and seg normally for consumption by sam_piz_load_sags
    case SAM_COMP_PRIM: {
        sam_seg_against_sa_group (vb, ctx, 0); // This will be reconstructed

        // we need to seg the primary NM before the SA NMs, if not segged yet
        if (!segconf.SA_NM_by_CIGAR_X) {
            ASSERT (dl->NM_len, "%s: PRIM line with SA is missing NM:i. Not expecting MAIN to send this line to PRIM", LN_NAME);
            sam_seg_NM_i (vb, dl, dl->NM, dl->NM_len);
        }

        SegCallback callbacks[NUM_SA_ITEMS] = { [SA_RNAME]=chrom_seg_cb, [SA_POS]=seg_pos_field_cb, [SA_CIGAR]=sam_seg_0A_cigar_cb, [SA_MAPQ]=sam_seg_0A_mapq_cb };            
        int32_t num_alns = 1/*primary aln*/ + seg_array_of_struct (VB, ctx, container_SA, STRa(sa), callbacks,  // 0 if SA is malformed 
                                                                   segconf.sam_semcol_in_contig ? sam_seg_correct_for_semcol_in_contig : NULL,
                                                                   add_bytes); 


        // We already tested the SA to be good when we added this line to PRIM in sam_seg_prim_add_sag_SA
        ASSSEG (IN_RANGX (num_alns, 2, MAX_SA_NUM_ALNS), "%s: Not expecting a malformed SA field in PRIM. num_alns=%u SA:Z=\"%.*s\"", 
                LN_NAME, num_alns, STRf(sa));

        // use SA.local to store number of alignments in this SA Group (inc. primary)
        dyn_int_append (VB, ctx, num_alns, 0); // this is always LT_UINT8 or LT_UINT16, because sam_seg_is_gc_line->sam_seg_prim_add_sag only makes a line into PRIM if num_alns <= MAX_SA_NUM_ALNS

        // PRIM: Remove the container b250 - Reconstruct will consume the SPECIAL_pull_from_sag, and sam_piz_load_sags will
        // consume OPTION_SA_* (to which we have already added the main fields of this line - RNAME, POS...)
        b250_seg_remove_last (VB, ctx, WORD_INDEX_NONE);

        // build SA Group structure in VB, to be later ingested into z_file->sa_*
        sam_seg_prim_add_sag_SA (vb, dl, STRa(sa), segconf.SA_NM_by_CIGAR_X ? vb->mismatch_bases_by_CIGAR : dl->NM, IS_BAM_ZIP);
        break;
    }

    // DEPN with SA Group - Seg against SA Group if we have one, or just a container if we don't
    case SAM_COMP_DEPN:
        if (vb->sag)  // sam_sa_seg_depn_find_sagroup verified that the group matches the SA field 
            sam_seg_against_sa_group_int (vb, ctx, vb->prim_aln_index_in_SA_Z, sa_len+1); // +1 for \t in SAM and \0 in BAM

        else {
            SegCallback callbacks[NUM_SA_ITEMS] = { [SA_RNAME]=chrom_seg_cb, [SA_POS]=seg_pos_field_cb, [SA_CIGAR]=sam_seg_0A_cigar_cb, [SA_MAPQ]=sam_seg_0A_mapq_cb };            
            seg_array_of_struct (VB, ctx, container_SA, STRa(sa), callbacks, 
                                 segconf.sam_semcol_in_contig ? sam_seg_correct_for_semcol_in_contig : NULL,
                                 add_bytes); 
        }
        break;

    default: ABORT ("Invalid comp_i=%u", vb->comp_i);
    }

    dl->SA = TXTWORD(sa);

    COPY_TIMER (sam_seg_SA_Z);
}

//---------
// PIZ
//---------

// used for OA, SA
bool sam_seg_0A_mapq_cb (VBlockP vb, ContextP ctx, STRp (mapq_str), uint32_t repeat)
{
    uint8_t mapq; // 8 bit by BAM specification of main field MAPQ
    if (!str_get_int_range8 (STRa(mapq_str), 0, 255, &mapq)) return false;
    
    seg_integer_fixed (VB, ctx, &mapq, false, mapq_str_len);
    return true;
}

static inline bool sam_piz_SA_field_is_line_matches_aln (VBlockSAMP vb, ContextP ctx,
                                                         STRp(my_rname), PosType32 my_pos, char my_strand, int64_t my_mapq, int64_t my_nm, 
                                                         STRp(aln))
{
    str_split (aln, aln_len, NUM_SA_ITEMS, ',', item, true);
    ASSPIZ (n_items == NUM_SA_ITEMS, "Invalid SA alignment: %.*s", STRf(aln));

    // temporarily replace S with H
    StoH stoh = ctx->flags.depn_clip_hard ? sam_cigar_S_to_H (vb, (char*)STRi(item, SA_CIGAR), false) : (StoH){};

    // pos, mapq, nm
    int64_t aln_pos, aln_mapq, aln_nm;
    bool found = 
        /*strand*/ *items[SA_STRAND] == my_strand &&
        /*cigar */ str_issame_(STRb(vb->textual_cigar), STRi(item, SA_CIGAR)) &&
        /*rname */ str_issame_(STRa(my_rname), STRi(item, SA_RNAME)) &&
        /*pos   */ str_get_int (STRi(item, SA_POS),  &aln_pos)  && aln_pos  == my_pos;

    // we also compare MAPQ and NM, unless --coverage or --count in which case these fields are skipped (because NM requires MD which requires SQBITMAP...)
    if (!flag.collect_coverage && !flag.count && !OUT_DT(FASTQ))
        found &=
        /*mapq  */ str_get_int (STRi(item, SA_MAPQ), &aln_mapq) && aln_mapq == my_mapq &&
        /*NM:i  */ str_get_int (STRi(item, SA_NM),   &aln_nm)   && aln_nm   == my_nm;

    sam_cigar_restore_S (stoh);

    return found;
}

// called to reconstruct SA in MAIN component
SPECIAL_RECONSTRUCTOR_DT (sam_piz_special_SA_main)
{
    VBlockSAMP vb = (VBlockSAMP)vb_;

    bool normal = snip[0] - '0';

    // case: we segged a container 
    if (!sam_has_prim || !normal) 
        reconstruct_from_ctx (vb, OPTION_SA_MAIN, 0, reconstruct);

    // case: we segged against the same-VB prim line
    else if (reconstruct) {        
        // step 1: reconstruct prim alignments
        sam_piz_special_COPY_BUDDY (VB, CTX(SAM_RNAME), (char[]){'0' + BUDDY_SAGGY }, 1, new_value, true); 
        RECONSTRUCT1(',');
        
        sam_piz_special_COPY_BUDDY (VB, CTX(SAM_POS), (char[]){'0' + BUDDY_SAGGY }, 1, new_value, true); 
        RECONSTRUCT1(',');
        
        SamFlags prim_flags = (SamFlags) {.value = *B(int64_t, CTX(SAM_FLAG)->history, vb->saggy_line_i) };
        RECONSTRUCT1 (prim_flags.rev_comp ? '-' : '+');
        RECONSTRUCT1(',');

        sam_piz_special_COPY_BUDDY_CIGAR (VB, CTX(SAM_CIGAR), (char[]){ '0' + BUDDY_SAGGY }, 1, new_value, true);
        RECONSTRUCT1(',');

        sam_piz_special_COPY_BUDDY (VB, CTX(SAM_MAPQ), (char[]){'0' + BUDDY_SAGGY }, 1, new_value, true); 
        RECONSTRUCT1(',');

        sam_piz_special_COPY_BUDDY (VB, CTX(OPTION_NM_i), (char[]){'0' + BUDDY_SAGGY }, 1, new_value, true); 
        RECONSTRUCT1(';');

        // step 2: all alignments in prim SA, except the one matching this line
        HistoryWord *hw = B(HistoryWord, CTX(OPTION_SA_Z)->history, vb->saggy_line_i);
        rom prim_SA = (hw->lookup == LookupTxtData) ? Btxt (hw->index) 
                                                    : Bc (CTX(OPTION_SA_Z)->per_line, hw->index);
        
        str_split (prim_SA, hw->len, MAX_SA_NUM_ALNS, ';', prim_aln, false);
        n_prim_alns--; // -1 due to terminal ';'

        // get alignment items of current (depn) line
        int64_t my_pos  = CTX(SAM_POS)->last_value.i;
        int64_t my_mapq = CTX(SAM_MAPQ)->last_value.i;
        
        // note: a non existing NM:i is taken as 0
        int64_t my_nm   = sam_piz_line_has_aux_field (vb, _OPTION_NM_i) ? reconstruct_peek (VB, CTX(OPTION_NM_i), 0, 0).i : 0;
        
        char my_strand  = last_flags.rev_comp ? '-' : '+';

        STR(my_rname);
        ctx_get_snip_by_word_index (CTX(SAM_RNAME), CTX(SAM_RNAME)->last_value.i, my_rname);

        // to do: improve effeciency by testing first the predicted alignment (vb->line-vb->saggy_line_i-1)
        bool found = false;
        for (int i=0; i < n_prim_alns; i++) { // =1 bc of last empty alignment due terminal ';'
            if (!found && ((found = sam_piz_SA_field_is_line_matches_aln (vb, ctx, STRa(my_rname), my_pos, my_strand, my_mapq, my_nm, STRi(prim_aln, i)))))
                continue; // skip the alignment matching this line

            RECONSTRUCT_SEP (prim_alns[i], prim_aln_lens[i], ';');
        }
        ASSPIZ (found, "Unexpectedly, primary SA=\"%.*s\" does not contain an alignment matching this line=%.*s,%"PRId64",%c,%.*s,%"PRId64",%"PRId64" (depn_clip_hard=%s)", 
                STRfw(*hw), STRf(my_rname), my_pos, my_strand, STRfb(vb->textual_cigar), my_mapq, my_nm, TF(ctx->flags.depn_clip_hard));
    }

    return NO_NEW_VALUE;
}

void sam_piz_SA_get_saggy_prim_item (VBlockSAMP vb, int sa_item, pSTRp(out))
{
    STR(SA);
    sam_reconstruct_from_buddy_get_textual_snip (vb, CTX (OPTION_SA_Z), BUDDY_SAGGY, pSTRa(SA));
    
    str_split (SA, SA_len, MAX_SA_NUM_ALNS, ';', prim_aln, false); // split SA:Z into alignments

    int predicted_aln_i = segconf.is_sorted ? 0 : (vb->line_i - vb->saggy_line_i) - 1;
    ASSPIZ (predicted_aln_i < n_prim_alns-1, "predicted_aln_i=%d ∉ [0,%d]", predicted_aln_i, n_prim_alns-2);
    
    str_split (prim_alns[predicted_aln_i], prim_aln_lens[predicted_aln_i], NUM_SA_ITEMS, ',', item, true); // split predicted alignment into its items
    ASSPIZ (n_items == NUM_SA_ITEMS, "Failed to split prim SA alignment \"%.*s\"", STRfi(prim_aln,predicted_aln_i));

    *out = items[sa_item]; // points into txt_data at prim alignment's SA:Z (always textual)
    *out_len = item_lens[sa_item];
}

SPECIAL_RECONSTRUCTOR_DT (sam_piz_special_COPY_PRIM)
{
    VBlockSAMP vb = (VBlockSAMP)vb_;

    int item_i = snip[0]-'0';

    STR(item);
    sam_piz_SA_get_saggy_prim_item (vb, item_i, pSTRa(item));
    if (reconstruct) RECONSTRUCT_str (item);   

    if (item_i == SA_POS || item_i == SA_MAPQ)
        str_get_int (STRa(item), &new_value->i);

    return HAS_NEW_VALUE;
}
