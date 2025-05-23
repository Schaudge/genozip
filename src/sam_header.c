// ------------------------------------------------------------------
//   sam_header.c
//   Copyright (C) 2020-2025 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

// ----------------------
// SAM / BAM header stuff
// ----------------------

#include <time.h>
#include "sam_private.h"
#include "random_access.h"
#include "zfile.h"
#include "contigs.h"
#include "stats.h"
#include "arch.h"

// globals
ContigPkgP sam_hdr_contigs = NULL; // If no contigs in header: BAM: empty structure (RNAME must be * for all lines); SAM: NULL (RNAME in lines may have contigs) 
HdSoType sam_hd_so = HD_SO_UNKNOWN;
HdGoType sam_hd_go = HD_GO_UNKNOWN;
static Buffer sam_deep_tip = {};

static rom map_sigs[] = SAM_MAPPER_SIGNATURE;

uint32_t sam_num_header_contigs (void)
{
    return sam_hdr_contigs ? sam_hdr_contigs->contigs.len32 : 0;
}

rom sam_get_deep_tip (void)
{
    return sam_deep_tip.len ? B1STc(sam_deep_tip) : NULL;
}

void sam_destroy_deep_tip (void)
{
    buf_destroy (sam_deep_tip);
}

static void sam_header_add_contig (STRp (contig_name), PosType64 LN, void *out_ref_index)
{
    WordIndex ref_index;

    // case: we have a reference, we use the reference chrom_index    
    if (flag.reference & REF_ZIP_LOADED) 
        ref_index = ref_contigs_ref_chrom_from_header_chrom (STRa(contig_name), &LN); // also verifies LN

    else // internal
        ref_index = sam_hdr_contigs->contigs.len32;

    // In case of REF_INTERNAL compression, and we have a header contigs - we calculate their GPOS
    Contig *prev = sam_hdr_contigs->contigs.len ? B(Contig, sam_hdr_contigs->contigs, sam_hdr_contigs->contigs.len-1) : NULL;
    PosType64 gpos = (IS_REF_INTERNAL && prev) ? ROUNDUP64 (prev->gpos + prev->max_pos) : 0; // similar to ref_make_prepare_one_range_for_compress

    // add to contigs. note: index is sam_hdr_contigs is by order of appearance in header, not the same as the reference
    BNXT (Contig, sam_hdr_contigs->contigs) = (Contig){ 
        .min_pos    = 1,
        .max_pos    = LN,        // if last_pos was provided, it is unchanged, and verified = the pos in ref. if not provided, it is what the reference says.
        .char_index = sam_hdr_contigs->dict.len, 
        .snip_len   = contig_name_len,
        .ref_index  = ref_index, // matching reference contig (with possibly a different variation of the contig name)
        .gpos       = gpos
    };

    if (flag.show_txt_contigs) 
        iprintf ("index=%u \"%.*s\" LN=%"PRId64" ref_index=%d snip_len=%u gpos=%"PRId64"\n", 
                 sam_hdr_contigs->contigs.len32-1, STRf(contig_name), LN, ref_index, contig_name_len, gpos);

    // add to contigs_dict
    buf_add_more (NULL, &sam_hdr_contigs->dict, contig_name, contig_name_len, NULL);
    BNXTc (sam_hdr_contigs->dict) = 0; // nul-termiante

    // contigs may contain a semicolon, eg "ScxkALA_1850;HRSCAF=2697" (see: https://hgdownload.soe.ucsc.edu/hubs/GCF/005/870/125/GCF_005870125.1/GCF_005870125.1.chromAlias.txtwcs)
    segconf.sam_semcol_in_contig = segconf.sam_semcol_in_contig || memchr (contig_name, ';', contig_name_len);
}

// call a callback for each SQ line (contig) in the textual header (SAM and BAM). Note: callback function is the same as foreach_contig
static void foreach_textual_SQ_line (rom txt_header, // nul-terminated string if SAM header
                                     uint32_t bam_header_len, // if BAM header: total header len. if SAM header: 0
                                     ContigsIteratorCallback callback, void *callback_param)
{
    rom line = txt_header;
    uint32_t bam_txt_hdr_len = 0;

    // nul-terminate if BAM (per spec, textual header in BAM is not nul-terminated)
    char __save_=0, *__addr_=0;
    if (bam_header_len) {     
        bam_txt_hdr_len = GET_UINT32(txt_header+4);
        if (!bam_txt_hdr_len) return; // no header
        __save_ = txt_header[bam_txt_hdr_len + 8]; // temporary \0 after textual header
        *(__addr_ = (char *)&txt_header[bam_txt_hdr_len + 8]) = 0;

        line += 8;
    }

    for (uint32_t i=0 ; ; i++) {
        if (*line != '@') break;

        char *newline = strchr (line, '\n');
        ASSERT (newline, "line %u of textual SAM header does not end with a newline: \"%s\"", i, line);

        if (line[1] == 'S' && line[2] == 'Q') { // this test will always be in the buffer - possible in the overflow area

            // line looks something like: @SQ\tSN:chr10\tLN:135534747 (no strict order between SN and LN and there could be more parameters)
            rom chrom_name = strstr (line, "SN:"); 
            rom pos_str    = strstr (line, "LN:"); 
            if (chrom_name && pos_str) {
                unsigned chrom_name_len = strcspn (&chrom_name[3], "\t\n\r");

                PosType64 last_pos = (PosType64)strtoull (&pos_str[3], NULL, 10);

                ASSINP (last_pos <= MAX_POS_SAM, "Error: @SQ record in header contains LN:%"PRId64" which is beyond the maximum permitted by the SAM spec of %"PRId64,
                        last_pos, MAX_POS_SAM);

                callback (&chrom_name[3], chrom_name_len, last_pos, callback_param);
            }
        }

        *newline = '\n';    // restore
        line = newline + 1; // beginning of next line or \0
    }

    if (bam_header_len) SAFE_RESTORE;
    
}

// call a callback for each SQ line (contig). Note: callback function is the same as foreach_contig
static void foreach_binary_SQ_line (rom txt_header, uint32_t txt_header_len, // binary BAM header
                                    ContigsIteratorCallback callback, void *callback_param)
{
    #define HDRSKIP(n) if (txt_header_len < next + n) goto incomplete_header; next += n
    #define HDR32 (next + 4 <= txt_header_len ? GET_UINT32 (&txt_header[next]) : 0) ; if (txt_header_len < next + 4) goto incomplete_header; next += 4;

    ASSERT0 (IS_SRC_BAM, "this function can only run on a BAM file");

    uint32_t next=0;

    // skip magic, l_text, text
    HDRSKIP(4);
    uint32_t l_text = HDR32;
    HDRSKIP(l_text);

    uint32_t n_ref = HDR32;

    for (uint32_t ref_i=0; ref_i < n_ref; ref_i++) {
        uint32_t l_name = HDR32; // name length including \0
        rom name = &txt_header[next];
        HDRSKIP (l_name);
        uint32_t l_ref = HDR32;

        callback (name, l_name-1, l_ref, callback_param);
    }

    return;

incomplete_header:
    ASSERT (flag.show_bam, "incomplete BAM header (next=%u)", next); // missing BAM header allowed only if --show-bam

    #undef HDRSKIP
    #undef HDR32
}   

static void sam_header_zip_inspect_HD_line (BufferP txt_header) 
{
    START_TIMER;

    uint32_t hdr_len = IS_BAM_ZIP ? *B32(*txt_header, 1)     : txt_header->len32;
    rom hdr          = IS_BAM_ZIP ? (rom)B32(*txt_header, 2) : txt_header->data;

    if (!str_isprefix_(STRa(hdr), _S("@HD\t"))) return; // no HD
    
    rom nl = memchr (hdr, '\n', hdr_len);
    if (!nl) return; // no newline

    SAFE_NUL(nl);
    rom so = strstr (hdr, "\tSO:");
    
    if (so) {
        static rom hd_so[] = HD_SO_NAMES;
        so += 4;

        for (int i=ARRAY_LEN(hd_so)-1; i >= 0; i--) { // reverse, as its most frequently "coordinate"
            int len = strlen (hd_so[i]);
            if (nl - so >= len && !memcmp (so, hd_so[i], len)) {
                sam_hd_so = i;
                break;
            }
        } 
    }

    rom go = strstr (hdr, "\tGO:");
    
    if (go) {
        static rom hd_go[] = HD_GO_NAMES;
        so += 4;

        for (int i=0; i < ARRAY_LEN(hd_go); i++) {
            int len = strlen (hd_go[i]);
            if (nl - go >= len && !memcmp (go, hd_go[i], len)) {
                sam_hd_go = i;
                break;
            }
        } 
    }

    if (strstr (hdr, map_sigs[MP_CRATAC]))
        segconf.sam_mapper = MP_CRATAC;

    SAFE_RESTORE;

    COPY_TIMER_EVB (sam_header_zip_inspect_HD_line);
}

static void sam_header_zip_build_stats_programs (rom hdr, rom after)
{
    #define EQ3(s1,s2) (s1[0]==s2[0] && s1[1]==s2[1] && s1[2]==s2[2])

    while (hdr < after) {
        str_split_by_tab (hdr, after - hdr, 10, NULL, false, true, false); // also advances hdr to after the newline
        if (!hdr || n_flds < 2 || fld_lens[0] != 3 || !EQ3(flds[0], "@PG")) break;

        int ID_i=-1, PN_i=-1;
        STRl(pg, 128) = 0;
        
        // find ID and/or PN
        for (int i=1; i < n_flds; i++) 
            if (EQ3(flds[i], "ID:")) { // add part before first . and/or -
                ID_i = i;
                rom dot = memchr (flds[i], '.', fld_lens[i]);
                if (dot) fld_lens[i] = dot - flds[i];
                
                rom hyphen = memchr (flds[i], '-', fld_lens[i]);
                if (hyphen) fld_lens[i] = hyphen - flds[i];
            }
            else if (EQ3(flds[i], "PN:")) 
                PN_i = i;
        
        // case: the (possibly truncated) ID is a prefix of PN - keep just ID
        if (PN_i>=0 && ID_i>=0) {
            if (str_issame_(flds[PN_i]+3, MIN_(fld_lens[PN_i],fld_lens[ID_i]) - 3, flds[ID_i]+3, fld_lens[ID_i]-3)) 
                pg_len = snprintf (pg, sizeof(pg), "%.*s", fld_lens[ID_i]-3, flds[ID_i]+3);
            else
                pg_len = snprintf (pg, sizeof(pg), "%.*s %.*s", STRfi(fld,ID_i), STRfi(fld,PN_i));
        }

        else if (PN_i>=0) // only PN exists
            pg_len = snprintf (pg, sizeof(pg), "%.*s", fld_lens[PN_i]-3, flds[PN_i]+3);

        else if (ID_i>=0) // only ID exists
            pg_len = snprintf (pg, sizeof(pg), "%.*s", fld_lens[ID_i]-3, flds[ID_i]+3);

        bool is_dup = false;
        if (stats_programs.len) {
            SAFE_NULB (stats_programs);
            is_dup = strstr (B1STc(stats_programs), pg);
            SAFE_RESTORE;
        }

        if (!is_dup) {
            buf_add_moreS (evb, &stats_programs, pg, "stats_programs");
            BNXTc (stats_programs) = ';'; // buf_add_moreS allocates space for a separator
        }
    }
}

// ZIP and PIZ
static void sam_header_create_deep_tip (rom hdr, rom after)
{
    SAFE_NUL (after);

    STR0(fq);
    sam_deep_tip.name = "sam_deep_tip";

    while (hdr < after && (((fq = strstr (hdr, ".fq.gz")) && (fq_len=6)) || ((fq = strstr (hdr, ".fastq.gz")) && (fq_len=9)))) {
        // find start of word
        while (fq > hdr && (IS_ALPHANUMERIC(fq[-1]) || fq[-1] == '_' || fq[-1] == '-')) // note: we stop also at '/' - record only the base name
            STRdec(fq, 1);

        SAFE_NULT(fq);
        if (!sam_deep_tip.len || !strstr (B1STc(sam_deep_tip), fq)) { // avoid dups
            if (!sam_deep_tip.len) 
                bufprintf (evb, &sam_deep_tip, "Tip: Use --deep to losslessly co-compress BAM and FASTQ, saving about 40%%, compared to compressing FASTQ and BAM separately. E.g.:\n"
                           "%s --reference %s --deep %s", arch_get_argv0(), ref_get_filename() ? ref_get_filename() : "reference-genome.fa.gz", txt_name);

            buf_add_moreC (evb, &sam_deep_tip, " ", NULL);
            buf_append_string (evb, &sam_deep_tip, fq);
        }
        SAFE_RESTORE;
        
        hdr = fq + fq_len;
    }

    if (sam_deep_tip.len) 
        buf_add_moreC (evb, &sam_deep_tip, "\n\0", NULL);

    SAFE_RESTORE;
}

// ZIP
static void sam_header_zip_inspect_PG_lines (BufferP txt_header) 
{
    START_TIMER;

    uint32_t hdr_len = IS_BAM_ZIP ? *B32(*txt_header, 1)     : txt_header->len32;
    rom hdr          = IS_BAM_ZIP ? (rom)B32(*txt_header, 2) : txt_header->data;
    rom after        = hdr + hdr_len;
    
    if (!hdr_len) return; // no header
    
    SAFE_NULT(hdr);

    // advance to point to first @PG line
    #define IS_PG(s) ((s)[1] == 'P' && (s)[2] == 'G' && s[3] == '\t')

    rom s = hdr - 1;
    while ((s = strchr (s+1, '@'))) 
        if (IS_PG(s)) break;

    if (!s) goto done; // header doesn't contain any @PG lines
    rom first_PG = s;

    rom after_PGs = NULL;
    while ((s = strchr (s, '@'))) {
        if (!after_PGs && s[-1]=='\n' && !IS_PG(s)) 
            after_PGs = s;

        else if (after_PGs && s[-1]=='\n' && IS_PG(s)) // oops, found another PG after bunch of non-PGs... cancel previous "after_PGs"
            after_PGs = NULL;

        s++;
    }

    if (!after_PGs) after_PGs = after;

    RESAFE_NUL (after_PGs); // move the NUL up to after all PG lines (even if PG lines are non-sequential)

    ASSERT0 (ARRAY_LEN(map_sigs) == NUM_MAPPERS, "Invalid SAM_MAPPER_SIGNATURE array length - perhaps missing commas between strings?");

    for (int i=1; i < ARRAY_LEN(map_sigs); i++) // skip 0=unknown
        if (strstr (first_PG, map_sigs[i]) &&   // scans subset of header containing all PG lines
            (!segconf.sam_mapper || strlen (map_sigs[i]) > strlen(map_sigs[segconf.sam_mapper]))) { // first match or better match (eg "bwa-mem2" is a better match than "bwa")
            segconf.sam_mapper = i;
            break;
        }

    if (MP(STAR) && strstr (first_PG, "--solo")) segconf.star_solo = true;

    // cisCall uses bwa mem: https://www.ciscall.org/en/ciscall7.html
    if (MP(UNKNOWN) && strstr (first_PG, "cisCall")) segconf.sam_mapper = MP_BWA;

    // DRAGEN optionally rewrites base quality scores of 'N' to a fixed value - this affects Deep
    if (MP(DRAGEN) && flag.deep) {
        rom fastq_n_quality = strstr (first_PG, "fastq-n-quality: "); // minimum Phread quality of 'N' bases, default 2. see: https://support.illumina.com/content/dam/illumina-support/help/Illumina_DRAGEN_Bio_IT_Platform_v3_7_1000000141465/Content/SW/Informatics/Dragen/SoftwareInpNBase_fDG.htm
        rom fastq_offset    = strstr (first_PG, "fastq-offset: ");    // offset of byte value vs phred. default 33.
        int fastq_n_quality_value = fastq_n_quality ? atoi (fastq_n_quality + STRLEN("fastq-n-quality: ")) : 0;
        int fastq_offset_value    = fastq_offset    ? atoi (fastq_offset + STRLEN("fastq-offset: ")) : 0;
        if (fastq_n_quality_value && fastq_offset_value) {
            segconf.deep_N_sam_score = fastq_n_quality_value + fastq_offset_value;

            if (flag_show_deep) iprintf ("deep_N_sam_score='%c'(%u)\n", segconf.deep_N_sam_score, segconf.deep_N_sam_score);
        }
    }

    if (strstr (first_PG, "ABRA2")) segconf.sam_has_abra2 = true;

    // build buffer of unique PN+ID fields, for stats
    sam_header_zip_build_stats_programs (first_PG, after_PGs);

    sam_segconf_set_by_MP();
    
done:
    SAFE_RESTORE;

    // insect entire header, not just PG lines (after nul after all PGs has been restored)
    
    sam_header_create_deep_tip (hdr, after); 

    COPY_TIMER_EVB (sam_header_zip_inspect_PG_lines); 
}

static void sam_header_zip_inspect_RG_lines (BufferP txt_header) 
{
    START_TIMER;

    uint32_t hdr_len = IS_BAM_ZIP ? *B32(*txt_header, 1)     : txt_header->len32;
    rom hdr          = IS_BAM_ZIP ? (rom)B32(*txt_header, 2) : txt_header->data;
    rom after        = hdr + hdr_len;
    
    if (!hdr_len) return; // no header
    
    SAFE_NULT(hdr);

    // advance to point to first @RG line
    #define IS_RG(s) (s[1] == 'R' && s[2] == 'G' && s[3] == '\t')
    #define IS_ID(s) (s[0] == 'I' && s[1] == 'D' && s[2] == ':')
    #define IS_PL(s) (s[0] == 'P' && s[1] == 'L' && s[2] == ':')

    while ((hdr = strchr (hdr, '@'))) 
        if (IS_RG(hdr)) {
            str_split_by_tab (hdr, after - hdr, 32, NULL, false, true, false); // also advances hdr to after the newline
        
            for (int i=1; i < n_flds; i++)
                if (IS_ID (flds[i])) 
                    ctx_populate_zf_ctx (OPTION_RG_Z, flds[i]+3, fld_lens[i]-3, WORD_INDEX_NONE);

                else if (!segconf.tech_by_RG && IS_PL (flds[i])) {
                    SAFE_NUL(&flds[i][fld_lens[i]]);

                    static struct { rom name/*case insensitive*/; SeqTech tech; } PL_to_tech[] = {
                        // standard                       // observed non-standard
                        { "Illumina",   TECH_ILLUM     }, 
                        { "DNBSEQ",     TECH_MGI       }, { "mgi", TECH_MGI },
                        { "PacBio",     TECH_PACBIO    }, 
                        { "Ultima",     TECH_ULTIMA    }, 
                        { "ONT",        TECH_NANOPORE  }, 
                        { "IonTorrent", TECH_IONTORR   }, 
                        { "Element",    TECH_ELEMENT   }, 
                        { "Singular",   TECH_SINGLR    }, 
                        { "Capillary",  TECH_CAPILLARY }, 
                        { "Helicos",    TECH_HELICOS   },  
                        { "LS454",      TECH_LS454       }, 
                        { "SOLiD",      TECH_SOLID     }, 
                    };
                    
                    for (int pl=0; pl < ARRAY_LEN(PL_to_tech); pl++) 
                        if (str_case_compare (flds[i]+3, PL_to_tech[pl].name, NULL)) {
                            segconf.tech_by_RG = PL_to_tech[pl].tech; // note: this is very unreliable, our segconf.tech is a much better predictor of the true sequencing technology. In many BAM files, this is missing, contains non-standard values, and is sometimes simply false (e.g. old Ultima files have LS454)
                            break;
                        }

                    if (!segconf.tech_by_RG)
                        strncpy (segconf.tech_by_RG_unidentified, flds[i]+3, sizeof(segconf.tech_by_RG_unidentified)-1);
                    
                    SAFE_RESTORE;
                }           
        }
        else hdr++;

    SAFE_RESTORE;

    segconf.sam_multi_RG = ZCTX(OPTION_RG_Z)->nodes.len32 >= 2;

    COPY_TIMER_EVB (sam_header_zip_inspect_RG_lines); 
}

typedef struct { uint32_t n_contigs, dict_len; } ContigsCbParam;
static void sam_header_count_contigs_cb (STRp(chrom_name), PosType64 last_pos, void *cb_param)
{
    // note: for simplicity, we consider contigs to be in the range [0, last_pos] even though POS=0 doesn't exist - last_pos+1 loci
    ((ContigsCbParam*)cb_param)->n_contigs++;
    ((ContigsCbParam*)cb_param)->dict_len += chrom_name_len + 1;
}

// ZIP
static void sam_header_alloc_contigs (BufferP txt_header)
{
    // count contigs in textual header
    ContigsCbParam txt_ctg_count={}, bin_ctg_count={}; 
    foreach_textual_SQ_line (txt_header->data, IS_SRC_BAM ? txt_header->len : 0, sam_header_count_contigs_cb, &txt_ctg_count);

    // count contigs in binary header
    if (IS_SRC_BAM && txt_file->type != CRAM) // in case of CRAM we don't have binary contigs (unless header was read through samtools - but even then, the CRAM itself has only a textual header)
        foreach_binary_SQ_line  (STRb(*txt_header), sam_header_count_contigs_cb, &bin_ctg_count);
    
    // we expect textual and binary files to be identical, however we observed in the wild files where the binary contigs
    // are a subset of the textual contigs (apartently generated by STAR: https://www.encodeproject.org/files/ENCFF047UEJ/@@download/ENCFF047UEJ.bam)
    // we take the larger set and discard the smaller one
    int32_t delta = (int32_t)txt_ctg_count.n_contigs - (int32_t)bin_ctg_count.n_contigs;

    // warning if different number of contigs between textual and binary contig list, unless one of them is empty
    ASSERTW (!delta || !txt_ctg_count.n_contigs || !bin_ctg_count.n_contigs, 
             "FYI: %s is not compliant with the SAM specification: Inconsistent number of contigs between the textual version of the SAM header (%u contigs) and the binary version (%u contigs). Genozip is using the larger of the two (i.e. the %s one).",
             txt_file->basename, txt_ctg_count.n_contigs, bin_ctg_count.n_contigs, delta >= 0 ? "textual" : "binary");

    // sanity check - if we have the same number of contigs, dict_len should be the same too
    ASSERT (delta || txt_ctg_count.dict_len == bin_ctg_count.dict_len, 
            "Inconsistent contigs between the textual version of the SAM header (dict_len=%u) and the binary version (dict_len=%u)",
            txt_ctg_count.dict_len, bin_ctg_count.dict_len);

    ContigsCbParam *winner = delta >= 0 ? &txt_ctg_count : &bin_ctg_count;

    if (winner->n_contigs) {
        sam_hdr_contigs = IS_REF_INTERNAL ? ref_get_ctgs() : CALLOC (sizeof (ContigPkg));
        sam_hdr_contigs->name = "sam_hdr_contigs";        
        sam_hdr_contigs->contigs.param = (delta > 0); // 1 if contigs come from the textual header, 0 if binary
        
        buf_alloc (evb, &sam_hdr_contigs->contigs, 0, winner->n_contigs, Contig, 1, "sam_hdr_contigs->contigs"); 
        buf_alloc (evb, &sam_hdr_contigs->dict,    0, winner->dict_len,  char,   1, "sam_hdr_contigs->dict"); 
    }
}

static void sam_header_zip_inspect_SQ_lines (VBlockP txt_header_vb, BufferP txt_header) 
{
    START_TIMER;
    
    sam_header_alloc_contigs (txt_header); 
    if (sam_hdr_contigs) {
        START_TIMER;

        if (sam_hdr_contigs->contigs.param) // contigs originate from textual header
            foreach_textual_SQ_line (txt_header->data, IS_SRC_BAM ? txt_header->len32 : 0, sam_header_add_contig, NULL);
        else
            foreach_binary_SQ_line  (STRb(*txt_header), sam_header_add_contig, NULL);

        COPY_TIMER_EVB (sam_header_add_contig);

        if (IS_ZIP)            
            contigs_create_index (sam_hdr_contigs, SORT_BY_NAME); // used by sam_sa_add_sa_group
    }

    COPY_TIMER_EVB (sam_header_zip_inspect_SQ_lines);
}

// called from cram_read_sam_header to verify that all CRAM header contigs appear in reference, 
// because otherwise samtools will hang 
static void sam_header_verify_same_contig_as_ref (STRp (contig_name), PosType64 LN, void *cram_filename)
{
    ref_contigs_verify_same_contig_as_ref (cram_filename, STRa(contig_name), LN); 
}

void sam_header_zip_inspect_SQ_lines_in_cram (rom cram_filename)
{
    foreach_textual_SQ_line (B1STc(evb->txt_data), 0, sam_header_verify_same_contig_as_ref, (void *)cram_filename);
}

void sam_header_finalize (void)
{    
    if (!IS_REF_INTERNAL) {
        contigs_destroy (sam_hdr_contigs); // destroy bc we need to remove the buffers from the buffer_list ahead of FREEing the memory
        FREE (sam_hdr_contigs);
    }
    else
        sam_hdr_contigs = NULL; // memory will be freed when we destroy the reference

    sam_hd_so = HD_SO_UNKNOWN;
    sam_hd_go = HD_GO_UNKNOWN;
}

// ZIP
bool sam_header_has_string (ConstBufferP txt_header, rom substr)
{
    uint32_t next=0;
    #define HDRSKIP(n) if (txt_header->len < next + n) return false; next += n
    #define HDR32 (next + 4 <= txt_header->len ? GET_UINT32 (&txt_header->data[next]) : 0) ; if (txt_header->len < next + 4) return false; next += 4;

    if (IS_BAM_ZIP) {
        HDRSKIP(4); // magic
        uint32_t l_text = HDR32;
        rom text = Bc (*txt_header, next);
        SAFE_NUL (&text[l_text]);
        bool found = !!strstr (text, substr);
        SAFE_RESTORE;
        return found;
    }
    else  // SAM - already nul-terminated in sam_header_inspect
        return !!strstr (txt_header->data, substr);

    #undef HDRSKIP
    #undef HDR32
}

// constructs hdr_contigs, and in ZIP, also initialzes refererence ranges and random_access
bool sam_header_inspect (VBlockP txt_header_vb, BufferP txt_header, struct FlagsTxtHeader txt_header_flags)
{    
    START_TIMER;

    // if there is no external reference provided, then we create our internal one, and store it
    // (if an external reference IS provided, the user can decide whether to store it or not, with --reference vs --REFERENCE)
    if (IS_ZIP && flag.reference == REF_NONE) flag.reference = REF_INTERNAL;

    if (flag.show_txt_contigs && is_genocat) exit_ok;

    // get contigs from @SQ lines
    if (IS_ZIP || flag.collect_coverage)  // note: up to v13, contigs were carried by SEC_REF_CONTIG for REF_INTERNAL too 
        sam_header_zip_inspect_SQ_lines (txt_header_vb, txt_header);

    if (IS_ZIP && !flag.bam_assist) {

        if (!IS_BAM_ZIP) *BAFTc (*txt_header) = 0; // nul-terminate as required by foreach_textual_SQ_line

        sam_header_zip_inspect_HD_line  (txt_header);
        sam_header_zip_inspect_PG_lines (txt_header);
        sam_header_zip_inspect_RG_lines (txt_header);

        // in case of internal reference, we need to initialize. in case of --reference, it was initialized by ref_load_external_reference()
        if (IS_REF_INTERNAL && sam_hdr_contigs) 
            ref_initialize_ranges (RT_DENOVO); 

        // evb buffers must be alloced by the main thread, since other threads cannot modify evb's buf_list
        random_access_alloc_ra_buf (evb, SAM_RNAME);
    }

    else if (IS_PIZ) {
        // deep tip - create in case tip is displayed in "--test after ZIP"
        uint32_t hdr_len = IS_SRC_BAM_PIZ ? *B32(*txt_header, 1)     : txt_header->len32;
        rom hdr          = IS_SRC_BAM_PIZ ? (rom)B32(*txt_header, 2) : txt_header->data;

        sam_header_create_deep_tip (hdr, hdr + hdr_len);
    }

    COPY_TIMER_EVB (sam_header_inspect);
    return true;
}

// ZIP: called from txtfile_read_header
// returns header length if header read is complete + sets lines.len, -1 not complete yet 
// note: usually a BAM header fits into a single 512KB READ BUFFER, so this function is called only twice (without and then with data).
// callback from DataTypeProperties.is_header_done
int32_t bam_is_header_done (bool is_eof)
{
    #define HDRSKIP(n) if (evb->txt_data.len < next + n) goto incomplete_header; next += n
    #define HDR32 (next + 4 <= evb->txt_data.len ? GET_UINT32 (&evb->txt_data.data[next]) : 0) ; if (evb->txt_data.len < next + 4) goto incomplete_header; next += 4;

    uint32_t next=0;

    HDRSKIP(4); // magic
    bool is_magical = !memcmp (evb->txt_data.data, BAM_MAGIC, 4);
    if (flag.show_bam && !is_magical) return 0; // BAM file has no header - allowed only with --show-bam

    if (!is_magical) {
        // check - perhaps is actually SAM (easy to accidentally get samtools to produce SAM instead of BAM)
        if (evb->txt_data.len32 < 64 KB && !is_eof)
            return HEADER_NEED_MORE; // we need a bit of data to tell if its SAM

        if (is_sam (STRb(evb->txt_data), NULL)) {
            txt_file->data_type = z_file->data_type = DT_SAM;
            txt_file->src_codec = txt_file->effective_codec; // not CODEC_BAM anymore
            z_file->z_flags.txt_is_bin = false;
            return HEADER_DATA_TYPE_CHANGED;
        }
        else
            ASSINP (false, "%s doesn't have a BAM magic - it doesn't seem to be a BAM file: magic=%2.2x%2.2x%2.2x%2.2x ", txt_name, (uint8_t)evb->txt_data.data[0], (uint8_t)evb->txt_data.data[1], (uint8_t)evb->txt_data.data[2], (uint8_t)evb->txt_data.data[3]);
    }

    // sam header text
    uint32_t l_text = HDR32;
    rom text = Bc (evb->txt_data, next);
    HDRSKIP(l_text);

    uint32_t header_n_ref = HDR32;

    for (uint32_t ref_i=0; ref_i < header_n_ref; ref_i++) {
        uint32_t l_name = HDR32;
        HDRSKIP (l_name + 4); // skip name and l_ref
    }

    evb->lines.len = str_count_char (text, l_text, '\n'); // number of text lines in the SAM header

    return next; // return BAM header length

incomplete_header:
    return HEADER_NEED_MORE;

    #undef HDRSKIP
    #undef HDR32
}   

//------------------------------------------------------------------
// Translator functions for reconstructing SAM<-->BAM header formats
//------------------------------------------------------------------

// add @PG line
static inline void sam_header_add_PG (BufferP txtheader_buf)
{
    if (flag.no_pg) return; // user specified --no-PG

    // the command line length is unbound, careful not to put it in a bufprintf
    bufprintf (txtheader_buf->vb, txtheader_buf, "@PG\tID:genozip-%u\tPN:genozip\tDS:%s\tVN:%s\tCL:", 
               getpid(), GENOZIP_URL, code_version().s);
    buf_append_string (txtheader_buf->vb, txtheader_buf, flags_command_line());
    buf_append_string (txtheader_buf->vb, txtheader_buf, "\n");
}

// PIZ main thread: make the txt header either SAM or BAM according to flag.out_dt, and regardless of the source file
TXTHEADER_TRANSLATOR (sam_header_bam2sam)
{
    if (flag.no_header || !txtheader_buf->len /* SA component */) {
        txtheader_buf->len = 0; // remove the BAM header data
        return;
    }

    ASSERT0 (buf_is_alloc (txtheader_buf), "txtheader_buf not allocated");

    uint32_t l_text = GET_UINT32 (Bc (*txtheader_buf, 4));
    memmove (txtheader_buf->data, Bc (*txtheader_buf, 8), l_text);
    txtheader_buf->len = l_text;
    
    sam_header_add_PG (txtheader_buf);
}

static void sam_header_sam2bam_count_sq (rom chrom_name, unsigned chrom_name_len, PosType64 last_pos, void *callback_param)
{
    (*(uint32_t *)callback_param)++;
}

static void sam_header_sam2bam_ref_info (STRp (ref_contig_name), PosType64 last_pos, void *callback_param)
{
    BufferP txtheader_buf = (BufferP )callback_param;

    buf_alloc (txtheader_buf->vb, txtheader_buf, ref_contig_name_len+9, 0, char, 1, 0);

    // l_name
    ref_contig_name_len++; // inc. nul terminator
    *(uint32_t *)BAFTc (*txtheader_buf) = LTEN32 (ref_contig_name_len); 
    txtheader_buf->len += sizeof (uint32_t);

    ASSINP (ref_contig_name_len <= INT32_MAX, "Error: cannot convert to BAM because l_name=%u exceeds BAM format maximum of %u", ref_contig_name_len, INT32_MAX);

    // name
    buf_add (txtheader_buf, ref_contig_name, ref_contig_name_len-1);                  
    BNXTc (*txtheader_buf) = 0;

    // l_ref
    uint32_t last_pos32 = (uint32_t)last_pos;
    *(uint32_t *)BAFTc (*txtheader_buf) = LTEN32 (last_pos32); 
    txtheader_buf->len += sizeof (uint32_t);

    ASSINP (last_pos <= INT32_MAX, "Error: cannot convert to BAM because contig %.*s has length=%"PRId64" that exceeds BAM format maximum of %u", 
            ref_contig_name_len-1, ref_contig_name, last_pos, INT32_MAX);
}

// prepare BAM header from SAM header, according to https://samtools.github.io/hts-specs/SAMv1.pdf section 4.2
TXTHEADER_TRANSLATOR (sam_header_sam2bam)
{
    // grow buffer to accommodate the BAM header fixed size and text (inc. nul terminator)
    buf_alloc (comp_vb, txtheader_buf, 12 + 1, 0, char, 1, "txt_data");

    // nul-terminate text - required by foreach_textual_SQ_line - but without enlengthening buffer
    *BAFTc (*txtheader_buf) = 0;

    // n_ref = count SQ lines in text
    uint32_t n_ref=0;
    foreach_textual_SQ_line (txtheader_buf->data, 0, sam_header_sam2bam_count_sq, &n_ref);

    // we can't convert to BAM if its a SAM file with aligned reads but without SQ records, compressed with REF_INTERNAL - as using the REF_INTERNAL
    // contigs would produce lengths that don't match actual reference files - rendering the BAM file useless for downstream
    // analysis. Better give an error here than create confusion downstream. 
    ASSINP (n_ref ||  // has SQ records
            !z_file->z_flags.dts_ref_internal || // has external reference
            ZCTX(SAM_POS)->word_list.len==1, // has only one POS word = "Delta 0" = unaligned SAM that doesn't need contigs 
            "Failed to convert %s from SAM to BAM: the SAM header must have SQ records (see https://samtools.github.io/hts-specs/SAMv1.pdf section 1.3), or alternatively the file should be genozipped with --reference or --REFERENCE", z_name);

    // if no SQ lines - get lines from loaded contig (will be available only if file was compressed with --reference or --REFERENCE)
    bool from_SQ = !!n_ref;
    if (!from_SQ) n_ref = ref_num_contigs();

    // grow buffer to accommodate estimated reference size (we will more in sam_header_sam2bam_ref_info if not enough)
    buf_alloc (comp_vb, txtheader_buf, n_ref * 100 + (50 + strlen (flags_command_line())), 0, char, 1, 0);

    // add PG
    sam_header_add_PG (txtheader_buf);

    // construct magic, l_text, text and n_ref fields of BAM header
    char *text = txtheader_buf->data + 8;
    uint32_t l_text = txtheader_buf->len;
    ASSINP (txtheader_buf->len <= INT32_MAX, "Cannot convert to BAM because SAM header length (%"PRIu64" bytes) exceeds BAM format maximum of %u", txtheader_buf->len, INT32_MAX);

    memmove (text, txtheader_buf->data, l_text);    // text
    memcpy (B1STc (*txtheader_buf), BAM_MAGIC, 4);  // magic
    *B32 (*txtheader_buf, 1) = LTEN32 (l_text);     // l_text
    *(uint32_t *)Bc (*txtheader_buf, l_text + 8) = LTEN32 (n_ref); // n_ref
    txtheader_buf->len += 12; // BAM header length so far, before adding reference information

    // option 1: copy reference information from SAM SQ lines, if available
    if (from_SQ) 
        foreach_textual_SQ_line (STRb(*txtheader_buf), sam_header_sam2bam_ref_info, txtheader_buf);

    // option 2: copy reference information from ref_contigs if available - i.e. if pizzed with external or stored reference
    else if (ref_num_contigs()) 
        foreach_contig (ref_get_ctgs(), sam_header_sam2bam_ref_info, txtheader_buf);
}
