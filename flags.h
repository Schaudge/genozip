// ------------------------------------------------------------------
//   flags.h
//   Copyright (C) 2019-2021 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#ifndef FLAGS_INCLUDED
#define FLAGS_INCLUDED

#include "genozip.h"

typedef enum { 
    REF_NONE,      // ZIP (except SAM) and PIZ when user didn't specify an external reference
    REF_INTERNAL,  // ZIP SAM only: use did not specify an external reference - reference is calculated from file(s) data
    REF_EXTERNAL,  // ZIP & PIZ: user specified --reference
    REF_EXT_STORE, // ZIP: user specified --REFERENCE
    REF_STORED,    // PIZ: file contains REFERENCE sections (user cannot specify --reference)
    REF_LIFTOVER,  // ZIP: user specified --liftover
} ReferenceType;

typedef struct {
    
    // genozip options that affect the compressed file
    int fast, make_reference, multifasta, md5;
    char *vblock;
    
    // ZIP: data modifying options
    int optimize, optimize_sort, optimize_PL, optimize_GL, optimize_GP, optimize_VQSLOD,  // optimize flags
        optimize_QUAL, optimize_Vf, optimize_ZM, optimize_DESC;
    
    #define NOT_PAIRED_END 0 
    #define PAIR_READ_1    1
    #define PAIR_READ_2    2
    int pair; // unfortunately we can't rely on enum being sizeof(int)

    // genounzip options
    #define FLAG_BGZF_BY_ZFILE -1
    int bgzf;   // can be set by --bgzf, or by various other conditions. values 0-12 indicate the level of libdeflate, FLAG_BGZF_BY_ZFILE means use SEC_BGZF or default level if it is absent
    
    int out_dt; // used to indicate the desired dt of the output txt - consumed by file_open, and thereafter equal to txt_file->data_type

    // PIZ: data-modifying genocat options for showing only a subset of the file, or otherwise modify the file 
    int header_one, header_only_fast, no_header, header_only, // how to handle the txt header
        regions, samples, drop_genotypes, gt_only, sequential, no_pg, interleave, luft, sort, unsorted,
        kraken_taxid, kraken_taxid_negative;
    int64_t lines_first, lines_last; // set by --lines 
    char *grep;
    uint32_t one_vb, one_component, downsample, shard ;
    enum { SAM_FLAG_INCLUDE_IF_ALL=1, SAM_FLAG_INCLUDE_IF_NONE, SAM_FLAG_EXCLUDE_IF_ALL } sam_flag_filter;
    enum { SAM_MAPQ_INCLUDE_IF_AT_LEAST=1, SAM_MAPQ_EXCLUDE_IF_AT_LEAST } sam_mapq_filter;
    uint16_t FLAG; // the value for sam_flag_filter
    uint8_t MAPQ;  // the value for sam_mapq_filter

    // genols options
    int bytes;

    // options affecting the software interaction (but not the file contents)
    int force, quiet, 
        to_stdout,   // redirect txt output upon decompression to stdout 
        replace, 
        do_register,
        lic_width,   // width of license output, 0=dynamic (undocumented parameter of --license)
        test,        // implies md5
        index_txt,   // create an index
        list;        // a genols option
    char *threads_str, *out_filename;

    ReferenceType reference;

    // stats / metadata flags for end users
    int show_stats; 
    enum { VLD_NONE, VLD_REPORT_INVALID, VLD_REPORT_VALID, VLD_INVALID_FOUND } validate; // genocat: tests if this is a valid genozip file (z_file opens correctly)
    
    // analysis
    int list_chroms, show_sex, idxstats, count; 
    enum { COV_NONE, COV_ALL, COV_CHROM, COV_ONE } show_coverage;

    // stats / debug useful mostly for developers
    int show_memory, show_dict, show_b250, show_aliases, show_digest, show_recon_plan,
        show_index, show_gheader, show_ref_contigs, show_chain_contigs, show_ref_seq,
        show_reference, show_ref_hash, show_ref_index, show_ref_alts, show_chain,
        show_codec, show_containers, show_alleles, show_bgzf, show_txt_contigs,
        show_vblocks, show_threads, show_kraken, show_uncompress,
        debug_progress, show_hash, debug_memory, debug_threads,
        seg_only, xthreads, show_flags,
        echo,    // show the command line in case of an error
        show_headers; // (1 + SectionType to display) or 0=flag off or -1=all sections
    char *help, *dump_section, *show_is_set, *show_time, *show_mutex;

    DictId dict_id_show_one_b250,   // argument of --show-b250-one
           show_one_counts,
           dump_one_b250_dict_id,   // argument of --dump-b250-one
           dump_one_local_dict_id;  // argument of --dump-local-one
    char *show_one_dict;    // argument of --show-dict-one

    // internal flags set by the system, not the command line
    bool debug,              // set if DEBUG is defined
         ref_use_aligner,    // ZIP: compression requires using the aligner
         const_chroms,       // ZIP: chroms dictionary created from reference or file header and no more chroms can be added
         reading_reference,  // system is currently reading a reference  as a result of --chain (not normal PIZ of a .chain.genozip)
         trans_containers,   // PIZ: decompression invokes container translators
         processing_rejects, // ZIP & PIZ: currently zipping liftover rejects file / component
         genocat_no_ref_file,// PIZ (genocat): we don't need to load the reference data
         genocat_no_dicts,   // PIZ (genocat): we don't need to read the dicts
         genocat_no_reconstruct,  // PIZ: User requested to genocat with only metadata to be shown, not file contents
         no_writer,          // PIZ: User requested to genocat with only metadata to be shown, not file contents (but we still might do reconstruction without output)
         multiple_files,     // Command line includes multiple files
         reconstruct_as_src, // the reconstructed data type is the same as the source data type
         data_modified_by_txtheader,
         data_modified_by_reconstruction,
         data_modified_by_writer,
         vbs_may_be_dropped_by_piz_read_one_vb,
         data_modified,      // PIZ: output is NOT precisely identical to the compressed source, and hence we cannot use its BZGF blocks
                             // ZIP: txt data is modified during Seg
         may_drop_lines,     // PIZ: reconstruction is allowed to drop lines
         explicit_ref,       // ref_filename was set by --reference or --REFERENCE (as opposed to being read from the genozip header)
         dyn_set_mem,        // ZIP: we're now segging as part of zip_dynamically_set_max_memory()
         collect_coverage;   // PIZ: collect coverage data for show_sex/show_coverage/idxstats

#define flag_loading_auxiliary (flag.reading_reference || flag.reading_chain || flag.reading_kraken) // PIZ: currently reading auxiliary file (reference, chain etc)

    char *reading_chain;     // system is currently loading a chain file by this name
    char *reading_kraken;    // system is currently loading a kraken file by this name
    char *unbind;
    char *log_filename;      // output to info_stream goes here

    enum { BIND_NONE, BIND_ALL, BIND_PAIRS, BIND_REJECTS } bind; // ZIP: user used --output to bind all files or --pair without --output to bind every 2
    uint64_t stdin_size;
    unsigned longest_filename; // length of longest filename of the txt/z files on the command line

    // default max amount of txt data in each variant block. this is user-configurable with --vblock
    #define MAX_VBLOCK_MEMORY      2048         // in MB
    #define VBLOCK_MEMORY_MIN_DYN  (16   << 20) // VB memory - min/max when set in zip_dynamically_set_max_memory
    #define VBLOCK_MEMORY_MAX_DYN  (512  << 20) 
    #define VBLOCK_MEMORY_FAST     (16   << 20) // VB memory with --fast
    #define VBLOCK_MEMORY_MAKE_REF (1    << 20) // VB memory with --make-reference - reference data 
    #define VBLOCK_MEMORY_REFHASH  (16   << 20) // VB memory with --make-reference - refhash data (overridable with --vblock)
    #define VBLOCK_MEMORY_GENERIC  (16   << 20) // VB memory for the generic data type
    uint64_t vblock_memory;
} Flags;

extern Flags flag;

#define SAVE_FLAGS Flags save_flag = flag
#define RESTORE_FLAGS flag = save_flag

// save and reset flags that are intended to operate on the compressed file rather than the reference file
#define SAVE_FLAGS_AUX Flags save_flag = flag; \
    flag.test = flag.md5 = flag.show_memory = flag.show_stats= flag.no_header = flag.show_bgzf =\
    flag.header_one = flag.header_only = flag.regions = flag.show_index = flag.show_dict =  \
    flag.show_b250 = flag.show_ref_contigs = flag.list_chroms = flag.interleave = flag.count = \
    flag.downsample = flag.shard = flag.one_vb = flag.one_component = flag.xthreads= \
    flag.show_sex = flag.show_coverage = flag.idxstats = flag.collect_coverage = 0; /* int */ \
    flag.grep = flag.show_time = flag.unbind = flag.show_one_dict = flag.out_filename = NULL; /* char* */ \
    flag.dict_id_show_one_b250 = flag.dump_one_b250_dict_id = flag.dump_one_local_dict_id = DICT_ID_NONE; /* DictId */ 
    
#define SAVE_FLAG(f) typeof(flag.f) save_##f = flag.f 
#define TEMP_FLAG(f,value) SAVE_FLAG(f) ; flag.f=(typeof(flag.f))(uint64_t)value
#define SET_FLAG(f) SAVE_FLAG(f) ; flag.f=(typeof(flag.f))(uint64_t)1
#define CLEAR_FLAG(f) SAVE_FLAG(f) ; flag.f=(typeof(flag.f))(uint64_t)0
#define RESTORE_FLAG(f) flag.f = save_##f

// check for incompatabilities between flags
extern bool option_is_short[];
#define OT(l,s) option_is_short[(int)s[0]] ? "-"s : "--"l

extern void flags_init_from_command_line (int argc, char **argv);
extern void flags_update (unsigned num_files, const char **filenames);
extern void flags_update_zip_one_file (void);
extern void flags_update_piz_one_file (int z_file_i);

extern void flags_store_command_line (int argc, char **argv);
extern const BufferP flags_command_line (void);
extern void flags_display_debugger_params (void);
extern const char *flags_pipe_in_process_name (void);
extern unsigned flags_pipe_in_pid (void);
extern bool flags_pipe_in_process_died (void);

#endif