// ------------------------------------------------------------------
//   sam_barcodes.c
//   Copyright (C) 2020-2023 Genozip Limited
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited,
//   under penalties specified in the license.

#include "sam_private.h"
#include "reconstruct.h"
#include "lookback.h"

// fields used by STARsolo and 10xGenomics cellranger. Some are SAM-standard and some not.

// update context tag names if this file has UB/UR/UY which are aliased to BX/RX/QX
void sam_segconf_retag_UBURUY (void)
{
    strcpy (ZCTX(OPTION_BX_Z)->tag_name, "UB:Z");
    strcpy (ZCTX(OPTION_RX_Z)->tag_name, "UR:Z");
    strcpy (ZCTX(OPTION_QX_Z)->tag_name, "UY:Z");
    strcpy (ZCTX(OPTION_QX_DOMQRUNS)->tag_name, "U0Y_DOMQ");
    strcpy (ZCTX(OPTION_QX_QUALMPLX)->tag_name, "U1Y_MPLX");
    strcpy (ZCTX(OPTION_QX_DIVRQUAL)->tag_name, "U2Y_DEVQ");
}

//------------------------------------------------------------------------------------------------------------------------
// CB:Z - "Cell identifier, consisting of the optionally-corrected cellular barcode sequence and an optional suffix"
// CellRanger format: CB:Z:TCACTATCATGGCTGC-1 
// STARsolo format: CB:Z:GGTGCGAA_TCCGTCTA_CTAAGGGA
// Sam spec: multiple components and suffix allowed, recommended separator '-'.
//
// CR:Z - "Cellular barcode. The uncorrected sequence bases of the cellular barcode as reported by the sequencing machine"
// Example: CR:Z:GGTGCGAA_TCCGTCTA_CTAAGGGA (note: no suffix, even is CB:Z has one)
//------------------------------------------------------------------------------------------------------------------------

static void sam_seg_CB_Z_segconf (VBlockSAMP vb, STRp(cb))
{
    if      (memchr (cb, '_', cb_len)) segconf.CR_CB_seperator = '_'; // as observed in STARsolo-generated files
    else if (memchr (cb, '-', cb_len)) segconf.CR_CB_seperator = '-'; // as recommended by the SAM spec

    str_split (cb, cb_len, 0, segconf.CR_CB_seperator, item, false);

    bool has_suffix = (n_items > 1) && !str_is_ACGT (STRi(item, n_items-1), NULL);
         
    segconf.n_CR_CB_CY_seps = n_items - has_suffix - 1;

    if (n_items > SMALL_CON_NITEMS) return; // it won't fit in a SmallContainer - we will just seg as a whole

    segconf.CB_con = (SmallContainer){ .nitems_lo = n_items, .repeats = 1 };

    for (int i=0; i < n_items-1; i++)
        segconf.CB_con.items[i] = (ContainerItem){ .dict_id = { _OPTION_CB_ARR }, .separator = { segconf.CR_CB_seperator } };

    segconf.CB_con.items[n_items-1].dict_id.num = has_suffix ? _OPTION_CB_SUFFIX : _OPTION_CB_ARR;

    container_prepare_snip ((ContainerP)&segconf.CB_con, 0, 0, qSTRa(segconf.CB_con_snip));
}

static void sam_seg_CB_do_seg (VBlockSAMP vb, ContextP channel_ctx, STRp(cb), unsigned add_bytes)
{
    if (!seg_by_container (VB, channel_ctx, (ContainerP)&segconf.CB_con, STRa(cb), STRa(segconf.CB_con_snip), NULL, false, add_bytes))
        seg_add_to_local_string (VB, channel_ctx, STRa(cb), LOOKUP_SIMPLE, add_bytes); // requires no_stons
}

void sam_seg_CB_Z (VBlockSAMP vb, ZipDataLineSAM *dl, STRp(cb), unsigned add_bytes)
{
    START_TIMER;

    dl->solo_z_fields[SOLO_CB] = TXTWORD(cb);

    if (segconf.running && !segconf.CB_con.repeats) 
        sam_seg_CB_Z_segconf (vb, STRa(cb));

    if (IS_DEPN(vb) && vb->sag && IS_SAG_SOLO && str_issame_(STRa(cb), STRBw(z_file->solo_data, vb->solo_aln->word[SOLO_CB])))
        sam_seg_against_sa_group (vb, CTX(OPTION_CB_Z), add_bytes);

    else
        sam_seg_buddied_Z_fields (vb, dl, MATED_CB, STRa(cb),  
                                  segconf.CB_con.repeats ? sam_seg_CB_do_seg : (SegBuddiedCallback)0, // careful in case container is not initialized
                                  add_bytes);

    COPY_TIMER(sam_seg_CB_Z);
}

static void sam_seg_CR_do_seg (VBlockSAMP vb, ContextP channel_ctx, STRp(cr), unsigned add_bytes)
{
    // diff against CB if possible
    if (has(CB_Z)) {
        STR(cb);
        sam_seg_get_aux_Z (vb, vb->idx_CB_Z, pSTRa(cb), IS_BAM_ZIP); // base field can be before or after
        seg_set_last_txt (VB, CTX(OPTION_CB_Z), STRa(cb));

        if (cb_len < cr_len) goto fallback; // can't diff

        seg_diff (VB, channel_ctx, CTX(OPTION_CB_Z), STRa(cr), false, add_bytes); 
    }
    
    else fallback: {
        // add "exception" (i.e. undiffable) cr to CR_X
        seg_add_to_local_blob (VB, CTX(OPTION_CR_Z_X), STRa(cr), add_bytes);

        // add redirection CR -> CR_X
        seg_by_ctx (VB, STRa(redirect_to_CR_X_snip), channel_ctx, 0); 
    }
}

void sam_seg_CR_Z (VBlockSAMP vb, ZipDataLineSAM *dl, STRp(cr), unsigned add_bytes)
{
    START_TIMER;

    dl->solo_z_fields[SOLO_CR] = TXTWORD(cr);

    if (IS_DEPN(vb) && vb->sag && IS_SAG_SOLO && str_issame_(STRa(cr), STRBw(z_file->solo_data, vb->solo_aln->word[SOLO_CR])))
        sam_seg_against_sa_group (vb, CTX(OPTION_CR_Z), add_bytes);

    // copy CB, and if different than CB - xor against it
    else
        sam_seg_buddied_Z_fields (vb, dl, MATED_CR, STRa(cr), sam_seg_CR_do_seg, add_bytes);                                

    COPY_TIMER(sam_seg_CR_Z);
}

//-----------------------------------------------------------------------------------------------------
// QT:Z - "Phred quality of the sample barcode sequence in the BC:Z tag"
// CY:Z - "Phred quality of the cellular barcode sequence in the CR tag"
//-----------------------------------------------------------------------------------------------------

// Note: Seggable against prim, but not against mate (since each mate might have different quality for its barcode sequence)
// Note: Might be multiple elements seperated by a space (spec recommentation) or '_' (observed in STARsolo) 
// Requirements: 1. array did, 2. zip callback function
bool sam_seg_barcode_qual (VBlockSAMP vb, ZipDataLineSAM *dl, Did did_i, SoloTags solo, uint8_t n_seps, 
                           STRp(qual), qSTRp (con_snip), MiniContainerP con, unsigned add_bytes)
{
    START_TIMER;

    Did array_did_i = did_i + 1; // must be one after
    bool dont_compress = false;

    dl->solo_z_fields[solo] = TXTWORD(qual);

    if (segconf.running && !con_snip[0]) {
        char sep = 0;
        if      (str_count_char (STRa(qual), ' ') == n_seps) sep = ' '; // as recommended by the SAM spec (not valid QUAL phred score)
        // note: '_' might be a valid phred score - hence the additional check of verifying n_seps
        else if (str_count_char (STRa(qual), '_') == n_seps) sep = '_'; // as observed in STARsolo-generated files
    
        if (sep) {
            str_split (qual, qual_len, 0, sep, item, false);
            
            *con = (MiniContainer){
                .nitems_lo         = 1,
                .repeats           = n_items,
                .repsep            = { sep },
                .drop_final_repsep = true,
                .items[0]          = { .dict_id = CTX(array_did_i)->dict_id }
            };

            container_prepare_snip ((ContainerP)con, 0, 0, STRa(con_snip));
        }
    }

    // seg against sag
    if (IS_DEPN(vb) && vb->sag && IS_SAG_SOLO && str_issame_(STRa(qual), STRBw(z_file->solo_data, vb->solo_aln->word[solo]))) 
        sam_seg_against_sa_group (vb, CTX(did_i), add_bytes);
    
    // seg as an array - with items in local
    else if (con_snip[0]) {

        str_split (qual, qual_len, con->repeats, con->repsep[0], item, true);
        if (!n_items) goto fallback;

        for (int i=0; i < n_items; i++) 
            seg_add_to_local_blob (VB, CTX(array_did_i), STRi(item,i), item_lens[i]);

        seg_by_did (VB, con_snip, *con_snip_len, did_i, (add_bytes - qual_len) + (n_items-1)); // account for separators
    }

    else fallback: 
        // note: this goes into the primary did_i, not the array item
        seg_add_to_local_string (VB, CTX(did_i), STRa(qual), LOOKUP_SIMPLE, add_bytes);

    COPY_TIMER(sam_seg_barcode_qual);
    return dont_compress;
}

//------------------------------------------------------------------------------------------------------------------------
// RX:Z (SAM standard tag - longranger, novoalign, agilent) ; UR:Z (cellranger) - "Chromium molecular barcode sequence as reported by the sequencer"
// BX:Z (longranger) ; UB:Z (cellranger) - "Chromium molecular barcode sequence that is error-corrected among other molecular barcodes with the same cellular barcode and gene alignment"
//------------------------------------------------------------------------------------------------------------------------

static void sam_seg_RX_do_seg (VBlockSAMP vb, ContextP channel_ctx, STRp(rx), unsigned add_bytes)
{
    // diff against UB, but not if length is 1 (in STARsolo, UB is always "-" in this case while UR is a single base)
    if (rx_len > 1 && (has(UB_Z) || has(BX_Z))) {
        STR(bx);
        sam_seg_get_aux_Z (vb, has(BX_Z) ? vb->idx_BX_Z : vb->idx_UB_Z, pSTRa(bx), IS_BAM_ZIP); // base field can be before or after
        seg_set_last_txt (VB, CTX(OPTION_BX_Z), STRa(bx));

        if (bx_len < rx_len) goto fallback; // can't diff

        seg_diff (VB, channel_ctx, CTX(OPTION_BX_Z), STRa(rx), false, add_bytes); 
    }
    
    else fallback: {
        // if undiffable, store verbatim in RX_X
        seg_add_to_local_blob (VB, CTX(OPTION_RX_Z_X), STRa(rx), add_bytes);

        // add redirection RX -> RX_X
        seg_by_ctx (VB, STRa(redirect_to_RX_X_snip), channel_ctx, 0); 
    }
}

static void sam_seg_RX_array (VBlockSAMP vb, ContextP ctx, STRp(rx), unsigned add_bytes)
{
    seg_array (VB, ctx, OPTION_RX_Z, STRa(rx), segconf.RX_sep, 0, false, STORE_NONE, _OPTION_RX_Z_X, add_bytes);
}

void sam_seg_RX_Z (VBlockSAMP vb, ZipDataLineSAM *dl, STRp(rx), unsigned add_bytes)
{
    START_TIMER;

    // In Novoalign, AGeNT Trimmer (and maybe others) RX can consist of multiple barcodes eg: "GTCCCT-TTTCTA"
    if (segconf.running && !segconf.n_RX_seps) {    
        if      (memchr (rx, '_', rx_len)) segconf.RX_sep = '_'; 
        else if (memchr (rx, '-', rx_len)) segconf.RX_sep = '-'; // as recommended by the SAM spec

        if (segconf.RX_sep)
            segconf.n_RX_seps = str_count_char (STRa(rx), segconf.RX_sep);
    }

    dl->solo_z_fields[SOLO_RX] = TXTWORD(rx);

    if (IS_DEPN(vb) && vb->sag && IS_SAG_SOLO && str_issame_(STRa(rx), STRBw(z_file->solo_data, vb->solo_aln->word[SOLO_RX]))) 
        sam_seg_against_sa_group (vb, CTX(OPTION_RX_Z), add_bytes);

    else if (segconf.has_agent_trimmer)                               // Agilent case - expected to be predictable from ZA:Z and ZB:B, so no need to mate
        agilent_seg_RX (VB, CTX(OPTION_RX_Z), STRa(rx), add_bytes);
        
    else
        sam_seg_buddied_Z_fields (vb, dl, MATED_RX, STRa(rx), 
                                  segconf.RX_sep ? sam_seg_RX_array   // Novoalign-like case - array of short barcodes
                                                 : sam_seg_RX_do_seg, // cellranger/longranger-like case - long barcodes, possibly diffed against UB/BX
                                  add_bytes);                                

    COPY_TIMER (sam_seg_RX_Z);
}

void sam_seg_BX_Z (VBlockSAMP vb, ZipDataLineSAM *dl, STRp(bx), unsigned add_bytes)
{
    START_TIMER;

    dl->solo_z_fields[SOLO_BX] = TXTWORD(bx);

    if (IS_DEPN(vb) && vb->sag && IS_SAG_SOLO && str_issame_(STRa(bx), STRBw(z_file->solo_data, vb->solo_aln->word[SOLO_BX])))
        sam_seg_against_sa_group (vb, CTX(OPTION_BX_Z), add_bytes);

    else
        sam_seg_buddied_Z_fields (vb, dl, MATED_BX, STRa(bx), 0, add_bytes);

    COPY_TIMER (sam_seg_BX_Z);
}

//-----------------------------------------------------------------------------------------------------
// QX:Z (longranger, novoalign, AGeNT Trimmer) UY:Z (cellranger) - "Chromium molecular barcode read quality. Phred scores as reported by sequencer"
//-----------------------------------------------------------------------------------------------------

void sam_seg_QX_Z (VBlockSAMP vb, ZipDataLineSAM *dl, STRp(qx), unsigned add_bytes)
{
    START_TIMER;

    dl->solo_z_fields[SOLO_QX] = TXTWORD(qx);

    if (IS_DEPN(vb) && vb->sag && IS_SAG_SOLO && str_issame_(STRa(qx), STRBw(z_file->solo_data, vb->solo_aln->word[SOLO_QX]))) {
        sam_seg_against_sa_group (vb, CTX(OPTION_QX_Z), add_bytes);
        dl->dont_compress_QX = true;
    }
    
    else if (segconf.has_agent_trimmer) 
        agilent_seg_QX (VB, CTX(OPTION_QX_Z), STRa(qx), add_bytes);

    else {
        CTX(OPTION_QX_Z)->local.len32 += qx_len;
        seg_lookup_with_length (VB, CTX(OPTION_QX_Z), qx_len, add_bytes); // actual data will be accessed by the codec with a callback
    }

    COPY_TIMER (sam_seg_QX_Z);
}


//-----------------------------------------------------------------------------------------------------
// BC:Z - "Barcode sequence (Identifying the sample/library)"
//-----------------------------------------------------------------------------------------------------

static void sam_seg_BC_array (VBlockSAMP vb, ContextP ctx, STRp(bc), unsigned add_bytes)
{
    seg_array (VB, ctx, OPTION_BC_Z, STRa(bc), segconf.BC_sep, 0, false, STORE_NONE, _OPTION_BC_ARR, add_bytes);
}

void sam_seg_BC_Z (VBlockSAMP vb, ZipDataLineSAM *dl, STRp(bc), unsigned add_bytes)
{
    START_TIMER;

    if (segconf.running && !segconf.n_BC_QT_seps) {    
        if      (memchr (bc, '_', bc_len)) segconf.BC_sep = '_'; 
        else if (memchr (bc, '-', bc_len)) segconf.BC_sep = '-'; // as recommended by the SAM spec

        if (segconf.BC_sep)
            segconf.n_BC_QT_seps = str_count_char (STRa(bc), segconf.BC_sep);
    }

    dl->solo_z_fields[SOLO_BC] = TXTWORD(bc);

    if (IS_DEPN(vb) && vb->sag && IS_SAG_SOLO && str_issame_(STRa(bc), STRBw(z_file->solo_data, vb->solo_aln->word[SOLO_BC]))) 
        sam_seg_against_sa_group (vb, CTX(OPTION_BC_Z), add_bytes);

    else 
        sam_seg_buddied_Z_fields (vb, dl, MATED_BC, STRa(bc),  
                                  segconf.n_BC_QT_seps ? sam_seg_BC_array : 0,
                                  add_bytes);

    COPY_TIMER (sam_seg_BC_Z);
}

//-----------------------------------------------------------------------------------------------------
// GN:Z - STARsolo: Gene name for unique-gene reads. CellRanger: (;-seperated list) 
// GX:Z - STARsolo: Gene ID for for unique-gene reads. CellRanger: (;-seperated list)
// gn:z - STARsolo: Gene names for unique- and multi-gene reads (;-seperated list)
// gx:z - STARsolo: Gene IDs for unique- and multi-gene reads (;-seperated list)
//-----------------------------------------------------------------------------------------------------

void sam_seg_gene_name_id (VBlockSAMP vb, ZipDataLineSAM *dl, Did did_i, STRp(value), unsigned add_bytes)
{
    START_TIMER;

    ctx_set_encountered (VB, CTX(did_i));
    seg_set_last_txt (VB, CTX(did_i), STRa(value));

    seg_array (VB, CTX(did_i), did_i, STRa(value), ';', 0, false, STORE_NONE, DICT_ID_NONE, add_bytes);

    COPY_TIMER (sam_seg_gene_name_id);
}

//-----------------------------------------------------------------------------------------------------
// fx:Z - CellRanger: Feature identifier matched to this Feature Barcode read. Specified in the id column of the feature reference.
//-----------------------------------------------------------------------------------------------------

void sam_seg_fx_Z (VBlockSAMP vb, ZipDataLineSAM *dl, STRp(fx), unsigned add_bytes)
{
    START_TIMER;

    // it is often a copy of GX. TO DO: support case where fx is before GX (bug 623)
    if (ctx_encountered_in_line (VB, OPTION_GX_Z)) {
        if (str_issame_(STRa(fx), last_txtx(vb, CTX(OPTION_GX_Z)), vb->last_txt_len(OPTION_GX_Z)))
            seg_by_did (VB, STRa(copy_GX_snip), OPTION_fx_Z, add_bytes);

        else
            goto fallback;
    }

    else fallback:
        seg_by_did (VB, STRa(fx), OPTION_fx_Z, add_bytes);

    COPY_TIMER (sam_seg_fx_Z);
}

//-----------------------------------------------------------------------------------------------------
// 2R:Z - cellranger
// TQ:Z - longranger & cellranger: Sequence of the 7 trimmed bases following the barcode sequence at the start of R1. Can be used to reconstruct the original R1 sequence.
//-----------------------------------------------------------------------------------------------------

void sam_seg_other_seq (VBlockSAMP vb, ZipDataLineSAM *dl, Did did_i, STRp(seq), unsigned add_bytes)
{
    START_TIMER;

    seg_add_to_local_blob (VB, CTX(did_i), STRa(seq), add_bytes);

    COPY_TIMER (sam_seg_other_seq);
}

//-----------------------------------------------------------------------------------------------------
// GR:Z
//-----------------------------------------------------------------------------------------------------

void sam_seg_GR_Z (VBlockSAMP vb, ZipDataLineSAM *dl, STRp(gr), unsigned add_bytes)
{
    START_TIMER;

    // diff against CR if possible
    if (has(CR_Z)) {
        STR(cr);
        sam_seg_get_aux_Z (vb, vb->idx_CR_Z, pSTRa(cr), IS_BAM_ZIP); // base field can be before or after
        seg_set_last_txt (VB, CTX(OPTION_CR_Z), STRa(cr));

        if (cr_len < gr_len) goto fallback; // can't diff

        seg_diff (VB, CTX(OPTION_GR_Z), CTX(OPTION_CR_Z), STRa(gr), false, add_bytes); 
    }
    
    else fallback: {
        // add "exception" (i.e. undiffable) cr to CR_X
        seg_add_to_local_blob (VB, CTX(OPTION_GR_Z_X), STRa(gr), add_bytes);
        // SNIPi1 (SNIP_LOOKUP, gr_len);
        // seg_by_did (VB, STRa(snip), OPTION_GR_Z_X, add_bytes);

        // add redirection GR -> GR_X
        seg_by_ctx (VB, STRa(redirect_to_GR_X_snip), CTX(OPTION_GR_Z), 0); 
    }

    COPY_TIMER (sam_seg_GR_Z);
}

//-----------------------------------------------------------------------------------------------------
// GY:Z
//-----------------------------------------------------------------------------------------------------

void sam_seg_GY_Z (VBlockSAMP vb, ZipDataLineSAM *dl, STRp(gy), unsigned add_bytes)
{
    START_TIMER;

    // diff against CY if possible
    if (has(CY_Z)) {
        STR(cy);
        sam_seg_get_aux_Z (vb, vb->idx_CY_Z, pSTRa(cy), IS_BAM_ZIP); // base field can be before or after
        seg_set_last_txt (VB, CTX(OPTION_CY_Z), STRa(cy));

        if (cy_len < gy_len) goto fallback; // can't diff

        seg_diff (VB, CTX(OPTION_GY_Z), CTX(OPTION_CY_Z), STRa(gy), false, add_bytes); 
    }
    
    else fallback: {
        // add "exception" (i.e. undiffable) cy to CY_X
        seg_add_to_local_blob (VB, CTX(OPTION_GY_Z_X), STRa(gy), add_bytes);

        // add redirection GY -> GY_X
        seg_by_ctx (VB, STRa(redirect_to_GY_X_snip), CTX(OPTION_GY_Z), 0); 
    }

    COPY_TIMER (sam_seg_GY_Z);
}


//-----------------------------------------------------------------------------------------------------
// TX:Z - cellranger: Transcript list
// Example: exon: (RE:Z=E) "TX:Z:ENST00000368838,+542,90M;ENST00000368841,+952,90M;ENST00000368843,+1118,90M;ENST00000458013,+1097,90M;ENST00000482791,+268,90M"
//        intron: (RE:Z=N) "TX:Z:ENSG00000241860,+"
//
// AN:Z - cellranger: Antisense transcript list
// Example: "AN:Z:hg19_ENST00000456328,-8,91M;hg19_ENST00000515242,-5,91M;hg19_ENST00000518655,-3,91M"
//-----------------------------------------------------------------------------------------------------

// For files generated with 10xGenomics cellranger: TX:Z and AN:Z fields
//
// from the manual: https://support.10xgenomics.com/single-cell-gene-expression/software/pipelines/latest/output/bam
//
// TX: "Present in reads aligned to the same strand as the transcripts in this semicolon-separated list that are 
// compatible with this alignment. Transcripts are specified with the transcript_id key in the reference GTF attribute 
// column. The format of each entry is [transcript_id],[strand][pos],[cigar]. strand is + as reads with this annotation 
// were correctly aligned in the expected orientation (in contrast to the AN tag below, where the strand is - to 
// indicate antisense alignments. pos is the alignment offset in transcript coordinates, and cigar is the CIGAR string 
// in transcript coordinates."
//
// AN: "Present for reads that are aligned to the antisense strand of annotated transcripts. If intron counts are not 
// included (with include-introns=false), this tag is the same as the TX tag but with - values for the strand 
// identifier. If introns are included (include-introns=true), the AN tag contains the corresponding antisense gene 
// identifier values (starting with ENSG) rather than transcript identifier values (starting with ENST)."
//

#define lookback_value last_value.i

void sam_seg_TX_AN_initialize (VBlockSAMP vb, Did did_i)
{
    ContextP lookback_ctx = CTX(did_i + 1);
    ContextP negative_ctx = CTX(did_i + 2);
    ContextP gene_ctx     = CTX(did_i + 3);
    ContextP pos_ctx      = CTX(did_i + 5);
    ContextP cigar_ctx    = CTX(did_i + 6);
    ContextP sam_pos_ctx  = CTX(did_i + 7);

    cigar_ctx->no_stons = true;  // required by sam_seg_other_CIGAR
    pos_ctx->ltype      = LT_DYN_INT;

    if (segconf.is_sorted) { // the lookback method is only for sorted files    
        gene_ctx->no_stons          = true;  // as we store by index
        negative_ctx->no_stons      = true;  // as we store by index
        negative_ctx->flags.lookback0_ok = true;  // a SNIP_LOOKBACK when there is no lookback is ok, since anyway we would not use this value

        lookback_ctx->flags.store   = STORE_INT;
        lookback_ctx->ltype         = LT_DYN_INT;
        lookback_ctx->local_param   = true;
        lookback_ctx->local.prm8[0] = lookback_size_to_local_param (1024);
        lookback_ctx->local_always  = (lookback_ctx->local.param != 0); // no need for a SEC_LOCAL section if the parameter is 0 (which is the default anyway)

        lookback_init (VB, lookback_ctx, negative_ctx, STORE_INDEX); // lookback_ctx->local.param must be set before
        lookback_init (VB, lookback_ctx, gene_ctx,     STORE_INDEX); 
        lookback_init (VB, lookback_ctx, pos_ctx,      STORE_INT);
        lookback_init (VB, lookback_ctx, sam_pos_ctx,  STORE_INT);

        ctx_create_node (VB, negative_ctx->did_i, cSTR("+"));  // positive: word_index=0
        ctx_create_node (VB, negative_ctx->did_i, cSTR("-"));  // negative: word_index=1

        ctx_create_node (VB, sam_pos_ctx->did_i, STRa(copy_POS_snip));  // word_index=0
    }

    // case: we won't be using lookback: create dummy all-the-same contexts (nodes will be deleted in sam_seg_finalize if not used)
    else {
        ctx_create_node (VB, negative_ctx->did_i, "dummy", 5);
        ctx_create_node (VB, lookback_ctx->did_i, "dummy", 5);
    }
}

// Seg lookback callback for GENE item of TX or AN: seg against lookback if there is one
static bool sam_seg_TX_AN_gene (VBlockP vb, ContextP gene_ctx, STRp(gene), uint32_t rep)
{
    ContextP container_ctx = gene_ctx - 3;
    ContextP lb_ctx        = gene_ctx - 2;

    // create node, but don't seg yet - just get the node_index
    bool is_new;
    WordIndex gene_index = ctx_create_node_is_new (vb, gene_ctx->did_i, STRa (gene), &is_new);

    int64_t iterator = -1;
    int64_t lookback = is_new ? 0 : lookback_get_next (vb, lb_ctx, gene_ctx, gene_index, &iterator); // the previous entry with the same gene

    if (lookback && container_ctx->did_i == OPTION_TX_Z)
        seg_by_ctx (vb, STRa(TX_lookback_snip), gene_ctx, gene_len); 

    else if (lookback && container_ctx->did_i == OPTION_AN_Z)
        seg_by_ctx (vb, STRa(AN_lookback_snip), gene_ctx, gene_len); 

    else 
        // no lookback - seg normally. Note: we can't seg_id_field because we are looking back by index
        gene_index = seg_by_ctx (vb, STRa(gene), gene_ctx, gene_len); 

    seg_add_to_local_resizable (vb, lb_ctx, lookback, 0);

    lookback_insert (vb, lb_ctx->did_i, gene_ctx->did_i, false, (int64_t)gene_index);
    
    container_ctx->lookback_value = lookback; // for use when segging POS

    return true; // segged successfully
}

static void sam_seg_TX_AN_negative (VBlockP vb, ContextP container_ctx, ContextP negative_ctx, bool negative, bool is_same_as_lookback)
{
    if (is_same_as_lookback && container_ctx->did_i == OPTION_TX_Z) 
        seg_by_ctx (vb, STRa(TX_lookback_snip), negative_ctx, 0); 

    else if (is_same_as_lookback && container_ctx->did_i == OPTION_AN_Z)
        seg_by_ctx (vb, STRa(AN_lookback_snip), negative_ctx, 0); 

    else 
        seg_known_node_index (vb, negative_ctx, negative, 0);
}

static bool sam_seg_TX_AN_sam_pos (VBlockP vb, ContextP sam_pos_ctx, STRp(value), uint32_t rep)
{
    seg_known_node_index (VB, sam_pos_ctx, 0, 0); // node created in initialize

    return true; // segged successfully
}

// Seg lookback callback for POS item of TX o RAN:
// - predict that pos distance from lookback pos is the same as SAM_POS distance
// - and replace the already segged GENE with a lookback if there is one.
static bool sam_seg_TX_AN_pos (VBlockP vb, ContextP pos_ctx, STRp(pos), uint32_t rep)
{
    ContextP container_ctx = pos_ctx - 5;
    ContextP lb_ctx        = pos_ctx - 4;
    ContextP negative_ctx  = pos_ctx - 3;
    ContextP sam_pos_ctx   = pos_ctx + 2; // note: we use the cigar loopback buffer (otherwise not needed) to store the corresponding SAM_POS

    int64_t lookback = container_ctx->lookback_value; // set here in sam_seg_TX_AN_gene

    PosType64 this_sam_pos = DATA_LINE(vb->line_i)->POS;

    PosType64 this_pos; 
    if (!str_get_int (STRa(pos), &this_pos)) return false; // not the format we're expecting
    bool negative=0;

    if (lookback) {
        PosType64 lookback_pos     = lookback_get_value (vb, lb_ctx, pos_ctx,      lookback).i;
        PosType64 lookback_sam_pos = lookback_get_value (vb, lb_ctx, sam_pos_ctx,  lookback).i; 
        bool lookback_negative   = lookback_get_index (vb, lb_ctx, negative_ctx, lookback);

        PosType64 pos_delta = this_pos - lookback_pos; // can be positive or negative
        PosType64 sam_pos_delta = this_sam_pos - lookback_sam_pos; // always non-negative         

        negative = pos_delta < 0;
        if (negative) pos_delta = -pos_delta;

        sam_seg_TX_AN_negative (vb, container_ctx, negative_ctx, negative, lookback_negative == negative);

        // we predict that the difference in SAM_POS is the same as the difference in the TX/AN pos, and store the delta vs the prediction
        if (pos_delta == sam_pos_delta) {
            SNIPi2 (SNIP_SPECIAL, SAM_SPECIAL_TX_AN_POS, pos_delta - sam_pos_delta);
            seg_by_ctx (vb, STRa(snip), pos_ctx, pos_len);
        }
        else 
            seg_integer (vb, pos_ctx, this_pos, true, pos_len);
    }

    else { // no loopback
        // the value of negative for a non-loopback entry is consumed but not used, so we can seg whatever. we seg the most common value, 
        // "SNIP_LOOKBACK". This requires setting flags.lookback0_ok.
        sam_seg_TX_AN_negative (vb, container_ctx, negative_ctx, 0, true);
        
        seg_integer (vb, pos_ctx, this_pos, true, pos_len);
    }
    
    lookback_insert (vb, lb_ctx->did_i, pos_ctx->did_i,      false, (int64_t)this_pos);
    lookback_insert (vb, lb_ctx->did_i, sam_pos_ctx->did_i,  false, (int64_t)this_sam_pos); 
    lookback_insert (vb, lb_ctx->did_i, negative_ctx->did_i, false, (int64_t)negative);   

    return true; // segged successfully
}

static bool sam_seg_TX_AN_pos_unsorted (VBlockP vb, ContextP pos_ctx, STRp(pos), uint32_t rep)
{
    seg_integer_or_not (VB, pos_ctx, STRa(pos), pos_len);
    return true;
}

SPECIAL_RECONSTRUCTOR (sam_piz_special_TX_AN_POS)
{
    ContextP lb_ctx       = ctx - 4;
    ContextP negative_ctx = ctx - 3;
    ContextP sam_pos_ctx  = ctx + 2; 

    int64_t lookback = lb_ctx->last_value.i;

    PosType64 lookback_pos     = lookback_get_value (vb, lb_ctx, ctx, lookback).i;
    PosType64 lookback_sam_pos = lookback_get_value (vb, lb_ctx, sam_pos_ctx, lookback).i;
    PosType64 this_sam_pos     = CTX(SAM_POS)->last_value.i;
    bool negative            = negative_ctx->last_value.i; // note: stores index, which conveniently is 1 for the snip "-" and 0 for "+"

    PosType64 sam_pos_delta = this_sam_pos - lookback_sam_pos;         
    PosType64 pos_delta = (sam_pos_delta + atoi (&snip[1])) * (negative ? -1 : 1);

    new_value->i = lookback_pos + pos_delta;
    
    if (reconstruct) RECONSTRUCT_INT (new_value->i);

    return HAS_NEW_VALUE; 
}

static bool sam_seg_TX_AN_cigar (VBlockP vb, ContextP cigar_ctx, STRp(cigar), uint32_t rep)
{
    if (str_issame_(STRa(cigar), STRb(VB_SAM->textual_cigar)))
        seg_by_ctx (vb, (char[]){ SNIP_SPECIAL, SAM_SPECIAL_COPY_TEXTUAL_CIGAR }, 2, cigar_ctx, cigar_len);
    
    else
        sam_seg_other_CIGAR (VB_SAM, cigar_ctx, STRa(cigar), false, cigar_len);

    return true; // segged successfully
}

SPECIAL_RECONSTRUCTOR (sam_piz_special_COPY_TEXTUAL_CIGAR)
{
    if (reconstruct)
        RECONSTRUCT_BUF (VB_SAM->textual_cigar);

    return NO_NEW_VALUE;
}

// eg: TX:Z:hg19_ENST00000456328,+1336,91M;hg19_ENST00000515242,+1329,91M;hg19_ENST00000518655,+1162,91M
//     AN:Z:hg19_ENST00000456328,-1400,91M;hg19_ENST00000515242,-1393,91M;hg19_ENST00000518655,-1226,91M
void sam_seg_TX_AN_Z (VBlockSAMP vb, ZipDataLineSAM *dl, Did did_i, STRp(value), unsigned add_bytes)
{
    START_TIMER;

    static const MediumContainer tx_con = {
        .nitems_lo         = 7, 
        .drop_final_repsep = true,
        .repsep            = {';'},
        .items             = { { .dict_id = { _OPTION_TX_LOOKBACK  }, .separator = { CI0_INVISIBLE, CI1_LOOKBACK } }, 
                               { .dict_id = { _OPTION_TX_NEGATIVE  }, .separator = { CI0_INVISIBLE, CI1_LOOKBACK } }, 
                               { .dict_id = { _OPTION_TX_GENE      }, .separator = {',',            CI1_LOOKBACK } },
                               { .dict_id = { _OPTION_TX_STRAND    }, .separator = { CI0_DIGIT                   } },
                               { .dict_id = { _OPTION_TX_POS       }, .separator = { ',',           CI1_LOOKBACK } },
                               { .dict_id = { _OPTION_TX_CIGAR     },                                              },
                               { .dict_id = { _OPTION_TX_SAM_POS   }, .separator = { CI0_INVISIBLE, CI1_LOOKBACK   } } }
    };

    static const MediumContainer an_con = {
        .nitems_lo         = 7,
        .drop_final_repsep = true,
        .repsep            = {';'},
        .items             = { { .dict_id = { _OPTION_AN_LOOKBACK  }, .separator = { CI0_INVISIBLE, CI1_LOOKBACK } }, 
                               { .dict_id = { _OPTION_AN_NEGATIVE  }, .separator = { CI0_INVISIBLE, CI1_LOOKBACK } }, 
                               { .dict_id = { _OPTION_AN_GENE      }, .separator = {',',            CI1_LOOKBACK } },
                               { .dict_id = { _OPTION_AN_STRAND    }, .separator = { CI0_DIGIT                   } },
                               { .dict_id = { _OPTION_AN_POS       }, .separator = { ',',           CI1_LOOKBACK } },
                               { .dict_id = { _OPTION_AN_CIGAR     },                                              },
                               { .dict_id = { _OPTION_AN_SAM_POS   }, .separator = { CI0_INVISIBLE, CI1_LOOKBACK   } } }
    };

    bool use_lb = segconf.is_sorted && !segconf.running;

    SegCallback callbacks_lb[]    = { seg_do_nothing_cb, seg_do_nothing_cb, sam_seg_TX_AN_gene, 0, sam_seg_TX_AN_pos,          sam_seg_TX_AN_cigar, sam_seg_TX_AN_sam_pos };
    SegCallback callbacks_no_lb[] = { seg_do_nothing_cb, seg_do_nothing_cb, 0,                  0, sam_seg_TX_AN_pos_unsorted, sam_seg_TX_AN_cigar, sam_seg_TX_AN_sam_pos };

    ConstMediumContainerP con = (did_i == OPTION_TX_Z) ? &tx_con : &an_con;

    int32_t repeats = seg_array_of_struct (VB, CTX(did_i), *con, STRa(value),
                                           use_lb ? callbacks_lb : callbacks_no_lb, 
                                           add_bytes);

    // case: we failed to seg as a container - flush lookbacks (rare condition, and complicated to rollback given the round-robin and unlimited repeats)
    if (use_lb && repeats == -1) 
        lookback_flush (VB, con);

    COPY_TIMER(sam_seg_TX_AN_Z);
}
