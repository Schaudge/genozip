// ------------------------------------------------------------------
//   stats.c
//   Copyright (C) 2019-2022 Genozip Limited
//   Please see terms and conditions in the file LICENSE.txt

#include <stdarg.h>
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

typedef struct {
    DidIType my_did_i, st_did_i;
    int64_t txt_len, z_size;
    char name[100];
    rom type;
    StrText did_i, words, hash, uncomp_dict, comp_dict, comp_b250, comp_data;
    float pc_of_txt, pc_of_z, pc_dict, pc_in_local, pc_failed_singletons, pc_hash_occupancy;
    uint32_t global_hash_prime;
} StatsByLine;

static Buffer stats={}, STATS={}, features={}, hash_occ={};

// calculate hash_occ before consolidating stats
static void stats_submit_calc_hash_occ (StatsByLine *sbl, unsigned num_stats)
{
    for (uint32_t i=0, need_sep=0; i < num_stats; i++) 
        if (sbl[i].pc_hash_occupancy >= 50) {
            ContextP zctx = ZCTX(sbl[i].my_did_i);
            uint32_t n_words = zctx->nodes.len32;
             
            // in case of an over-populated hash table, we send the first 3 and last 3 words in the dictionary, which will help debugging the issue
            bufprintf (evb, &hash_occ, "%s%s%%2C%s%%2C%s%%2C%u%%25%%2C%s%%2C%s%%2C%s%%2C%s%%2C%s%%2C%s", 
                       need_sep++ ? "%3B" : "", url_esc_non_valid_charsS(sbl[i].name).s, url_esc_non_valid_charsS(sbl[i].type).s, 
                       url_esc_non_valid_charsS (str_size (sbl[i].global_hash_prime).s).s, (int)sbl[i].pc_hash_occupancy, 
                       // note: str_replace_letter modifies dict data, but its ok, since we have already written the dicts to z_file
                       url_esc_non_valid_charsS (str_replace_letter ((char *)ctx_get_zf_nodes_snip (zctx, 0, 0, 0),         sizeof(UrlStr), ',', -127)).s, // first three words in the the dictionary of this field
                       url_esc_non_valid_charsS (str_replace_letter ((char *)ctx_get_zf_nodes_snip (zctx, 1, 0, 0),         sizeof(UrlStr), ',', -127)).s,
                       url_esc_non_valid_charsS (str_replace_letter ((char *)ctx_get_zf_nodes_snip (zctx, 2, 0, 0),         sizeof(UrlStr), ',', -127)).s,
                       url_esc_non_valid_charsS (str_replace_letter ((char *)ctx_get_zf_nodes_snip (zctx, n_words-3, 0, 0), sizeof(UrlStr), ',', -127)).s, // last three words 
                       url_esc_non_valid_charsS (str_replace_letter ((char *)ctx_get_zf_nodes_snip (zctx, n_words-2, 0, 0), sizeof(UrlStr), ',', -127)).s,
                       url_esc_non_valid_charsS (str_replace_letter ((char *)ctx_get_zf_nodes_snip (zctx, n_words-1, 0, 0), sizeof(UrlStr), ',', -127)).s);
        }
}

static void stats_submit (StatsByLine *sbl, unsigned num_stats, uint64_t all_txt_len, float src_comp_ratio, float all_comp_ratio)
{
    // run submission in a separate process to not stall the main process 
#ifndef _WIN32
    fflush ((FILE *)z_file->file); // flush before fork, otherwise both processes will have z_file I/O buffers...

    pid_t child_pid = fork();
    if (child_pid) return; // parent returns
#endif

    static Buffer url_buf={};

    // reference: https://stackoverflow.com/questions/18073971/http-post-to-a-google-form/47444396#47444396    
    /* To get entry IDs - in Chrome browser: 1. open form 2. click on eye icon to Preview 2. right-click Inspect 3. go to "console" tab 4. run this code:
    function loop(e){
    if(e.children)
    for(let i=0;i<e.children.length;i++){
        let c = e.children[i], n = c.getAttribute('name');
        if(n) console.log(`${c.getAttribute('aria-label')}: ${n}`);
        loop(e.children[i]);
     }
    }; loop(document.body);
    */ 

    bufprint0 (evb, &url_buf, "https://docs.google.com/forms/d/e/1FAIpQLSdc997k63YnW4fRxnQOHRqngCT_6_fIhrBQZgTXPTgrPCpe_w/formResponse"); // the ID is in the url when previewing the form
    bufprintf (evb, &url_buf, "?entry.1917122099=%s", GENOZIP_CODE_VERSION);                                                   // Genozip version Eg "14.0.0"
    bufprintf (evb, &url_buf, "&entry.1014872627=%u", license_get_number());                                                   // license #. Eg "32412351324"
    bufprintf (evb, &url_buf, "&entry.1861722167=%s", url_esc_non_valid_chars (arch_get_user_host()));                         // user@host. Eg "john@hpc"
    bufprintf (evb, &url_buf, "&entry.441046403=%s", dt_name (z_file->data_type));                                             // data type. Eg "VCF"
    bufprintf (evb, &url_buf, "&entry.984213484=%s", url_esc_non_valid_charsS (str_size (all_txt_len).s).s);                   // Txt size (original file size), eg "6.2 GB"
    bufprintf (evb, &url_buf, "&entry.960659059=%s%%2C%.1f", codec_name (txt_file->codec), src_comp_ratio);                    // Source codec/gain eg "GZ/4.3"
    bufprintf (evb, &url_buf, "&entry.621670070=%.1f", all_comp_ratio);                                                        // Genozip gain over source txt eg "5.4"
    bufprintf (evb, &url_buf, "&entry.1635780209=OS=%s%%3Bcores=%u", url_esc_non_valid_charsS(arch_get_os()).s, arch_get_num_cores());  // Environment: OS, # cores, eg:
    bufprintf (evb, &url_buf, "&entry.2140634550=%s", features.len ? url_esc_non_valid_charsS (B1STc(features)).s : "NONE");                                                        // Features. Eg "Sorted"
    bufprintf (evb, &url_buf, "&entry.282448068=%s",  hash_occ.len ? B1STc(hash_occ) : "NONE");      // Hash ineffeciencies, eg "RNAME,64.0 KB,102%" - each field is quadlet - name, type, hash size, hash occupancy    

    bufprint0 (evb, &url_buf, "&entry.988930848=");      // Compression ratio of individual fields ratio, eg "FORMAT/GT,20%,78;..." - each field is triplet - name, percentage of z_data, compression ratio
    for (uint32_t i=0, need_sep=0; i < num_stats; i++) 
        if (sbl[i].z_size)
            bufprintf (evb, &url_buf, "%s%s%%2C%.1f%%25%%2C%.1fX", 
                       need_sep++ ? "%3B" : "", url_esc_non_valid_charsS(sbl[i].name).s, sbl[i].pc_of_z, (float)sbl[i].txt_len / (float)sbl[i].z_size); // ratio z vs txt

    bufprint0 (evb, &url_buf, "&entry.1369097179=");     // Flags. Eg "best,reference=INTERNAL"

    #define F(name) if (flag.name) { bufprint0 (evb, &url_buf, #name "%3B"); }
    F(best) ; F(fast) ;
    F(optimize) ; F(optimize_DESC) ; F(optimize_phred) ; F(optimize_QUAL) ; F(optimize_Vf) ;
    F(optimize_VQSLOD) ; F(optimize_ZM) ; F(optimize_sort) ; F(GL_to_PL) ; F(GP_to_PP) ;
    F(add_line_numbers) ; F(sort) ; F(unsorted) ; F(md5) ; F(subdirs) ; F(out_filename) ;
    F(replace) ; F(force) ; F(stdin_size) ; F(test) ; F(match_chrom_to_reference) ;
    F(reading_chain); F(reading_kraken) ; F(make_reference) ; F(dvcf_rename) ; F(dvcf_drop) ;
    F(show_lift) ; F(pair) ; F(multiseq) ; F(files_from) ;
    #undef F

    #define F(name,fmt,none_value) if (flag.name != (none_value)) bufprintf (evb, &url_buf, #name "=" fmt "%%3B", flag.name);
    F(show_stats_comp_i, "%u", COMP_NONE);
    F(threads_str, "%s", NULL);
    F(vblock, "%s", NULL);
    #undef F

    if (flag.reference)                bufprintf (evb, &url_buf, "reference=%s%%3B", ref_type_name());    
    if (crypt_have_password())         bufprint0 (evb, &url_buf, "encrypted%3B");    
    if (tar_is_tar())                  bufprint0 (evb, &url_buf, "tar%3B");    
    if (flag.kraken_taxid!=TAXID_NONE) bufprint0 (evb, &url_buf, "taxid%3B");    
    if (file_get_stdin_type())         bufprintf (evb, &url_buf, "stdin_type=%s%%3B", ft_name(file_get_stdin_type()));    
    if (flag.show_one_counts.num)      bufprintf (evb, &url_buf, "show_counts=%s%%3B", url_esc_non_valid_charsS (dis_dict_id(flag.show_one_counts).s).s);

    url_read_string (B1STc(url_buf), NULL, 0); // ignore errors
    
#ifndef _WIN32
    exit(0); // child process is done
#endif

    // note: we leak the (small amount of) memory allocated by url_esc_non_valid_chars()
    buf_free (url_buf);    
    buf_free (hash_occ);    
    buf_free (features);    
}

// store the sizes of dict / b250 / local in zctx->*.param, and of other sections in sbl[st].z_size
static void stats_get_compressed_sizes (StatsByLine *sbl) 
{
    ContextIndex ctx_index[MAX_DICTS];

    // initialize & prepare context index
    for (DidIType did_i=0; did_i < z_file->num_contexts; did_i++) {
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
            DidIType did_i = ctx_get_existing_did_i_do (sec->dict_id, z_file->contexts, z_file->dict_id_to_did_i_map,
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
    bufprint0 (evb, &stats, "\n\n");
    if (txt_file->name) 
        bufprintf (evb, &stats, "%s file%s%s: %.*s\n", dt_name (z_file->data_type), 
                   z_file->bound_txt_names.count > 1 ? "s" : "", // param holds the number of txt files
                   flag.pair ? " (paired)" : "",
                   (int)z_file->bound_txt_names.len, z_file->bound_txt_names.data);
    
    if (flag.reference == REF_MAKE_CHAIN) {
        bufprintf (evb, &stats, "PRIM reference: %s MD5=%s genozip_version=%u\n", ref_get_filename (prim_ref), digest_display (ref_get_file_md5 (prim_ref)).s, ref_get_genozip_version (prim_ref));
        bufprintf (evb, &stats, "LUFT reference: %s MD5=%s genozip_version=%u\n", ref_get_filename (gref), digest_display (ref_get_file_md5 (gref)).s, ref_get_genozip_version (gref));
    }

    else if (flag.reference == REF_EXTERNAL || flag.reference == REF_EXT_STORE || flag.reference == REF_LIFTOVER) 
        bufprintf (evb, &stats, "Reference: %s MD5=%s genozip_version=%u\n", ref_get_filename (gref), digest_display (ref_get_file_md5 (gref)).s, ref_get_genozip_version (gref));

    if (Z_DT(DT_VCF)) 
        bufprintf (evb, &stats, "Samples: %u   ", vcf_header_get_num_samples());

    uint32_t num_used_ctxs=0;
    for_zctx 
        if (zctx->nodes.len || zctx->txt_len) num_used_ctxs++;

    if ((Z_DT(DT_SAM) || Z_DT(DT_BAM)) && z_has_gencomp)
        bufprintf (evb, &stats, "%ss: %s (PRIM component: %s DEPN component: %s)  Contexts: %u   Vblocks: %u x %u MB  Sections: %u\n", 
                   DTPZ (line_name), str_int_commas (z_file->num_lines).s, str_int_commas (gencomp_get_num_lines (SAM_COMP_PRIM)).s, 
                   str_int_commas (gencomp_get_num_lines (SAM_COMP_DEPN)).s, num_used_ctxs, 
                   z_file->num_vbs, (uint32_t)(segconf.vb_size >> 20), z_file->section_list_buf.len32);

    else if (z_is_dvcf)
        bufprintf (evb, &stats, "%ss: %s (Prim-only: %s Luft-only: %s)  Contexts: %u   Vblocks: %u x %u MB  Sections: %u\n", 
                   DTPZ (line_name), str_int_commas (z_file->num_lines).s, str_int_commas (gencomp_get_num_lines (VCF_COMP_PRIM_ONLY)).s, 
                   str_int_commas (gencomp_get_num_lines (VCF_COMP_LUFT_ONLY)).s, num_used_ctxs, 
                   z_file->num_vbs, (uint32_t)(segconf.vb_size >> 20), z_file->section_list_buf.len32);

    else
        bufprintf (evb, &stats, "%ss: %s   Contexts: %u   Vblocks: %u x %u MB  Sections: %u\n", 
                   DTPZ (line_name), str_int_commas (z_file->num_lines).s, num_used_ctxs, 
                   z_file->num_vbs, (uint32_t)(segconf.vb_size >> 20), z_file->section_list_buf.len32);

    if (Z_DT(DT_KRAKEN)) {
        int64_t dominant_taxid_count;
        rom dominant_taxid = ctx_get_snip_with_largest_count (KRAKEN_TAXID, &dominant_taxid_count);

        if (dominant_taxid_count != -1)
            bufprintf (evb, &stats, "Dominant TaxID: %s  %s: %s (%-5.2f%%)\n", dominant_taxid, DTPZ (line_name),
                       str_int_commas (dominant_taxid_count).s, 100.0 * (float)dominant_taxid_count / (float)z_file->num_lines); 
        else
            bufprint0 (evb, &stats, "Dominant TaxID: No dominant species\n"); 
    }  
    
    else if (kraken_is_loaded) {
        bufprint0 (evb, &stats, "Features: Per-line taxonomy ID data\n");
        bufprint0 (evb, &features, "taxonomy_data%3B");
    }
    
    else if (Z_DT(DT_CHAIN) && flag.reference == REF_MAKE_CHAIN && !segconf.chain_mismatches_ref)
        bufprint0 (evb, &stats, "Features: Chain file suitable for use with genozip --chain\n");

    if (Z_DT(DT_VCF)) {
        if (z_is_dvcf) { 
            bufprintf (evb, &stats, "Features: Dual-coordinates: Main VBs: %u Prim-only VBs: %u Luft-only VBs: %u\n", 
                       sections_get_num_vbs(VCF_COMP_MAIN), sections_get_num_vbs(VCF_COMP_PRIM_ONLY), sections_get_num_vbs(VCF_COMP_LUFT_ONLY));
            bufprint0 (evb, &features, "DVCF%3B");
        }
    }

    if (Z_DT(DT_SAM) || Z_DT(DT_BAM)) {
        if (segconf.sam_is_sorted) {
            bufprint0 (evb, &stats, "Sorting: Sorted by POS\n");
            bufprint0 (evb, &features, "Sorted%3B");
        }
        
        if (segconf.sam_is_collated) {
            bufprint0 (evb, &stats, "Sorting: Collated by QNAME\n");
            bufprint0 (evb, &features, "Collated%3B");
        }
        
        if (!segconf.sam_is_sorted && !segconf.sam_is_collated) {
            bufprint0 (evb, &stats, "Sorting: Not sorted or collated\n");
            bufprint0 (evb, &features, "Not_sorted_or_collated%3B");
        }
        
        if (z_has_gencomp) { 
            bufprintf (evb, &stats, "Main VBs: %u Prim VBs: %u Depn VBs: %u\n", 
                       sections_get_num_vbs(SAM_COMP_MAIN), sections_get_num_vbs(SAM_COMP_PRIM), sections_get_num_vbs(SAM_COMP_DEPN));
            bufprint0 (evb, &features, "Has_gencomp%3B");
        }
    }

    if (Z_DT(DT_FASTA)) {
        bufprintf (evb, &stats, "Sequence type: %s\n", segconf.seq_type==SQT_AMINO ? "Amino acids" : "Nucleotide bases");
        bufprint0 (evb, &features, segconf.seq_type==SQT_AMINO ? "Amino_acids%3B" : "Nucleotide_bases%3B");
    }

    if (segconf.qname_flavor) {
        bufprintf (evb, &stats, "Read name style: %s%s%s\n", 
                   qf_name(segconf.qname_flavor), segconf.qname_flavor2 ? " + " : "", segconf.qname_flavor2 ? qf_name(segconf.qname_flavor2) : "");
        bufprintf (evb, &features, "Flavor=%s%s%s%%3B", 
                   qf_name(segconf.qname_flavor), segconf.qname_flavor2 ? "+" : "", segconf.qname_flavor2 ? qf_name(segconf.qname_flavor2) : "");
    }
    else if (Z_DT(DT_FASTQ) || Z_DT(DT_BAM) || Z_DT(DT_SAM) || Z_DT(DT_KRAKEN))
        bufprint0 (evb, &features, "Flavor=unrecognized%3B");   

    bufprintf (evb, &stats, "Genozip version: %s %s\nDate compressed: %s\n", 
               GENOZIP_CODE_VERSION, arch_get_distribution(), str_time().s);

    bufprint0 (evb, &stats, "Command line: ");
    buf_add_string (evb, &stats, flags_command_line()); // careful not to use bufprintf with command_line as it can exceed the maximum length in bufprintf

    bufprintf (evb, &stats, "\n%s\n", license_get_one_line());
    
    *BAFTc(features) = '\0';
}

static DESCENDING_SORTER (stats_sort_by_z_size, StatsByLine, z_size)

static void stats_consolidate_ctxs (StatsByLine *sbl, unsigned num_stats)
{
    for (unsigned parent=0; parent < num_stats; parent++) 

        // case: we might consolidate to this context
        if (sbl[parent].my_did_i != DID_I_NONE && sbl[parent].st_did_i == DID_I_NONE)  {
            for (unsigned child=0; child < num_stats; child++) {

                if (sbl[parent].my_did_i  == sbl[child].st_did_i) {
                    sbl[parent].txt_len   += sbl[child].txt_len;
                    sbl[parent].z_size    += sbl[child].z_size;  
                    sbl[parent].pc_of_txt += sbl[child].pc_of_txt;
                    sbl[parent].pc_of_z   += sbl[child].pc_of_z;  

                    if (flag.debug_stats)
                        iprintf ("Consolidated %s (did=%u) (txt_len=%"PRIu64" z_size=%"PRIu64") into %s (did=%u) (AFTER: txt_len=%"PRIu64" z_size=%"PRIu64")\n",
                                 sbl[child].name, sbl[child].my_did_i, sbl[child].txt_len, sbl[child].z_size, 
                                 sbl[parent].name, sbl[parent].my_did_i, sbl[parent].txt_len, sbl[parent].z_size);

                    sbl[child] = (StatsByLine){ .my_did_i = DID_I_NONE, .st_did_i = DID_I_NONE };
                }
            }

            if (!strcmp (sbl[parent].name, "SQBITMAP")) strcpy (sbl[parent].name, "SEQ"); // rename
        }
}

void stats_set_consolidation (VBlockP vb, DidIType parent, unsigned num_deps, ...)
{
    va_list args;
    va_start (args, num_deps);

    for (unsigned d=0; d < num_deps; d++) {
        DidIType dep = (DidIType)va_arg (args, int); 
        if (dep != parent) 
            CTX(dep)->st_did_i = parent;
    }

    CTX(parent)->is_stats_parent = true;
    
    va_end (args);
}

void stats_set_consolidation_(VBlockP vb, DidIType parent, unsigned num_deps, ContextP *dep_ctxs)
{
    for (unsigned d=0; d < num_deps; d++)
        if (dep_ctxs[d]->did_i != parent) 
            dep_ctxs[d]->st_did_i = parent;

    CTX(parent)->is_stats_parent = true;
}

void stats_set_consolidationN (VBlockP vb, DidIType parent, DidIType first_dep, unsigned num_deps)
{
    for (ContextP ctx=CTX(first_dep); ctx < CTX(first_dep + num_deps); ctx++)
        if (ctx->did_i != parent) 
            ctx->st_did_i = parent;

    CTX(parent)->is_stats_parent = true;
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
    for (unsigned i=0; i < num_stats; i++) 
        if (sbl[i].name[0] && !strcmp (consolidated_name, sbl[i].name)) {
            survivor = &sbl[i];
            break;
        }

    for (unsigned i=0; i < num_stats; i++) {
        
        if (!sbl[i].name[0]) continue; // unused entry

        for (unsigned d=0; d < num_deps; d++)
            if (!strcmp (deps[d], sbl[i].name)) {
                if (!survivor) {
                    survivor = &sbl[i]; // first found gets to be the survivor
                }
                else {
                    survivor->txt_len   += sbl[i].txt_len;
                    survivor->z_size    += sbl[i].z_size;  
                    survivor->pc_of_txt += sbl[i].pc_of_txt;
                    survivor->pc_of_z   += sbl[i].pc_of_z; 
                    strcpy (survivor->name, consolidated_name); // rename only if at least one was consolidated

                    if (flag.debug_stats)
                        iprintf ("Consolidated %s (txt_len=%"PRIu64" z_size=%"PRIu64") into %s (AFTER: txt_len=%"PRIu64" z_size=%"PRIu64")\n",
                                 sbl[i].name, sbl[i].txt_len, sbl[i].z_size, survivor->name, survivor->txt_len, survivor->z_size);

                    sbl[i] = (StatsByLine){}; 
                }
                break;
            } 
    }

    va_end (args);
}

static void stats_output_stats (StatsByLine *s, unsigned num_stats, float src_comp_ratio, Codec codec,
                                int64_t all_txt_len, int64_t all_txt_len_0, int64_t all_z_size, float all_pc_of_txt, float all_pc_of_z, float all_comp_ratio)
{
    bufprintf (evb, &stats, "\nSections (sorted by %% of genozip file):%s\n", "");
    bufprintf (evb, &stats, "NAME                   GENOZIP      %%       TXT      %%   RATIO\n%s", "");

    for (uint32_t i=0; i < num_stats; i++, s++)
        if (s->z_size)
            bufprintf (evb, &stats, "%-20.20s %9s %5.1f%% %9s %5.1f%% %6.1fX\n", 
                       s->name, 
                       str_size (s->z_size).s, s->pc_of_z, // z size and % of total z that is in this line
                       str_size ((float)s->txt_len).s, s->pc_of_txt, // txt size and % of total txt which is in this line
                       (float)s->txt_len / (float)s->z_size); // ratio z vs txt

    if (src_comp_ratio != 1 && flag.show_stats_comp_i == COMP_NONE)
        bufprintf (evb, &stats, 
                   "GENOZIP vs %-9s %9s %5.1f%% %9s %5.1f%% %6.1fX\n", 
                   codec_name (codec),
                   str_size (all_z_size).s, all_pc_of_z, // total z size and sum of all % of z (should be 100)
                   str_size (all_txt_len_0 / src_comp_ratio).s, all_pc_of_txt, // total txt fize and ratio z vs txt
                   all_comp_ratio / src_comp_ratio);
    
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
                                int64_t all_txt_len, int64_t all_uncomp_dict, int64_t all_comp_dict, int64_t all_comp_b250, int64_t all_comp_data, 
                                int64_t all_z_size, float all_pc_of_txt, float all_pc_of_z, float all_comp_ratio)
{
#define PC(pc) ((pc==0 || pc>=10) ? 0 : (pc<1 ? 2:1))

    // add diagnostic info
    if (flag.show_stats_comp_i == COMP_NONE) {
        bufprintf (evb, &STATS, "\nSystem info: OS=%s cores=%u endianity=%s\n", 
                   arch_get_os(), arch_get_num_cores(), arch_get_endianity());
        bufprintf (evb, &STATS, "\nSections (sorted by %% of genozip file):%s\n", "");
    }

    bufprintf (evb, &STATS, "did_i Name              Parent            #Words  Snips-(%% of #Words)    Hash-table    uncomp      comp      comp      comp      comp       txt    comp   %% of   %% of  %s\n", "");
    bufprintf (evb, &STATS, "                                         in file   Dict  Local FailSton   Size Occp      dict      dict      b250     local     TOTAL             ratio    txt    zip%s\n", "");

    for (uint32_t i=0; i < num_stats; i++, s++)
        if (s->z_size)
            bufprintf (evb, &STATS, "%-2.2s    %-17.17s %-17.17s %6s  %4.*f%%  %4.*f%%  %4.*f%% %8s %3.0f%% %9s %9s %9s %9s %9s %9s %6.1fX %5.1f%% %5.1f%%\n", 
                       s->did_i.s, s->name, s->type, s->words.s, 
                       PC (s->pc_dict), s->pc_dict, PC(s->pc_in_local), s->pc_in_local, PC(s->pc_failed_singletons), s->pc_failed_singletons, 
                       s->hash.s, s->pc_hash_occupancy, // Up to here - these don't appear in the total
                       s->uncomp_dict.s, s->comp_dict.s, s->comp_b250.s, s->comp_data.s, str_size (s->z_size).s, 
                       str_size ((float)s->txt_len).s, (float)s->txt_len / (float)s->z_size, s->pc_of_txt, s->pc_of_z);

    // note: no point showing this per component in SAM and DVCF, bc all_txt_len_0 is fully accounted for in the MAIN component and it is 0 in the others
    if (!(z_has_gencomp && flag.show_stats_comp_i != COMP_NONE))
        bufprintf (evb, &STATS, "TOTAL                                                                               "
                "%9s %9s %9s %9s %9s %9s %6.1fX %5.1f%% %5.1f%%\n", 
                str_size (all_uncomp_dict).s, str_size (all_comp_dict).s, str_size (all_comp_b250).s, 
                str_size (all_comp_data).s,   str_size (all_z_size).s,    str_size (all_txt_len).s, 
                all_comp_ratio, all_pc_of_txt, all_pc_of_z);
}

// generate the stats text - all sections except genozip header and the two stats sections 
void stats_generate (void) // specific section, or COMP_NONE if for the entire file
{
    // initial allocation
    buf_alloc (evb, &stats, 0, 10000, char, 1, "stats"); stats.len = 0; // reset if used for previous file
    buf_alloc (evb, &STATS, 0, 10000, char, 1, "stats"); STATS.len = 0;
    buf_alloc (evb, &features, 0, 1000,  char, 1, "stats"); features.len  = 0;
    buf_alloc (evb, &hash_occ, 0, 1000,  char, 1, "stats"); features.len  = 0;
    
    if (flag.show_stats_comp_i == COMP_NONE) {
        stats_output_file_metadata();
        buf_copy (evb, &STATS, &stats, char,0,0, "STATS");
    }

    int64_t all_comp_dict=0, all_uncomp_dict=0, all_comp_b250=0, all_comp_data=0, all_z_size=0, all_txt_len=0;

    // prepare data
    #define NUM_SBL (NUM_SEC_TYPES + z_file->num_contexts + 2) // 2 for consolidated groups
    static Buffer sbl_buf = EMPTY_BUFFER;
    ARRAY_alloc (StatsByLine, sbl, NUM_SBL, true, sbl_buf, evb, "sbl");

    #define ST_NAME(st) (&st_name(st)[4]) // cut off "SEC_" 

    stats_get_compressed_sizes (sbl); // ctx sizes in ctx->local/dict/ctx.param, and other sections in sbl[st]->z_size.
    
    uint64_t z_size     = flag.show_stats_comp_i==COMP_NONE ? z_file->disk_so_far            : z_file->disk_so_far_comp           [flag.show_stats_comp_i];
    uint64_t txt_size   = flag.show_stats_comp_i==COMP_NONE ? z_file->txt_data_so_far_bind   : z_file->txt_data_so_far_bind_comp  [flag.show_stats_comp_i];
    uint64_t txt_size_0 = flag.show_stats_comp_i==COMP_NONE ? z_file->txt_data_so_far_bind_0 : z_file->txt_data_so_far_bind_0_comp[flag.show_stats_comp_i];
    StatsByLine *s = sbl;

    for (SectionType st=0; st < NUM_SEC_TYPES; st++, s++) { 

        if (st == SEC_DICT || st == SEC_B250 || st == SEC_LOCAL || st == SEC_COUNTS) continue; // these are covered by individual contexts

        s->txt_len    = st == SEC_TXT_HEADER  ? z_file->header_size : 0; // note: MAIN header only, ie excluding generated headers for DVCF
        s->type       = (st==SEC_REFERENCE || st==SEC_REF_IS_SET || st==SEC_REF_CONTIGS || st==SEC_REF_RAND_ACC || st == SEC_CHROM2REF_MAP || st==SEC_REF_IUPACS)
                      ? "SEQUENCE" : "Other"; // note: some contexts appear as "Other" in --stats, but in --STATS their parent is themself, not "Other"
        s->my_did_i   = s->st_did_i = DID_I_NONE;
        s->did_i.s[0] = s->words.s[0] = s->hash.s[0] = s->uncomp_dict.s[0] = s->comp_dict.s[0] = '-';
        s->pc_of_txt  = txt_size ? 100.0 * (float)s->txt_len / (float)txt_size : 0;
        s->pc_of_z    = z_size   ? 100.0 * (float)s->z_size  / (float)z_size   : 0;
        strcpy (s->name, ST_NAME (st)); 

        all_z_size    += s->z_size;
        all_comp_data += s->z_size;
        all_txt_len   += s->txt_len;
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
        all_comp_data   += zctx->local.count;
        all_z_size      += s->z_size;
        all_txt_len     += s->txt_len;

        if (Z_DT(DT_VCF) && dict_id_type(zctx->dict_id))
            sprintf (s->name, "%s/%s", dtype_name_z(zctx->dict_id), zctx->tag_name);
        else 
            strcpy (s->name, zctx->tag_name);

        // parent
        s->type  = zctx->dict_id.num                 == _SAM_SQBITMAP ? "SEQUENCE"
                 : zctx->st_did_i == DID_I_NONE                       ? zctx->tag_name
                 : ZCTX(zctx->st_did_i)->dict_id.num == _SAM_SQBITMAP ? "SEQUENCE"
                 :                                                     ZCTX(zctx->st_did_i)->tag_name;

        // note: each VB contributes local.len contains its b250.count if it has it, and local_num_len if not 
        float n_words = zctx->local.len;

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
        s->comp_data            = str_size (zctx->local.count);
        s->pc_of_txt            = txt_size ? 100.0 * (float)s->txt_len / (float)txt_size : 0;
        s->pc_of_z              = z_size   ? 100.0 * (float)s->z_size  / (float)z_size   : 0;
    
        zctx->b250.count = zctx->dict.count = zctx->local.count = zctx->txt_len = 0; // reset

        s++; // increment only if it has some data, otherwise already continued
    }

    unsigned num_stats = s - sbl;

    // note: for txt size and compression ratio in the TOTAL line (all_comp_ratio) we use txt_data_so_far_bind_0 
    // (the original txt data size) and not all_txt_size (size after ZIP modifications like --optimize). 
    // Therefore, in case of ZIP-modified txt, the sum of the (modified) fields in the TXT column will NOT equal the
    // TOTAL in the TXT column. That's ok.
    float all_comp_ratio = (float)txt_size_0 /* without modifications */ / (float)all_z_size;
    float all_pc_of_txt  = txt_size ? 100.0 * (float)all_txt_len / (float)txt_size : 0 /* with modifications */;
    float all_pc_of_z    = z_size   ? 100.0 * (float)all_z_size  / (float)z_size   : 0;

    // long form stats from --STATS    
    qsort (sbl, num_stats, sizeof (sbl[0]), stats_sort_by_z_size);  // sort by compressed size

    stats_output_STATS (sbl, num_stats, 
                        all_txt_len, all_uncomp_dict, all_comp_dict, all_comp_b250, all_comp_data, all_z_size, all_pc_of_txt, all_pc_of_z, all_comp_ratio);

    // calculate hash occupancy before consolidating stats
    stats_submit_calc_hash_occ (sbl, num_stats);

    // consolidates stats of child contexts into the parent one
    stats_consolidate_ctxs (sbl, num_stats);
    
    stats_consolidate_non_ctx (sbl, num_stats, 
                               flag.reference == REF_EXT_STORE ? "Reference" : "SEQ", // when compressing SAM/FASTQ with REF_EXT_STORE, account for the reference in its own "Parent"
                               6, ST_NAME (SEC_REFERENCE), ST_NAME (SEC_REF_IS_SET), 
                               ST_NAME (SEC_REF_CONTIGS), ST_NAME (SEC_REF_RAND_ACC), ST_NAME (SEC_CHROM2REF_MAP),
                               ST_NAME (SEC_REF_IUPACS));

    stats_consolidate_non_ctx (sbl, num_stats, "Other", 22 + (DTPZ(txt_header_required) == HDR_NONE), "E1L", "E2L", "EOL", 
                               "SAMPLES", "AUX", TOPLEVEL, "ToPLUFT", "TOP2BAM", "TOP2FQ", "TOP2FQEX", "TOP2VCF", "TOP2HASH", 
                               "LINEMETA", "CONTIG", "COORDS", "SAGROUP", "SAALN",
                               ST_NAME (SEC_RANDOM_ACCESS), ST_NAME (SEC_DICT_ID_ALIASES), ST_NAME (SEC_RECON_PLAN),
                               ST_NAME (SEC_VB_HEADER), ST_NAME (SEC_BGZF), ST_NAME(SEC_TXT_HEADER)/*must be last*/);
    
    ASSERTW (all_txt_len == txt_size || flag.make_reference, // all_txt_len=0 in make-ref as there are no contexts
             "Expecting all_txt_len=%"PRId64" == txt_size=%"PRId64, all_txt_len, txt_size);

    // short form stats from --stats    
    qsort (sbl, num_stats, sizeof (sbl[0]), stats_sort_by_z_size);  // re-sort after consolidation

    // source compression, eg BGZF, against txt before any modifications
    float src_comp_ratio = (float)txt_size_0 / 
                           (float)((flag.bind != BIND_FQ_PAIR && txt_file->disk_size) ? txt_file->disk_size : z_file->txt_disk_so_far_bind); 

    stats_output_stats (sbl, num_stats, src_comp_ratio, txt_file->source_codec, all_txt_len, txt_size_0, all_z_size, all_pc_of_txt, all_pc_of_z, all_comp_ratio);
    
    // if we're showing stats of a single components - output it now
    if (flag.show_stats_comp_i != COMP_NONE) {
        iprintf ("\n\nComponent=%s:\n", comp_name (flag.show_stats_comp_i));
        buf_print (flag.show_stats == STATS_SHORT ? &stats : &STATS, false);
        buf_free (stats);
        buf_free (STATS);
    }

    // note: we use txt_data_so_far_bind is the sum of recon_sizes - see zip_update_txt_counters - which is
    // expected to be the sum of txt_len. However, this NOT the size of the original file which is stored in
    // z_file->txt_data_so_far_bind_0.
    ASSERTW (flag.show_stats_comp_i != COMP_NONE || all_txt_len == txt_size || flag.data_modified || flag.make_reference, 
             "Hmm... incorrect calculation for %s sizes: total section sizes=%s but file size is %s (diff=%d)", 
             dt_name (z_file->data_type), str_int_commas (all_txt_len).s, str_int_commas (txt_size).s, 
             (int32_t)(txt_size - all_txt_len)); 

    if (flag.show_stats_comp_i == COMP_NONE) {
        zfile_compress_section_data (evb, SEC_STATS, &stats);
        zfile_compress_section_data (evb, SEC_STATS, &STATS);

        // store stats overhead, for stats_display (because when stats_display runs, section list won't be accessible since it is already converted to SectionEntFileFormat)
        Section sec = sections_last_sec (SEC_STATS, false) - 1; // first stats section     
        stats.count = z_file->disk_so_far - sec->offset;
    }

    if (flag.show_stats_comp_i != COMP_NONE) 
        iprint0 ("\nNote: Components stats don't include global sections like SEC_DICT, SEC_REFERENCE etc\n");

    if (license_get_type() != LIC_TYPE_PAID && !flag.debug && !getenv ("GENOZIP_TEST"))
        stats_submit (sbl, num_stats, all_txt_len, src_comp_ratio, all_comp_ratio); 

    buf_free (sbl_buf);
}

void stats_display (void)
{
    BufferP buf = flag.show_stats == 1 ? &stats : &STATS;

    if (!buf_is_alloc (buf)) return;  // no stats available

    buf_print (buf , false);

    if (z_file->disk_size < (1<<20) && command==ZIP)  // no need to print this note if total size > 1MB, as the ~2K of overhead is rounded off anyway
        // stats text doesn't include SEC_STATS and SEC_GENOZIP_HEADER - the last 3 sections in the file - since stats text is generated before these sections are compressed
        iprintf ("\nNote: ZIP total file size excludes overhead of %s\n", str_size (stats.count).s);

    iprint0 ("\n");
}

void stats_read_and_display (void)
{
    Section sec = sections_last_sec (SEC_STATS, true);
    if (!sec) {
        iprint0 ("No stats available for this file.\n"); // possibly the file was compressed with --cstats or --CSTATS
        return; // genozip file does not contain stats sections (SEC_STATS was introduced in v 7.0.5)
    }

    // read and uncompress the requested stats section
    zfile_get_global_section (SectionHeader, sec - (flag.show_stats==1),
                              flag.show_stats == 1 ? &stats : &STATS, "stats");
    
    stats_display();

    if (exe_type == EXE_GENOCAT) exit_ok(); // if this is genocat - we're done
}

// concatenate txt names of bound files so we can show them all
void stats_add_txt_name (rom fn)
{
    bufprintf (evb, &z_file->bound_txt_names, "%s%s", z_file->bound_txt_names.len ? " ": "", fn);
    z_file->bound_txt_names.count++; // number of files
}
