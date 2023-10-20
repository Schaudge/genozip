// ------------------------------------------------------------------
//   stats.c
//   Copyright (C) 2019-2023 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#include <stdarg.h>
#include <errno.h>
#include "genozip.h"
#include "buffer.h"
#include "strings.h"
#include "stats.h"
#include "sections.h"
#include "file.h"
#include "vblock.h"
#include "txtheader.h"
#include "reference.h"
#include "zfile.h"
#include "version.h"
#include "arch.h"
#include "codec.h"
#include "license.h"
#include "segconf.h"
#include "qname.h"
#include "gencomp.h"
#include "url.h"
#include "crypt.h"
#include "tar.h"
#include "generic.h"
#include "reference.h"
#include "contigs.h"

static Buffer stats={}, STATS={};
Buffer sbl_buf={}, features={}, hash_occ={}, internals={};
Buffer stats_programs = {}; // data-type specific programs (eg @PG for SAM/BAM FORMAT/INFO tags for VCF)
int64_t all_txt_len=0;
float src_comp_ratio=0, all_comp_ratio=0;

void stats_add_one_program (STRp(prog_name))
{
    stats_programs.name = "stats_programs"; // initialize name (note: destroyed between files, so needs to be reinitialized)

    buf_append (evb, stats_programs, char, prog_name, prog_name_len, "stats_programs");

    BNXTc (stats_programs) = ';'; // note: buf_append allocates one extra character
}

bool stats_is_in_programs (rom signature)
{
    if (!stats_programs.len) return false;

    SAFE_NULB(stats_programs);
    bool found = strstr (B1STc(stats_programs), signature);
    SAFE_RESTORE;

    return found;
}

// calculate hash_occ before consolidating stats
static void stats_calc_hash_occ (StatsByLine *sbl, unsigned num_stats)
{
    int need_sep=0;

    for (StatsByLine *s=sbl, *after=sbl + num_stats; s < after; s++) {
        ContextP zctx = (s->my_did_i != DID_NONE) ? ZCTX(s->my_did_i) : NULL;

        if (s->pc_hash_occupancy > 75 || // > 75%
            (zctx && zctx->nodes.len > 1000000 && z_file->num_lines / zctx->nodes.len < 16)) { // more than 10% of num_lines (and at least 1M)
             
            // in case of an over-populated hash table, we send the first 3 and last 3 words in the dictionary, which will help debugging the issue
            bufprintf (evb, &hash_occ, "%s%s%%2C%s%%2C%s%%2C%u%%25", 
                       need_sep++ ? "%3B" : "", url_esc_non_valid_charsS(s->name).s, url_esc_non_valid_charsS(s->type).s, 
                       url_esc_non_valid_charsS (str_size (s->global_hash_prime).s).s, (int)s->pc_hash_occupancy);
            
            uint32_t n_words = zctx->nodes.len32; // note: this can be a low number despite pc_hash_occupancy being large - if words ended up as singletons
            WordIndex words[NUM_COLLECTED_WORDS] = { 0, 1, 2, n_words-3, n_words-2, n_words-1 }; // first three and last threewords in the the dictionary of this field
            for (int i=0; i < MIN_(NUM_COLLECTED_WORDS, n_words); i++)
                // note: str_replace_letter modifies dict data, but its ok, since we have already written the dicts to z_file
                bufprintf (evb, &hash_occ, "%%2C%s", url_esc_non_valid_charsS (str_replace_letter ((char *)ctx_snip_from_zf_nodes (zctx, words[i], 0, 0), sizeof(UrlStr), ',', -127)).s); 
        }
    }

    // we send the first 6 qnames in case of unrecognized QNAME flavor or in case of existing by unrecognized QNAME2 flavor
    for (QType q=QNAME1; q < NUM_QTYPES; q++) {
        if (!segconf.qname_flavor[q] && segconf.n_unk_flav_qnames[q]) {

            bufprintf (evb, &hash_occ, "%s%s%%2C%%2C%%2C", need_sep++ ? "%3B" : "", qtype_name(q));

            for (int i=0; i < segconf.n_unk_flav_qnames[q]; i++)
                if (segconf.unk_flav_qnames[q][i][0])
                    bufprintf (evb, &hash_occ, "%%2C%s", url_esc_non_valid_charsS (str_replace_letter (segconf.unk_flav_qnames[q][i], strlen(segconf.unk_flav_qnames[q][i]), ',', -127)).s); 
        }
    }

    // if Deep with QNONE, we send the first QNAME of the SAM file and the first read name of the FASTQ
    if (flag.deep && segconf.deep_qtype == QNONE) {
        bufprintf (evb, &hash_occ, "%sQNONE_REASON%%2C%%2C%%2C", need_sep++ ? "%3B" : "");
        bufprintf (evb, &hash_occ, "%%2C%s", url_esc_non_valid_charsS (str_replace_letter (segconf.deep_1st_qname, strlen(segconf.deep_1st_qname), ',', -127)).s); 
        bufprintf (evb, &hash_occ, "%%2C%s", url_esc_non_valid_charsS (str_replace_letter (segconf.deep_1st_desc,  strlen(segconf.deep_1st_desc),  ',', -127)).s); 
    }
}

// stats of contexts and sections contribution to Z
static void stats_prepare_internals (StatsByLine *sbl, unsigned num_stats, uint64_t all_z_size)
{
    if (!all_z_size) return;
    
    int need_sep = 0;

    for (StatsByLine *after=sbl + num_stats; sbl < after; sbl++) 
        if (sbl->z_size) {
            bufprintf (evb, &internals, "%s%s%%2C%s%%2C%.1f%%25%%2C", 
                       need_sep++ ? "%3B" : "", url_esc_non_valid_charsS(sbl->name).s, url_esc_non_valid_charsS(sbl->type).s, sbl->pc_of_z);

            if (sbl->my_did_i != DID_NONE) {
                ContextP zctx = ZCTX(sbl->my_did_i);

                bufprintf (evb, &internals, "%s%%2C%.1f%%25%%2C",
                           url_esc_non_valid_charsS(sbl->lcodec).s, 100.0 * (double)zctx->local.count / (double)all_z_size);  // local

                bufprintf (evb, &internals, "%s%%2C%.1f%%25%%2C",
                           url_esc_non_valid_charsS(sbl->bcodec).s, 100.0 * (double)zctx->b250.count  / (double)all_z_size);  // b250

                bufprintf (evb, &internals, "%s%%2C%.1f%%25%%2C%u%%2C",
                           url_esc_non_valid_charsS(sbl->dcodec).s, 100.0 * (double)zctx->dict.count  / (double)all_z_size, zctx->nodes.len32); // dict
            }
        }
}

// store the sizes of dict / b250 / local in zctx->*.param, and of other sections in sbl[st].z_size
static void stats_get_compressed_sizes (StatsByLine *sbl) 
{
    ContextIndex ctx_index[MAX_DICTS];

    // initialize & prepare context index
    for (Did did_i=0; did_i < z_file->num_contexts; did_i++) {
        ZCTX(did_i)->b250.len = ZCTX(did_i)->b250.count; // number of b250 words - move to len
        ZCTX(did_i)->b250.count = ZCTX(did_i)->dict.count = ZCTX(did_i)->local.count = 0; 
        ctx_index[did_i] = (ContextIndex){ .did_i = did_i, .dict_id = ZCTX(did_i)->dict_id };
    }
    
    qsort (ctx_index, z_file->num_contexts, sizeof (ContextIndex), sort_by_dict_id);

    for (Section sec = section_next(0); sec; sec = section_next (sec)) {

        if (flag.show_stats_comp_i == COMP_NONE && sec->st != SEC_B250 && sec->st != SEC_DICT && sec->st != SEC_LOCAL && sec->st != SEC_COUNTS)
            sbl[sec->st].z_size += sec->size;
            
        else if (flag.show_stats_comp_i != COMP_NONE && flag.show_stats_comp_i == sec->comp_i && 
            (sec->st==SEC_VB_HEADER || sec->st==SEC_TXT_HEADER || sec->st==SEC_RECON_PLAN))
            sbl[sec->st].z_size += sec->size;
            
        else if ((flag.show_stats_comp_i == COMP_NONE || flag.show_stats_comp_i == sec->comp_i) && 
                 (sec->st == SEC_B250 || sec->st == SEC_DICT || sec->st == SEC_LOCAL || sec->st == SEC_COUNTS)) {
            Did did_i = ctx_get_existing_did_i_do (sec->dict_id, z_file->contexts, z_file->d2d_map,
                                                        ctx_index, z_file->num_contexts);

            // accumulate z_size for its context in its local/b250/dict.param
            switch (sec->st) {
                case SEC_LOCAL  : ZCTX(did_i)->local.count += sec->size; break;
                case SEC_B250   : ZCTX(did_i)->b250.count  += sec->size; break;
                case SEC_COUNTS :
                case SEC_DICT   : ZCTX(did_i)->dict.count  += sec->size; break;
                default         : break;
            }
        }
    }

    // update allocations of compressed sizes between contexts
    if (DTPZ(stats_reallocate)) DTPZ(stats_reallocate)();
}

static void stats_output_file_metadata (void)
{
    #define FEATURE0(cond,stats_str,features_str)                       \
    if (cond) {                                                         \
        bufprint0 (evb, &stats, stats_str "\n");                        \
        bufprint0 (evb, &features, features_str ";");                   \
    }

    #define FEATURE(cond,stats_format,features_format,...)              \
    if (cond) {                                                         \
        bufprintf (evb, &stats, stats_format "\n", __VA_ARGS__);        \
        bufprintf (evb, &features, features_format ";", __VA_ARGS__);   \
    }

    bufprintf (evb, &features, "VBs=%u X %s;", z_file->num_vbs, str_size (segconf.vb_size).s);
    if (!Z_DT(GENERIC) && !flag.make_reference) {
        bufprintf (evb, &features, "num_lines=%"PRIu64";", z_file->num_lines);

        // note regarding DVCF: reports lines of reject components, which are duplicates of MAIN component lines
        if (z_file->num_components > 1) {
            bufprint0 (evb, &features, "comp_num_lines=");
            for (CompIType comp_i=0; comp_i < z_file->num_components; comp_i++)
                bufprintf (evb, &features, "%"PRId64"+", z_file->comp_num_lines[comp_i]); 

            features.len32 -= 1; // remove final "+"
            bufprint0 (evb, &features, ";");
        }
    }
        
    bufprint0 (evb, &stats, "\n\n");
    if (txt_file->name) 
        bufprintf (evb, &stats, "%s file%s%s: %.*s\n", flag.deep ? "BAM/FASTQ" : z_dt_name(),
                   z_file->bound_txt_names.count > 1 ? "s" : "", // param holds the number of txt files
                   (flag.deep && flag.pair)?" (deep & paired)" : flag.deep?" (deep)" : flag.pair?" (paired)" : "", 
                   (int)z_file->bound_txt_names.len, z_file->bound_txt_names.data);
    
    if (IS_REF_MAKE_CHAIN) {
        bufprintf (evb, &stats, "PRIM reference: %s %s=%s ref_genozip_version=%u\n", 
                   ref_get_filename (prim_ref), ref_get_digest_name (prim_ref), 
                   digest_display_(ref_get_genome_digest (prim_ref), ref_get_genozip_version (prim_ref)).s, 
                   ref_get_genozip_version (prim_ref));

        bufprintf (evb, &stats, "LUFT reference: %s %s=%s ref_genozip_version=%u\n", 
                   ref_get_filename (gref), ref_get_digest_name (gref), 
                   digest_display_(ref_get_genome_digest (gref), ref_get_genozip_version (gref)).s, 
                   ref_get_genozip_version (gref));
    }

    else if (IS_REF_EXTERNAL || IS_REF_EXT_STORE || IS_REF_LIFTOVER) 
        bufprintf (evb, &stats, "Reference: %s %s=%s ref_genozip_version=%u\n", 
                   ref_get_filename (gref),  ref_get_digest_name (gref), 
                   digest_display_(ref_get_genome_digest (gref), ref_is_digest_adler (gref)).s, 
                   ref_get_genozip_version (gref));

    uint32_t num_used_ctxs=0;
    for_zctx_that (zctx->nodes.len || zctx->txt_len) num_used_ctxs++;

    #define REPORT_VBs ({ \
        bufprintf (evb, &stats, "%ss: %s   Contexts: %u   Vblocks: %u x %s   Sections: %u\n",  \
                   DTPZ (line_name), str_int_commas (z_file->num_lines).s, num_used_ctxs, \
                   z_file->num_vbs, str_size (segconf.vb_size).s, z_file->section_list_buf.len32); })

    #define REPORT_QNAME \
        FEATURE (z_file->num_lines, "Read name style: %s%s", "Qname=%s%s", \
                 segconf_qf_name (0), cond_str(segconf.qname_flavor[1], " + ", segconf_qf_name(1)))

    #define REPORT_KRAKEN \
        FEATURE0 (kraken_is_loaded, "Features: Per-line taxonomy ID data", "taxonomy_data")

    switch (z_file->data_type) {

        case DT_SAM:
        case DT_BAM: {
            uint64_t num_alignments = z_file->comp_num_lines[SAM_COMP_MAIN] + z_file->comp_num_lines[SAM_COMP_PRIM] + z_file->comp_num_lines[SAM_COMP_DEPN]; // excluding Deep FQ components
            unsigned num_fq_files = MAX_(0, (int)z_file->num_components - SAM_COMP_FQ00); 
            double deep_pc = z_file->deep_stats[NDP_FQ_READS] ? (double)100.0 * (double)(z_file->deep_stats[NDP_DEEPABLE] + z_file->deep_stats[NDP_DEEPABLE_TRIM]) / (double)z_file->deep_stats[NDP_FQ_READS] : 0;

            if (z_has_gencomp) 
                bufprintf (evb, &stats, "%s %ss: %s (in Prim VBs: %s in Depn VBs: %s)\n", 
                           z_dt_name(),
                           DTPZ (line_name), str_int_commas (num_alignments).s, str_int_commas (z_file->comp_num_lines[SAM_COMP_PRIM]).s, 
                           str_int_commas (z_file->comp_num_lines[SAM_COMP_DEPN]).s);
            else
                bufprintf (evb, &stats, "%s %ss: %s\n", 
                           z_dt_name(), DTPZ (line_name), str_int_commas (num_alignments).s);

            if (flag.deep)
                bufprintf (evb, &stats, "FASTQ %ss: %s in %u FASTQ files (deep=%.1f%%%s qtype=%s no_qual=%s has_trimmed=%s)\n", 
                           dt_props[DT_FASTQ].line_name, str_int_commas (z_file->deep_stats[NDP_FQ_READS]).s, num_fq_files, deep_pc,
                           cond_int(segconf.sam_cropped_at, " crop=", segconf.sam_cropped_at), qtype_name (segconf.deep_qtype), TF(segconf.deep_no_qual), TF(segconf.deep_has_trimmed));

            bufprintf (evb, &stats, "Contexts: %u  Vblocks: %u x %u MB  Sections: %s\n", 
                       num_used_ctxs, z_file->num_vbs, (uint32_t)(segconf.vb_size >> 20), str_int_commas (z_file->section_list_buf.len).s);

            uint32_t num_deep_fq_vbs = 0;
            if (flag.deep) 
                for (CompIType comp_i = SAM_COMP_FQ00; comp_i < z_file->num_components; comp_i++)
                    num_deep_fq_vbs += sections_get_num_vbs (comp_i);

            if (z_has_gencomp) {
                bufprintf (evb, &stats, "Main VBs: %u Prim VBs: %u Depn VBs: %u", 
                           sections_get_num_vbs(SAM_COMP_MAIN), sections_get_num_vbs(SAM_COMP_PRIM), sections_get_num_vbs(SAM_COMP_DEPN));

                if (flag.deep) 
                    bufprintf (evb, &stats, " FASTQ VBs: %u", num_deep_fq_vbs);

                bufprint0 (evb, &stats, "\n");
            }
            else if (flag.deep) 
                bufprintf (evb, &stats, "%s VBs: %u FASTQ VBs: %u\n", z_dt_name(), sections_get_num_vbs(SAM_COMP_MAIN), num_deep_fq_vbs);
            else
                REPORT_VBs;

            bufprintf (evb, &features, "hdr_ncontigs=%u;", sam_num_header_contigs());
            if (IS_REF_LOADED_ZIP) bufprintf (evb, &features, "ref_ncontigs=%u;", ref_get_ctgs (gref)->contigs.len32);
            bufprintf (evb, &features, "ref_nbases=%"PRIu64";", contigs_get_nbases (IS_REF_INTERNAL ? sam_hdr_contigs : ref_get_ctgs (gref)));
            if (flag.deep) bufprintf (evb, &features, "deep=%.1f%%;", deep_pc);
            if (flag.deep && segconf.sam_cropped_at) bufprintf (evb, &features, "deep_crop=%ubp;", segconf.sam_cropped_at);
            if (flag.deep) bufprintf (evb, &features, "deep_qtype=%s;", qtype_name (segconf.deep_qtype));
            if (flag.deep) bufprintf (evb, &features, "deep_has_trimmed=%s;", TF(segconf.deep_has_trimmed));
            if (flag.deep) bufprintf (evb, &features, "deep_no_qual=%s;", TF (segconf.deep_no_qual));

            if (num_alignments) {
                FEATURE0 (segconf.is_sorted && !segconf.sam_is_unmapped, "Sorting: Sorted by POS", "Sorted_by_POS");        
                FEATURE0 (segconf.is_sorted && segconf.sam_is_unmapped, "Sorting: Unmapped", "Unmapped");        
                FEATURE0 (segconf.is_collated, "Sorting: Collated by QNAME", "Collated_by_QNAME");
                FEATURE0 (!segconf.is_sorted && !segconf.is_collated, "Sorting: Not sorted or collated", "Not_sorted_or_collated");
            }
                        
            FEATURE (true, "Aligner: %s", "Mapper=%s", segconf_sam_mapper_name()); 
            FEATURE0 (segconf.sam_bisulfite, "Feature: Bisulfite", "Bisulfite");
            FEATURE0 (segconf.is_paired, "Feature: Paired-End", "Paired-End");
            FEATURE0 (segconf.has_cellranger, "Feature: cellranger-style fields", "has_cellranger");
            REPORT_KRAKEN;

            if (segconf.sam_ms_type && segconf.has[OPTION_ms_i]) {
                rom names[] = ms_type_NAME;
                bufprintf (evb, &features, "ms:i_type=%s;", names[segconf.sam_ms_type]);
            }

            if (segconf.sam_XG_inc_S != unknown) 
                bufprintf (evb, &features, "XG_include_S=%s;", TF(segconf.sam_XG_inc_S));

            if (num_alignments) {
                double mate_line_pc  = 100.0 * (double)z_file->mate_line_count  / (double)num_alignments;
                double saggy_near_pc = 100.0 * (double)z_file->saggy_near_count / (double)num_alignments;
                double prim_far_pc   = 100.0 * (double)z_file->prim_far_count   / (double)num_alignments;
                #define PREC(f) ((f && f<10) ? (1 + ((f)<1)) : 0)
                FEATURE(true, "Buddying: sag_type=%s mate=%.*f%% saggy_near=%.*f%% prim_far=%.*f%%",
                        "sag_type=%s;mate=%.*f%%;saggy_near=%.*f%%;prim_far=%.*f%%",
                        sag_type_name (segconf.sag_type), PREC(mate_line_pc), mate_line_pc, PREC(saggy_near_pc), saggy_near_pc, PREC(prim_far_pc), prim_far_pc);

                if (z_file->num_aligned) {
                    bufprintf (evb, &features, "aligner_ok=%.1f%%;", 100.0 * (double)z_file->num_aligned / (double)z_file->num_lines);
                    bufprintf (evb, &features, "aligner_perfect=%.1f%%;", 100.0 * (double)z_file->num_perfect_matches / (double)z_file->num_lines);
                }

                if (flag.deep) 
                    FEATURE (z_file->num_lines, "SAM qname: %s", "SAM_Qname=%s", segconf_qf_name (QSAM))

                REPORT_QNAME;
            }            
            break;
        }

        case DT_VCF:
        case DT_BCF:
            bufprintf (evb, &stats, "Samples: %u   ", vcf_header_get_num_samples()); //  no newline
            bufprintf (evb, &features, "num_samples=%u;", vcf_header_get_num_samples());
            if (IS_REF_LOADED_ZIP) bufprintf (evb, &features, "ref_ncontigs=%u;", ref_get_ctgs (gref)->contigs.len32);
            if (IS_REF_LOADED_ZIP) bufprintf (evb, &features, "ref_nbases=%"PRIu64";", contigs_get_nbases (ref_get_ctgs (gref)));

            if (z_is_dvcf)
                bufprintf (evb, &stats, "%ss: %s (Prim-only: %s Luft-only: %s)  Contexts: %u   Vblocks: %u x %u MB  Sections: %u\n", 
                        DTPZ (line_name), str_int_commas (z_file->num_lines).s, str_int_commas (z_file->comp_num_lines[VCF_COMP_PRIM_ONLY]).s, 
                        str_int_commas (z_file->comp_num_lines[VCF_COMP_LUFT_ONLY]).s, num_used_ctxs, 
                        z_file->num_vbs, (uint32_t)(segconf.vb_size >> 20), z_file->section_list_buf.len32);
           
            REPORT_VBs;

            if (z_is_dvcf) {
                bufprintf (evb, &stats, "Features: Dual-coordinates: Main VBs: %u Prim-only VBs: %u Luft-only VBs: %u\n", 
                        sections_get_num_vbs(VCF_COMP_MAIN), sections_get_num_vbs(VCF_COMP_PRIM_ONLY), sections_get_num_vbs(VCF_COMP_LUFT_ONLY));
                bufprint0 (evb, &features, "DVCF;");
            }

            break;

        case DT_KRAKEN: {
            REPORT_VBs;

            int64_t dominant_taxid_count;
            rom dominant_taxid = ctx_get_snip_with_largest_count (KRAKEN_TAXID, &dominant_taxid_count);

            if (dominant_taxid_count != -1)
                bufprintf (evb, &stats, "Dominant TaxID: %s  %s: %s (%-5.2f%%)\n", dominant_taxid, DTPZ (line_name),
                        str_int_commas (dominant_taxid_count).s, 100.0 * (float)dominant_taxid_count / (float)z_file->num_lines); 
            else
                bufprint0 (evb, &stats, "Dominant TaxID: No dominant organism\n"); 

            REPORT_QNAME;
            break;
        }
        
        case DT_CHAIN: 
            REPORT_VBs;
            if (IS_REF_MAKE_CHAIN && !segconf.chain_mismatches_ref)
                bufprint0 (evb, &stats, "Features: Chain file suitable for use with genozip --chain\n");
            break;

        case DT_FASTQ:
            REPORT_VBs;
            REPORT_QNAME;
            FEATURE (flag.optimize_DESC && z_file->num_lines, "Sequencer: %s", "Sequencer=%s", segconf_tech_name());\
            FEATURE0 (segconf.multiseq, "Multiseq", "multiseq");
            if (IS_REF_LOADED_ZIP) bufprintf (evb, &features, "ref_ncontigs=%u;", ref_get_ctgs (gref)->contigs.len32);
            if (IS_REF_LOADED_ZIP) bufprintf (evb, &features, "ref_nbases=%"PRIu64";", contigs_get_nbases (ref_get_ctgs (gref)));
            if (z_file->num_aligned) {
                bufprintf (evb, &features, "aligner_ok=%.1f%%;", 100.0 * (double)z_file->num_aligned / (double)z_file->num_lines);                
                bufprintf (evb, &features, "aligner_perfect=%.1f%%;", 100.0 * (double)z_file->num_perfect_matches / (double)z_file->num_lines);
            }
            REPORT_KRAKEN;
            if (segconf.r1_or_r2) bufprintf (evb, &features, "R1_or_R2=R%d;", (segconf.r1_or_r2 == PAIR_R1) ? 1 : 2);

            break;

        case DT_FASTA:
            FEATURE0 (segconf.seq_type==SQT_AMINO, "Sequence type: Amino acids",      "Amino_acids");
            FEATURE0 (segconf.seq_type==SQT_NUKE, "Sequence type: Nucleotide bases", "Nucleotide_bases");
            FEATURE0 (segconf.multiseq, "Multiseq", "multiseq");
            FEATURE (true, "Sequences: %"PRIu64, "num_sequences=%"PRIu64, z_file->num_sequences);
            REPORT_VBs;
            REPORT_KRAKEN;
            break;

        case DT_GFF:
            REPORT_VBs;
            FEATURE (true, "GFF version: %d", "GFF_version=%d", segconf.gff_version); 
            FEATURE (segconf.has_embedded_fasta, "FASTA Sequences: %"PRIu64, "num_fasta_sequences=%"PRIu64, z_file->num_sequences);
            break;
            
        case DT_BED:
            if (z_file->num_lines) {
                FEATURE (true, "Columns: %u", "columns=%u", segconf.bed_num_flds);
                FEATURE0 (segconf.is_sorted, "Sorting: Sorted", "Sorted");        
            }
            break;
            
        case DT_REF: 
            bufprintf (evb, &stats, "%s of genome (in-memory): %s\n", digest_name(), digest_display (z_file->digest).s); 
            FEATURE (true, "Contigs: %u", "num_contigs=%u", ref_contigs_get_num_contigs(gref));
            FEATURE (true, "Bases: %"PRIu64, "num_bases=%"PRIu64, ref_contigs_get_genome_nbases(gref));
            break;

        case DT_GENERIC:
            REPORT_VBs;
            bufprintf (evb, &stats, "Vblocks: %u x %u MB  Sections: %u\n", 
                       z_file->num_vbs, (uint32_t)(segconf.vb_size >> 20), z_file->section_list_buf.len32);
    
            bufprintf (evb, &features, "magic=%s;extension=\"%s\";", generic_get_magic(), generic_get_ext());
            break;

        default:
            REPORT_VBs;
    }
    
    if (!flag.make_reference && z_file->num_lines) {
        bufprintf (evb, &features, "segconf.line_len=%u;", segconf.line_len); 
        if (segconf.longest_seq_len) bufprintf (evb, &features, "segconf.longest_seq_len=%u;", segconf.longest_seq_len); 
    }

    if (stats_programs.len) {
        if (*BLSTc(stats_programs) == ';') stats_programs.len--; // remove final ';'
     
        bufprintf (evb, &stats, "Programs: %.*s\n", STRfb(stats_programs));
    }

    bufprintf (evb, &stats, "Genozip version: %s %s\nDate compressed: %s\n", 
               GENOZIP_CODE_VERSION, arch_get_distribution(), str_time().s);

    bufprint0 (evb, &stats, "Command line: ");
    buf_append_string (evb, &stats, flags_command_line()); // careful not to use bufprintf with command_line as it can exceed the maximum length in bufprintf

    bufprintf (evb, &stats, "\n%s\n", license_get_one_line());
    
    *BAFTc(features) = '\0';
}

static DESCENDING_SORTER (stats_sort_by_z_size, StatsByLine, z_size)

static void stats_consolidate_ctxs (StatsByLine *sbl, unsigned num_stats)
{
    for (unsigned parent=0; parent < num_stats; parent++) 

        // case: we might consolidate to this context
        if (sbl[parent].my_did_i != DID_NONE && 
            (sbl[parent].st_did_i == DID_NONE || sbl[parent].st_did_i == sbl[parent].my_did_i))  {
            
            for (unsigned child=0; child < num_stats; child++) {

                if (child != parent && sbl[parent].my_did_i  == sbl[child].st_did_i) {
                    sbl[parent].txt_len   += sbl[child].txt_len;
                    sbl[parent].z_size    += sbl[child].z_size;  
                    sbl[parent].pc_of_txt += sbl[child].pc_of_txt;
                    sbl[parent].pc_of_z   += sbl[child].pc_of_z;  

                    if (flag.debug_stats)
                        iprintf ("Consolidated %s (did=%u) (txt_len=%"PRIu64" z_size=%"PRIu64") into %s (did=%u) (AFTER: txt_len=%"PRIu64" z_size=%"PRIu64")\n",
                                 sbl[child].name, sbl[child].my_did_i, sbl[child].txt_len, sbl[child].z_size, 
                                 sbl[parent].name, sbl[parent].my_did_i, sbl[parent].txt_len, sbl[parent].z_size);

                    sbl[child] = (StatsByLine){ .my_did_i = DID_NONE, .st_did_i = DID_NONE };
                }
            }

            if (!strcmp (sbl[parent].name, "SQBITMAP")) strcpy (sbl[parent].name, "SEQ"); // rename
        }
}

static void stats_consolidate_non_ctx (StatsByLine *sbl, unsigned num_stats, rom consolidated_name, 
                                       unsigned num_deps, ...)
{
    va_list args;
    va_start (args, num_deps);

    rom deps[num_deps];
    for (unsigned d=0; d < num_deps; d++) 
        deps[d] = va_arg (args, rom );

    // use existing SBL if it matches the consolidated name
    StatsByLine *survivor = NULL;
    for (StatsByLine *s=sbl, *after=sbl + num_stats; s < after; s++) 
        if (s->name[0] && !strcmp (consolidated_name, s->name)) {
            survivor = s;
            break;
        }

    for (StatsByLine *s=sbl, *after=sbl + num_stats; s < after; s++) {
        
        if (!s->name[0]) continue; // unused entry

        for (unsigned d=0; d < num_deps; d++) 
            if (!strcmp (deps[d], s->name)) {
                if (!survivor) {
                    survivor = s; // first found gets to be the survivor
                }
                else {
                    survivor->txt_len   += s->txt_len;
                    survivor->z_size    += s->z_size;  
                    survivor->pc_of_txt += s->pc_of_txt;
                    survivor->pc_of_z   += s->pc_of_z; 
                    strcpy (survivor->name, consolidated_name); // rename only if at least one was consolidated

                    if (flag.debug_stats)
                        iprintf ("Consolidated %s (txt_len=%"PRIu64" z_size=%"PRIu64") into %s (AFTER: txt_len=%"PRIu64" z_size=%"PRIu64")\n",
                                 s->name, s->txt_len, s->z_size, survivor->name, survivor->txt_len, survivor->z_size);

                    *s = (StatsByLine){}; 
                }
                break;
            } 
    }

    va_end (args);
}

static void stats_output_stats (StatsByLine *s, unsigned num_stats, float src_comp_ratio,
                                int64_t all_txt_len, int64_t all_txt_len_0, int64_t all_z_size, float all_pc_of_txt, float all_pc_of_z, float all_comp_ratio)
{
    bufprintf (evb, &stats, "\nSections (sorted by %% of genozip file):%s\n", "");
    bufprintf (evb, &stats, "NAME                   GENOZIP      %%       TXT      %%   RATIO\n%s", "");

    // if -W or -w appear multiple times, we print the stats header to stderr to allow use with "| grep"
    if (flag.show_stats == STATS_SHORT_GREP) { 
        fprintf (stderr, "NAME                   GENOZIP      %%       TXT      %%   RATIO\n%s", "");
        fflush (stderr);
    }

    for (uint32_t i=0; i < num_stats; i++, s++)
        if (s->z_size)
            bufprintf (evb, &stats, "%-20.20s %9s %5.1f%% %9s %5.1f%% %6.*fX\n", 
                       s->name, 
                       str_size (s->z_size).s, s->pc_of_z, // z size and % of total z that is in this line
                       str_size ((float)s->txt_len).s, s->pc_of_txt, // txt size and % of total txt which is in this line
                       ((float)s->txt_len / (float)s->z_size) < 1000 ? 1 : 0, // precision
                       (float)s->txt_len / (float)s->z_size); // ratio z vs txt

    if (src_comp_ratio != 1 && flag.show_stats_comp_i == COMP_NONE) {

        // display codec name if same source codec for all components, or "DISK_SIZE" if not
        rom source_code_name = codec_name (z_file->comp_source_codec[0]);

        for (CompIType comp_i=1; comp_i < z_file->num_components; comp_i++)
            if (z_file->comp_source_codec[comp_i] != z_file->comp_source_codec[0])
                source_code_name = "DISK_SIZE";

        bufprintf (evb, &stats, 
                   "GENOZIP vs %-9s %9s %5.1f%% %9s %5.1f%% %6.1fX\n", 
                   source_code_name,
                   str_size (all_z_size).s, all_pc_of_z, // total z size and sum of all % of z (should be 100)
                   str_size (all_txt_len_0 / src_comp_ratio).s, all_pc_of_txt, // total txt fize and ratio z vs txt
                   all_comp_ratio / src_comp_ratio);
    }

    // note: no point showing this per component in DVCF, bc all_txt_len_0 is fully accounted for in the MAIN component and it is 0 in the others
    if (flag.show_stats_comp_i == COMP_NONE || !z_is_dvcf)
        bufprintf (evb, &stats, 
                   "%-20s %9s %5.1f%% %9s %5.1f%% %6.1fX\n", 
                   src_comp_ratio != 1 ? "GENOZIP vs TXT" : "TOTAL",
                   str_size (all_z_size).s, all_pc_of_z, // total z size and sum of all % of z (should be 100)
                   str_size (all_txt_len_0).s, all_pc_of_txt, // total txt fize and ratio z vs txt
                   all_comp_ratio);
}

static void stats_output_STATS (StatsByLine *s, unsigned num_stats,
                                int64_t all_txt_len, int64_t all_uncomp_dict, int64_t all_comp_dict, int64_t all_comp_b250, int64_t all_comp_local, 
                                int64_t all_z_size, float all_pc_of_txt, float all_pc_of_z, float all_comp_ratio)
{
#define PC(pc) ((pc==0 || pc>=10) ? 0 : (pc<1 ? 2:1))

    // add diagnostic info
    if (flag.show_stats_comp_i == COMP_NONE) {
        bufprintf (evb, &STATS, "\nSystem info: OS=%s cores=%u endianity=%s\n", 
                   arch_get_os(), arch_get_num_cores(), arch_get_endianity());
        bufprintf (evb, &STATS, "\nSections (sorted by %% of genozip file):%s\n", "");
    }

    bufprint0 (evb, &STATS, "did_i Name              Parent            #Words  Snips-(% of #Words)     Hash-table   uncomp      comp      comp    uncomp      comp      comp       txt    comp   % of   % of\n");
    bufprint0 (evb, &STATS, "                                         in file   Dict  Local FailSton   Size Occp      dict      dict      b250     local     local     TOTAL             ratio    txt    zip\n");
    // note: bufprint0 requires % to display a %, while fprintf requires %%.
    
    // if -W or -w appear multiple times, we print the stats header to stderr to allow use with "| grep"
    if (flag.show_stats == STATS_LONG_GREP) {
        fprintf (stderr,    "did_i Name              Parent            #Words  Snips-(%% of #Words)     Hash-table   uncomp      comp      comp    uncomp      comp      comp       txt    comp   %% of   %% of\n");
        fprintf (stderr,    "                                         in file   Dict  Local FailSton   Size Occp      dict      dict      b250     local     local     TOTAL             ratio    txt    zip\n");
    }

    for (uint32_t i=0; i < num_stats; i++, s++)
        if (s->z_size)
            bufprintf (evb, &STATS, "%-2.2s    %-17.17s %-17.17s %6s  %4.*f%%  %4.*f%%  %4.*f%% %8s %3.0f%% %9s %9s %9s %9s %9s %9s %9s %6.*fX %5.1f%% %5.1f%%\n", 
                       s->did_i.s, s->name, s->type, s->words.s, 
                       PC (s->pc_dict), s->pc_dict, PC(s->pc_in_local), s->pc_in_local, PC(s->pc_failed_singletons), s->pc_failed_singletons, 
                       (s->is_dict_alias ? "ALIAS  " : s->hash.s), s->pc_hash_occupancy, // Up to here - these don't appear in the total
                       s->uncomp_dict.s, s->comp_dict.s, s->comp_b250.s, s->uncomp_local.s, s->comp_local.s, str_size (s->z_size).s, 
                       str_size ((float)s->txt_len).s, 
                       ((float)s->txt_len / (float)s->z_size) < 1000 ? 1 : 0, // precision
                        (float)s->txt_len / (float)s->z_size, // ratio z vs txt
                       s->pc_of_txt, s->pc_of_z);

    // note: no point showing this per component in SAM and DVCF, bc all_txt_len_0 is fully accounted for in the MAIN component and it is 0 in the others
    if (!(z_has_gencomp && flag.show_stats_comp_i != COMP_NONE))
        bufprintf (evb, &STATS, "TOTAL                                                                               "
                "%9s %9s %9s %9s %9s %9s %6.1fX %5.1f%% %5.1f%%\n", 
                str_size (all_uncomp_dict).s, str_size (all_comp_dict).s, str_size (all_comp_b250).s, 
                str_size (all_comp_local).s,   str_size (all_z_size).s,    str_size (all_txt_len).s, 
                all_comp_ratio, all_pc_of_txt, all_pc_of_z);
}

// generate the stats text - all sections except genozip header and the two stats sections 
void stats_generate (void) // specific section, or COMP_NONE if for the entire file
{
    // initial allocation
    buf_alloc (evb, &stats,     0, 10000,  char, 1, "stats");
    buf_alloc (evb, &STATS,     0, 10000,  char, 1, "stats");
    buf_alloc (evb, &features,  0, 1000,   char, 1, "stats");
    buf_alloc (evb, &hash_occ,  0, 1000,   char, 1, "stats");
    buf_alloc (evb, &internals, 0, 1000,   char, 1, "stats");

    if (flag.show_stats_comp_i == COMP_NONE) {
        stats_output_file_metadata();
        buf_copy (evb, &STATS, &stats, char,0,0, "stats");
    }

    int64_t all_comp_dict=0, all_uncomp_dict=0, all_comp_b250=0, all_comp_local=0, all_z_size=0;

    // prepare data
    #define NUM_SBL (NUM_SEC_TYPES + z_file->num_contexts + 2) // 2 for consolidated groups
    ARRAY_alloc (StatsByLine, sbl, NUM_SBL, true, sbl_buf, evb, "stats");

    #define ST_NAME(st) (&st_name(st)[4]) // cut off "SEC_" 

    stats_get_compressed_sizes (sbl); // ctx sizes in ctx->local/dict/ctx.param, and other sections in sbl[st]->z_size.
    
    uint64_t z_size     = flag.show_stats_comp_i==COMP_NONE ? z_file->disk_so_far            : z_file->disk_so_far_comp           [flag.show_stats_comp_i];
    uint64_t txt_size   = flag.show_stats_comp_i==COMP_NONE ? z_file->txt_data_so_far_bind   : z_file->txt_data_so_far_bind_comp  [flag.show_stats_comp_i];
    uint64_t txt_size_0 = flag.show_stats_comp_i==COMP_NONE ? z_file->txt_data_so_far_bind_0 : z_file->txt_data_so_far_bind_0_comp[flag.show_stats_comp_i];
    StatsByLine *s = sbl;

    for (SectionType st=0; st < NUM_SEC_TYPES; st++, s++) { 

        if (ST(DICT) || ST(B250) || ST(LOCAL) || ST(COUNTS)) continue; // these are covered by individual contexts

        s->txt_len    = ST(TXT_HEADER)  ? z_file->header_size : 0; // note: excluding generated headers for DVCF
        s->type       = (ST(REFERENCE) || ST(REF_IS_SET) || ST(REF_CONTIGS) || ST(CHROM2REF_MAP) || ST(REF_IUPACS)) ? "SEQUENCE" 
                      : (ST(RANDOM_ACCESS) || ST(REF_RAND_ACC))                                                                   ? "RandomAccessIndex"
                      :                                                                                                                     "Other"; // note: some contexts appear as "Other" in --stats, but in --STATS their parent is themself, not "Other"
        s->my_did_i   = s->st_did_i = DID_NONE;
        s->did_i.s[0] = s->words.s[0] = s->hash.s[0] = s->uncomp_dict.s[0] = s->comp_dict.s[0] = '-';
        s->pc_of_txt  = txt_size ? 100.0 * (float)s->txt_len / (float)txt_size : 0;
        s->pc_of_z    = z_size   ? 100.0 * (float)s->z_size  / (float)z_size   : 0;
        strcpy (s->name, ST_NAME (st)); 

        all_z_size     += s->z_size;
        all_comp_local += s->z_size;
        all_txt_len    += s->txt_len;
    }

    z_file->header_size = 0; // reset (in case of showing components)
    
    // contexts
    for_zctx {    
        s->z_size = zctx->dict.count + zctx->b250.count + zctx->local.count;

        if (!zctx->b250.count && !zctx->txt_len && !zctx->b250.len && !zctx->is_stats_parent && !s->z_size) 
            continue;

        s->txt_len = zctx->txt_len;
        
        all_comp_dict   += zctx->dict.count;
        all_uncomp_dict += zctx->dict.len;
        all_comp_b250   += zctx->b250.count;
        all_comp_local  += zctx->local.count;
        all_z_size      += s->z_size;
        all_txt_len     += s->txt_len;

        if ((Z_DT(VCF) || Z_DT(BCF) || Z_DT(GFF)) && dict_id_type(zctx->dict_id) != DTYPE_FIELD)
            sprintf (s->name, "%s/%s", dtype_name_z(zctx->dict_id), zctx->tag_name);
        else 
            strcpy (s->name, zctx->tag_name);

        // parent
        s->type  = zctx->dict_id.num                 == _SAM_SQBITMAP ? "SEQUENCE"
                 : zctx->st_did_i == DID_NONE                         ? zctx->tag_name
                 : ZCTX(zctx->st_did_i)->dict_id.num == _SAM_SQBITMAP ? "SEQUENCE"
                 :                                                      ZCTX(zctx->st_did_i)->tag_name;

        // note: each VB contributes local.len contains its b250.count if it has it, and local_num_len if not 
        float n_words = zctx->word_list.count; // fields segged of this type in the file : set in ctx_update_stats()

        s->my_did_i             = zctx->did_i;
        s->st_did_i             = zctx->st_did_i;
        s->did_i                = str_int_commas ((uint64_t)zctx->did_i); 
        s->words                = str_uint_commas_limit (n_words, 99999);
        s->pc_dict              = !n_words ? 0 : 100.0 * (float)MIN_(zctx->nodes.len, n_words) / n_words; // MIN_ is a workaround - not sure why nodes.len sometimes exceeds the dictionary words on the file (eg in TOPLEVEL)
        s->pc_in_local          = !n_words ? 0 : 100.0 * (float)zctx->local_num_words / n_words;
        s->pc_failed_singletons = !zctx->b250.count ? 0 : 100.0 * (float)zctx->num_failed_singletons / (float)zctx->b250.len;
        s->pc_hash_occupancy    = !zctx->global_hash_prime ? 0 : 100.0 * (float)(zctx->nodes.len + zctx->ston_nodes.len) / (float)zctx->global_hash_prime;
        s->global_hash_prime    = zctx->global_hash_prime;
        s->hash                 = str_size (zctx->global_hash_prime);
        s->uncomp_dict          = str_size (zctx->dict.len);
        s->comp_dict            = str_size (zctx->dict.count);
        s->comp_b250            = str_size (zctx->b250.count);
        s->uncomp_local         = str_size (zctx->local.len); // set in ctx_update_stats()
        s->comp_local           = str_size (zctx->local.count);
        s->pc_of_txt            = txt_size ? 100.0 * (float)s->txt_len / (float)txt_size : 0;
        s->pc_of_z              = z_size   ? 100.0 * (float)s->z_size  / (float)z_size   : 0;
        s->dcodec               = codec_name (zctx->dcodec);
        s->bcodec               = codec_name (zctx->bcodec);
        s->lcodec               = codec_name (zctx->lcodec_non_inherited ? zctx->lcodec_non_inherited : zctx->lcodec);
        s->is_dict_alias        = (zctx->did_i != zctx->dict_did_i);
        
        s++; // increment only if it has some data, otherwise already continued
    }

    sbl_buf.len32 = s - sbl;

    // note: for txt size and compression ratio in the TOTAL line (all_comp_ratio) we use txt_data_so_far_bind_0 
    // (the original txt data size) and not all_txt_size (size after ZIP modifications like --optimize). 
    // Therefore, in case of ZIP-modified txt, the sum of the (modified) fields in the TXT column will NOT equal the
    // TOTAL in the TXT column. That's ok.
    all_comp_ratio       = (float)txt_size_0 /* without modifications */ / (float)all_z_size;
    float all_pc_of_txt  = txt_size ? 100.0 * (float)all_txt_len / (float)txt_size : 0 /* with modifications */;
    float all_pc_of_z    = z_size   ? 100.0 * (float)all_z_size  / (float)z_size   : 0;

    // long form stats from --STATS    
    qsort (sbl, sbl_buf.len32, sizeof (sbl[0]), stats_sort_by_z_size);  // sort by compressed size

    stats_prepare_internals (sbl, sbl_buf.len32, all_z_size);

    stats_output_STATS (sbl, sbl_buf.len32, 
                        all_txt_len, all_uncomp_dict, all_comp_dict, all_comp_b250, all_comp_local, all_z_size, all_pc_of_txt, all_pc_of_z, all_comp_ratio);

    // calculate hash occupancy before consolidating stats
    stats_calc_hash_occ (sbl, sbl_buf.len32);
    
    // consolidates stats of child contexts into the parent one
    stats_consolidate_ctxs (sbl, sbl_buf.len32);
    
    stats_consolidate_non_ctx (sbl, sbl_buf.len32, 
                               IS_REF_EXT_STORE ? "Reference" : "SEQ", // when compressing SAM/FASTQ with REF_EXT_STORE, account for the reference in its own "Parent"
                               5, ST_NAME (SEC_REFERENCE), ST_NAME (SEC_REF_IS_SET), 
                               ST_NAME (SEC_REF_CONTIGS), ST_NAME (SEC_CHROM2REF_MAP),
                               ST_NAME (SEC_REF_IUPACS));

    stats_consolidate_non_ctx (sbl, sbl_buf.len32, "Other", 22 + (DTPZ(txt_header_required) == HDR_NONE), "E1L", "E2L", "EOL", 
                               "SAMPLES", "AUX", TOPLEVEL, "ToPLUFT", "TOP2BAM", "TOP2FQ", "TOP2NONE", "TOP2FQEX", "TOP2VCF", "TOP2HASH", 
                               "LINEMETA", "CONTIG", "COORDS", "SAG", "SAALN",
                               ST_NAME (SEC_DICT_ID_ALIASES), ST_NAME (SEC_RECON_PLAN),
                               ST_NAME (SEC_VB_HEADER), ST_NAME (SEC_BGZF), ST_NAME(SEC_TXT_HEADER)/*must be last*/);
        
    stats_consolidate_non_ctx (sbl, sbl_buf.len32, "RandomAccessIndex", 2, ST_NAME (SEC_RANDOM_ACCESS), ST_NAME (SEC_REF_RAND_ACC));
    
    ASSERTW (all_txt_len == txt_size || flag.make_reference || // all_txt_len=0 in make-ref as there are no contexts
             z_is_dvcf, // temporarily remove DVCF - our header data accounting is wrong (bug 681)
             "Expecting all_txt_len(sum of txt_len of all contexts)=%"PRId64" == txt_size(modified txt_file size)=%"PRId64" (diff=%"PRId64")", all_txt_len, txt_size, (int64_t)txt_size - all_txt_len);

    // short form stats from --stats    
    qsort (sbl, sbl_buf.len32, sizeof (sbl[0]), stats_sort_by_z_size);  // re-sort after consolidation

    // source compression, eg BGZF, against txt before any modifications
    src_comp_ratio = (float)txt_size_0 / z_file->txt_file_disk_sizes_sum; 

    stats_output_stats (sbl, sbl_buf.len32, src_comp_ratio, all_txt_len, txt_size_0, all_z_size, all_pc_of_txt, all_pc_of_z, all_comp_ratio);
    
    // if we're showing stats of a single components - output it now
    if (flag.show_stats_comp_i != COMP_NONE) {
        iprintf ("\n\nComponent=%s:\n", comp_name (flag.show_stats_comp_i));
        buf_print (ABS(flag.show_stats) == STATS_SHORT ? &stats : &STATS, false);
        buf_free (stats);
        buf_free (STATS);
    }

    // note: we use txt_data_so_far_bind is the sum of recon_sizes - see zip_update_txt_counters - which is
    // expected to be the sum of txt_len. However, this NOT the size of the original file which is stored in
    // z_file->txt_data_so_far_bind_0.
    ASSERT (!flag.debug_or_test || flag.show_stats_comp_i != COMP_NONE || all_txt_len == txt_size || flag.data_modified || z_is_dvcf || flag.make_reference, 
            "Hmm... incorrect calculation for %s sizes: total section sizes=%s but file size is %s (diff=%d)", 
            z_dt_name(), str_int_commas (all_txt_len).s, str_int_commas (txt_size).s, 
            (int32_t)(txt_size - all_txt_len)); 

    if (flag.show_stats_comp_i == COMP_NONE) { 
        
        zfile_compress_section_data (evb, SEC_STATS, &stats);
        zfile_compress_section_data (evb, SEC_STATS, &STATS);

        // store stats overhead, for stats_display (because when stats_display runs, section list won't be accessible since it is already converted to SectionEntFileFormat)
        Section sec = sections_last_sec (SEC_STATS, HARD_FAIL) - 1; // first stats section     
        stats.count = z_file->disk_so_far - sec->offset;
    }

    if (flag.show_stats_comp_i != COMP_NONE) 
        iprint0 ("\nNote: Components stats don't include global sections like SEC_DICT, SEC_REFERENCE etc\n");

    if (!(flag.show_stats && z_file->z_closes_after_me)) { // case: we're not going to display these stats - free now
        buf_destroy (stats);
        buf_destroy (STATS);
    }
}

static void stats_display (void)
{
    BufferP buf = ABS(flag.show_stats) == 1 ? &stats : &STATS;

    if (!buf_is_alloc (buf)) return;  // no stats available

    buf_print (buf , false);

    if (stats.count && z_file->disk_size < 1 MB && command==ZIP)  // no need to print this note if z size > 1MB, as the 2-4KB of overhead is rounded off anyway
        // stats text doesn't include SEC_STATS and SEC_GENOZIP_HEADER - the last 3 sections in the file - since stats text is generated before these sections are compressed
        iprintf ("\nNote: ZIP total file size excludes overhead of %s\n", str_size (stats.count).s);

    iprint0 ("\n");

    buf_destroy (stats);
    buf_destroy (STATS);
}

void stats_read_and_display (void)
{
    Section sec = sections_last_sec (SEC_STATS, SOFT_FAIL);
    if (!sec) {
        iprint0 ("No stats available for this file.\n"); // possibly the file was compressed with --cstats or --CSTATS
        return; // genozip file does not contain stats sections (SEC_STATS was introduced in v 7.0.5)
    }

    // if -W or -w appear multiple times, we print the stats header to stderr to allow use with "| grep"
    if (flag.show_stats == STATS_SHORT_GREP) 
        fprintf (stderr, "NAME                   GENOZIP      %%       TXT      %%   RATIO\n%s", "");

    else if (flag.show_stats == STATS_LONG_GREP) {
        fprintf (stderr, "did_i Name              Parent            #Words  Snips-(%% of #Words)    Hash-table    uncomp      comp      comp      comp      comp       txt    comp   %% of   %% of\n");
        fprintf (stderr, "                                         in file   Dict  Local FailSton   Size Occp      dict      dict      b250     local     TOTAL             ratio    txt    zip\n");
    }

    // read and uncompress the requested stats section
    zfile_get_global_section (SectionHeader, sec - (ABS(flag.show_stats)==1),
                              ABS(flag.show_stats) == 1 ? &stats : &STATS, "stats");
    
    stats_display();
}

// concatenate txt names of bound files so we can show them all
void stats_add_txt_name (rom fn)
{
    bufprintf (evb, &z_file->bound_txt_names, "%s%s", z_file->bound_txt_names.len ? " ": "", fn);
    z_file->bound_txt_names.count++; // number of files
}

void stats_finalize (void)
{
    if (flag.show_stats) stats_display();

    buf_destroy (stats);
    buf_destroy (STATS);
    buf_destroy (features);
    buf_destroy (hash_occ);
    buf_destroy (internals);
    buf_destroy (STATS);
    buf_destroy (sbl_buf);
    buf_destroy (stats_programs);

    all_txt_len = 0;
    src_comp_ratio = all_comp_ratio = 0;    
}
 