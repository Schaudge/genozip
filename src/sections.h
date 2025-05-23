// ------------------------------------------------------------------
//   sections.h
//   Copyright (C) 2019-2025 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#pragma once

#include "digest.h"
#include "local_type.h"

#define HEADER_IS(_st) (header->section_type == SEC_##_st)
#define ST(_st) (st == SEC_##_st)

// Section headers - integers are all big endian, unless mentioned otherwise

#define GENOZIP_MAGIC 0x27052012

#pragma pack(1) // structures that are part of the genozip format are packed.

// section headers are encoded in Big Endian (see https://en.wikipedia.org/wiki/Endianness)
// the reason for selecting big endian is that I am developing on little endian CPU (Intel) so
// endianity bugs will be discovered more readily this way

typedef packed_enum { STORE_NONE, STORE_INT, STORE_FLOAT, STORE_INDEX } StoreType; // values for SectionFlags.ctx.store
typedef packed_enum { B250_BYTES_4, B250_BYTES_3, B250_BYTES_2, B250_BYTES_1, // used in files 14 - 15.0.37
                      B250_VARL // used in files starting 15.0.39                                               
                    } B250Size; // part of the file format - goes into SectionHeaderCtx.b250_size

typedef packed_enum { SAG_NONE,     SAG_BY_SA, SAG_BY_NH, SAG_BY_SOLO, SAG_BY_CC, SAG_BY_FLAG, NUM_SAG_TYPES } SagType;
#define SAM_SAG_TYPE_NAMES { "NONE",   "BY_SA",   "BY_NH",   "BY_SOLO",   "BY_CC",   "BY_FLAG" }
#define IS_SAG_SA   (segconf.sag_type == SAG_BY_SA)
#define IS_SAG_NH   (segconf.sag_type == SAG_BY_NH)
#define IS_SAG_SOLO (segconf.sag_type == SAG_BY_SOLO)
#define IS_SAG_CC   (segconf.sag_type == SAG_BY_CC)
#define IS_SAG_FLAG (segconf.sag_type == SAG_BY_FLAG)

// goes into SectionHeader.flags and also SectionEnt.flags
typedef union SectionFlags {  
    uint8_t flags;

    struct FlagsGenozipHeader {
        // note: if updating dts_* flags, update in zfile_compress_genozip_header, sections_show_header too
        #define dts_ref_internal dt_specific // SAM: REF_INTERNAL was used for compressing (i.e. SAM file without reference) (introduced v6)
        #define v14_dts_paired   dt_specific // FASTQ: This z_file contains one or more pairs of FASTQs compressed with --pair (introduced v9.0.13 until v14, since v15 moved to TxtHeader)
        uint8_t dt_specific      : 1;  // this flag has a different meaning depending on the data_type, may be one of the above ^ 
        uint8_t aligner          : 1;  // SAM, FASTQ: our aligner may have used to align sequences to the reference (always with FASTQ if compressed with a reference, sometimes with SAM)
        uint8_t txt_is_bin       : 1;  // BAM: Source file is binary 
        uint8_t has_digest       : 1;  // true if compressed with digest (i.e. not --optimize etc) (v15) 
        #define v14_bgzf has_digest    // Up to v14: Reconstruct as BGZF (user may override) (determined by the last component) (since v15, determined by SectionHeaderTxtHeader.src_codec)
        uint8_t adler            : 1;  // true if Adler32 is used, false if MD5 is used 
        uint8_t has_gencomp      : 1;  // VCF: file supports dual coordinates - last two components are the "liftover rejects" data (v12)
                                       // SAM/BAM: PRIM and/or DEPN components exist (v14)
        uint8_t has_taxid        : 1;  // obsolete: each line in the file has Taxonomic ID information (v12 to 15.0.41)
        #define dts2_deep        dt_specific2 // SAM: this file is compressed with --deep (v15)
        uint8_t dt_specific2     : 1;  // this flag has a different meaning depending on the data_type, may be one of the above ^ (v15, "unused" up to v14)
    } genozip_header;

    struct FlagsTxtHeader {
        uint8_t pair             : 2;  // FASTQ component (inc. in a Deep SAM): PAIR_R1 or PAIR_R2 if this component is a paired FASTQ component (v15)
        #define v13_dvcf_comp_i pair   // v12-13: DVCF: 0=Main 1=Primary-only rejects 2=Luft-only rejects (in v14, this moved to SectionEnt.comp_i)
        uint8_t is_txt_luft      : 1;  // v12-15.0.41: is_txt_luft: DVCF: true if original source file was a dual-coordinates file in Luft rendition (v12)
        uint8_t no_gz_ext        : 1;  // source file was compressed with GZ/BGZF AND it did not have a .gz/.bgz extension (15.0.23) 
        uint8_t unused           : 4;
    } txt_header;

    union FlagsVbHeader {
        struct FlagsVbHeaderVcf {
            uint8_t coords             : 2; // DC_PRIMARY if it contains TOPLEVEL container, DC_LUFT if LUFT toplevel container, or DC_BOTH if both (used 12.0.0 - 15.0.41)
            uint8_t use_null_DP_method : 1; // "null_DP" method is used (13.0.5)
            uint8_t unused             : 5;
        } vcf;
        struct FlagsVbHeaderSam {
            uint8_t unused             : 2; 
            uint8_t v13_is_sorted      : 1; // Likely sorted (copied from segconf.is_sorted)     - introduced 13.0.3 and canceled v14
            uint8_t v13_is_collated    : 1; // Likely collated (copied from segconf.is_collated) - introduced 13.0.3 and canceled v14
            uint8_t unused2            : 4;
        } sam;
        struct FlagsVbHeaderGff {
            uint8_t embedded_fasta     : 1; // this VB consists embedded FASTA and has data_type=DT_FASTA (v15)
            uint8_t unused             : 7;
        } gff;
    } vb_header;

    struct FlagsMgzip {
        uint8_t OLD_has_eof_block: 1;  // PIZ: used for files up to 15.0.62
        #define implausible OLD_has_eof_block // ZIP: used for level discovery (not part of file format)
        uint8_t level            : 4;  // 0-12 for libdeflate or 0-9 for zlib level: 15 means unknown
        MgzipLibraryType library : 3;  // ignored if level=15 (introduced 9.0.16)
    } mgzip;

    struct FlagsCtx {
        StoreType store          : 2;  // after reconstruction of a snip, store it in ctx.last_value
        uint8_t paired           : 1;  // FASTQ VB (inc. Deep)
        #define r1_pair_identical paired // Meaning in an R1 section: pair-identical: this section should be used for both R1 and R2 if the corresponding R2 section missing (v15)
        #define r2_pair_assisted  paired // Meaning in an R2 section: pair-assisted: reconstruction of this context requires loading the corresponding R1 context to ctx->b250R1/localR1
        uint8_t store_delta      : 1;  // introduced v12.0.41: after reconstruction of a snip, store last_delta. notes: 1. last_delta also stored in case of a delta snip. 2. if using this, store=STORE_INT must be set.
        #define v13_copy_local_param spl_custom   // up to v13: copy ctx.b250/local.param from SectionHeaderCtx.param. since v14, piz always copies, except for LT_BITMAP
        uint8_t spl_custom       : 1;  // introduced v14: similar to store_per_line, but storing is done by the context's SPECIAL function, instead of in reconstruct_store_history
        uint8_t all_the_same     : 1;  // SEC_B250: the b250 data contains only one element, and should be used to reconstruct any number of snips from this context

        #define same_line        ctx_specific_flag // v13.0.5: Valid for contexts that use SNIP_OTHER_DELTA, SNIP_DIFF, SNIP_COPY(from other)(15.0.48), SPECIAL_MINUS(15.0.58), SPECIAL_PLUS(15.0.58), SPECIAL_DIVIDE_BY(15.0.58): if true, reconstructs gets the value in the line (whether before or after). if false, it gets the last value.
        #define no_textual_seq   ctx_specific_flag // v14.0.0: SAM_SQBITMAP: indicates that SEQ is NOT consumed by other fields, and therefore sam_piz_sam2bam_SEQ doesn't need to store textual_seq. 
        #define depn_clip_hard   ctx_specific_flag // v14.0.0: OPTION_SA_Z in SAM_COMP_MAIN: if true: depn lines, if their CIGAR has a clipping, it is hard clipping (H)
        #define lookback0_ok     ctx_specific_flag // v14.0.0: contexts that are items of a container with lookback. indicates that a SNIP_LOOKBACK when lookback=0 is not an error.
        #define trailing_zero_ok ctx_specific_flag // v14.0.17: INFO_QD: whether reconstruct with or without trailing zeros. 
        #define acgt_no_x        ctx_specific_flag // v15.0.13: contexts compressed with CODEC_ACGT: (NONREF and others): this context has no _X companion context
        uint8_t ctx_specific_flag: 1;  // v10.0.3: flag specific a context 

        uint8_t store_per_line   : 1;  // v12.0.41: store value or text for each line - in context->history        
    } ctx;

    struct FlagsDict {
        uint8_t deep_sam         : 1;  // v15: Deep: dictionary used by a SAM   
        uint8_t deep_fastq       : 1;  // v15: Deep: dictionary used by a FASTQ 
        #define MAX_ALL_THE_SAME_WI 15
        uint8_t all_the_same_wi  : 4;  // snip of this word_index to be reconstructed in no b250 and no local sections. can be non-zero (0-15) since 15.0.51.
        uint8_t unused           : 2;
    } dictionary;
    
    struct FlagsReconPlan { // used v12-15.0.63
        uint8_t unused1          : 1;  // v12-15.0.42: "luft: this section is for reconstructing the file in the Luft coordinates (introduced v12-15.0.42)"
        uint8_t frag_len_bits    : 4;  // 2^(frag_len_bits+17) is the maximum fragment length (v14) 
        uint8_t unused2          : 3;
    } recon_plan;

    struct FlagsRefContigs {
        uint8_t sequential_ref_index : 1;  // v14: first ref_contig has ref_index 0, second has 1 etc. ref_index set to 0 on disk and should be set by piz.
        uint8_t compacted            : 1;  // v14.0.10: for stored reference (not reference file): contigs stored as CompactContig
        uint8_t unused               : 6;
    } ref_contigs;

} SectionFlags __attribute__((__transparent_union__));

typedef struct FlagsMgzip FlagsMgzip;

#define SECTION_FLAGS_NONE ((SectionFlags){ .flags = 0 })

typedef struct SectionHeader {         // 28 bytes
    union {
    uint32_t     magic; 
    uint32_t     uncomp_adler32;       // used if --verify-codec is specified (a diagnostic)
    uint32_t     section_i;            // PIZ: section number in z_file->section_list. replaces magic (in memory only) after it is verified (15.0.66)
    };
    union {
    uint32_t     v14_compressed_offset;// up to v14: number of bytes from the start of the header that is the start of compressed data (sizeof header + header encryption padding) (32b up to v14, but upper 16b always 0)
    uint32_t     z_digest;             // Adler32 of the compressed/encrypted data of this section (v15)
    };
    uint32_t     data_encrypted_len;   // = data_compressed_len + padding if encrypted, 0 if not
    uint32_t     data_compressed_len;
    uint32_t     data_uncompressed_len;
    uint32_t     vblock_i;             // VB with in file starting from 1 ; 0 for non-VB sections
    SectionType  section_type;         // 1 byte
    Codec        codec;                // 1 byte - primary codec in which this section is compressed
    union { // 1 byte: section_type specific
    Codec        sub_codec;            // SEC_LOCAL : sub codec, in case primary codec invokes another codec
    uint8_t      dict_helper;          // SEC_DICT  : value passed to/from ctx->dict_helper (since 15.0.42).
    };
    SectionFlags flags;                // 1 byte
} SectionHeader, *SectionHeaderP; 

typedef struct {
    SectionHeader;
    uint8_t  genozip_version;
    EncryptionType encryption_type;    // one of EncryptionType
    uint16_t data_type;                // one of DataType
    uint64_t recon_size;               // data size of reconstructed file (note: for MAIN VBs in SAM gencomp, this excludes any PRIM/DEPN lines)
    uint64_t genozip_minor_ver : 14;   // populated since 15.0.28. 10 bits until 15.0.59
    uint64_t is_modified       : 1;    // ZIP modified the txt_data (e.g. --optimize). Since 15.0.60      
    uint64_t private_file      : 1;    // this file can only be decompressed by user with the specified license_hash (15.0.30)
    uint64_t num_lines_bound   : 48;   // number of lines in a bound file. "line" is data_type-dependent. For FASTQ, it is a read.
    uint32_t num_sections;             // number sections in this file (including this one)
    union {
        struct {                       // since v14
            uint16_t old_vb_size;      // v14 to 15.0.64: segconf.vb_size in MB (if not exact MB in zip - rounded up to nearest MB) (replaced by segconf_vb_size)
            char unused;                       
            CompIType num_txt_files;   // number of txt bound components in this file (1 if no binding). We don't count generated components (gencomp).
        };
        uint32_t v13_num_components;   // up to v13: number of txt bound components in this file (1 if no binding)
    };
    union { // 16 bytes
        Digest genome_digest;          // DT_REF: Digest of genome as loaded to memory (algorithm determined by genozip_header.adler) (since v15)
        Digest v14_REF_fasta_md5;      // DT_REF: MD5 of original FASTA file (v14, buggy)
        Digest FASTQ_v13_digest_bound; // DT_FASTQ: up to v13 "digest_bound": digest of concatenated pair of FQ (regarding other bound files in v13 - DVCF has digest 0, and other bound files are not reconstructable with v14+)    
    };
    #define PASSWORD_TEST "WhenIThinkBackOnAllTheCrapIlearntInHighschool"
    uint8_t  password_test[16];        // short encrypted block - used to test the validy of a password
#define FILE_METADATA_LEN 72
    char     created[FILE_METADATA_LEN];  // nul-terminated metadata
    Digest   license_hash;             // MD5(license_num)
#define REF_FILENAME_LEN 256
    union {
    char ref_filename[REF_FILENAME_LEN];   // external reference filename, nul-terimated. ref_filename[0]=0 if there is no external reference. DT_CHAIN (up to 15.0.41): LUFT reference filename.
    char fasta_filename[REF_FILENAME_LEN]; // DT_REF: fasta file used to generated this reference
    }; 
    union {
        Digest ref_genome_digest;      // Used if REF_EXTERNAL: SectionHeaderGenozipHeader.genome_digest of the reference file
        Digest refhash_digest;         // DT_REF: digest of refhash (v15)
    };
    union { // 271 bytes - data-type specific
        struct {
            // copied from their respective values in segconf, and copied back to segconf in PIZ
            DictId segconf_seq_len_dict_id;   // SAM: dict_id of one of the Q?NAME contexts (the "length=" item), which is expected to hold the seq_len for this read. 0 if there is no such item. v14.
            uint32_t segconf_std_seq_len;     // SAM: longest (up to 15.0.68: average) seq_len in segconf data. v14. (called sam_seq_len up to 15.0.68)
            SagType segconf_sag_type;         // SAM: v14
            uint8_t segconf_seq_len_cm;       // SAM: v14   
            uint8_t segconf_ms_type      : 3; // SAM: v14 
            uint8_t segconf_has_MD_or_NM : 1; // SAM: PIZ should call sam_analyze_copied_SEQ for dependent reads unless explicitly told not to. v14.
            uint8_t segconf_bisulfite    : 1; // SAM: v14
            uint8_t segconf_is_paired    : 1; // SAM: v14
            uint8_t segconf_sag_has_AS   : 1; // SAM: v14
            uint8_t segconf_pysam_qual   : 1; // SAM: v14
            uint8_t segconf_10xGen       : 1; // SAM: v14
            uint8_t segconf_SA_HtoS      : 1; // SAM: v14
            uint8_t segconf_is_sorted    : 1; // SAM: v14
            uint8_t segconf_is_collated  : 1; // SAM: v14
            uint8_t segconf_MD_NM_by_un  : 1; // SAM: v14
            uint8_t segconf_predict_meth : 1; // SAM: v14
            uint8_t segconf_deep_qname1  : 1; // SAM: v15
            uint8_t segconf_deep_qname2  : 1; // SAM: v15
            uint8_t segconf_deep_no_qual : 1; // SAM: v15
            uint8_t segconf_use_ins_ctxs : 1; // 15.0.30
            uint8_t segconf_MAPQ_use_xq  : 1; // 15.0.61 true if DRAGEN and has xq:i, since 15.0.69 also XQ:i
            uint8_t segconf_has_MQ       : 1; // 15.0.61
            uint8_t segconf_SA_CIGAR_abb : 1; // 15.0.66
            uint8_t segconf_SA_NM_by_X   : 1; // 15.0.66
            uint8_t segconf_CIGAR_has_eqx: 1; // 15.0.68
            uint8_t segconf_is_ileaved   : 1; // 15.0.69: Deep: FASTQ component is interleaved
            uint8_t segconf_sam_factor;       // 15.0.28: BAM only: 64X estimated blow-up factor of SAM txt_data vs BAM 
            uint8_t segconf_deep_N_fq_score;  // 15.0.39: Deep: when copying QUAL from SAM, update scores of 'N' bases to this value
            uint8_t unused_bits          : 8;
            uint16_t conc_writing_vbs;        // 15.0.64: max number of handed-over the VBs the writer will need to have open concurrent. Up to 15.0.63 this was in SectionHeaderReconPlan
            char unused[245];
        } sam;

        struct { 
            DictId segconf_seq_len_dict_id;   // FASTQ: dict_id of one of the Q?NAME contexts, which is expected to hold the seq_len for this read. 0 if there is no such item. copied from segconf.seq_len_dict_id. added v14.
            uint8_t segconf_fa_as_fq     : 1; // FASTQ: 15.0.58
            uint8_t segconf_is_ileaved   : 1; // FASTQ: 15.0.58
            uint8_t segconf_use_ins_ctxs : 1; // 15.0.30
            uint8_t unused_bits          : 5;
            uint8_t unused8[3];
            uint32_t segconf_std_seq_len;     // FASTQ: 15.0.69
            char unused[251];
        } fastq;

        struct {
            uint32_t segconf_has_RGQ      : 1; // VCF: copied from segconf.has[FORMAT_RGQ]. added v14.
            uint32_t segconf_GQ_method    : 3; // VCF: 15.0.37
            uint32_t segconf_FMT_DP_method: 2; // VCF: 15.0.37
            uint32_t segconf_del_svlen_is_neg : 1; // VCF: 15.0.48
            uint32_t segconf_sample_copy  : 1; // VCF: 15.0.69
            uint32_t max_ploidy_for_mux   : 8; // VCF: 15.0.36
            uint32_t segconf_MATEID_method: 3; // VCF: 15.0.48
            uint32_t segconf_INF_DP_method: 3; // VCF: 15.0.52
            uint32_t unused13             : 10;
            
            struct { // Note: number of bits must match argument of segconf_set_width()
                // when adding, also add to: 1.SegConf 2.vcf_seg_finalize_segconf 3.vcf_piz_genozip_header 4.vcf_zip_genozip_header 5.(maybe)vcf_piz_insert_field.dids
                uint64_t AC:3, MLEAC:3, AN:3, AF:3, SF:3, QD:3, DP:3;  // VCF: 15.0.37
                uint64_t AS_SB_TABLE : 4;      // VCF: 15.0.41
                uint64_t ID          : 6;      // VCF: 15.0.48
                uint64_t QUAL        : 4;      // VCF: 15.0.51
                uint64_t BaseCounts  : 5;      // VCF: 15.0.52
                uint64_t DPB         : 3;      // VCF: 15.0.61
                uint64_t unused      : 21;
            } width; 

            float segconf_Q_to_O;              // VCF: 15.0.61

            uint8_t unused[251];
        } vcf;
    };

    // ⇧ New non-data-type-specific fields grow upwards at the expense of unused[] ⇧ 
    uint32_t segconf_vb_size;          // in bytes (replaced old_vb_size). 15.0.65
    uint8_t lic_type;                  // LicenseType: license type used to create this file. 15.0.59.
} SectionHeaderGenozipHeader, *SectionHeaderGenozipHeaderP;
typedef const SectionHeaderGenozipHeader *ConstSectionHeaderGenozipHeaderP;

// this footer appears AFTER the genozip header data, facilitating reading the genozip header in reverse from the end of the file
typedef struct {
    uint64_t genozip_header_offset;
    uint32_t magic;
} SectionFooterGenozipHeader, *SectionFooterGenozipHeaderP;

typedef packed_enum { // these values and their order are part of the file format
                      CNN_NONE, CNN_SEMICOLON, CNN_COLON, CNN_UNDERLINE, CNN_HYPHEN, CNN_HASH, CNN_SPACE, CNN_PIPE, NUM_CNN
} QnameCNN /* 3 bits */;
#define CNN_TO_CHAR { 0,        ';',           ':',       '_',           '-',        '#',      ' ',       '|' }
#define CHAR_TO_CNN { [';']=CNN_SEMICOLON, [':']=CNN_COLON, ['_']=CNN_UNDERLINE, ['-']=CNN_HYPHEN, ['#']=CNN_HASH, [' ']=CNN_SPACE, ['|']=CNN_PIPE }

typedef struct { // 2 bytes
    uint8_t unused;                    // 15.0.0 to 15.0.25 : flavor_id (never used in PIZ)
    uint8_t has_seq_len  : 1;          // qname includes seq_len
    uint8_t is_consensus : 1;          // qname flavor is a "consensus read" flavor (15.0.26) (15.0.0 to 15.0.14 : is_mated: qname ends with /1 or /2 (never used in PIZ))
    uint8_t is_mated     : 1;          // qname's final two characters are separator + mate (1 or 2), eg "/1" or "|2" or ".1" (previously called "has_R")
    uint8_t cnn          : 3;          // QnameCNN: terminate before the last character that is this, to canonoize 
    uint8_t is_tokenized : 1;          // unrecognized flavor: qname is tokenized (15.0.63)
    uint8_t unused_bits  : 1;
} QnameFlavorProp;

// The text file header section appears once in the file (or multiple times in case of bound file), and includes the txt file header 
typedef struct {
    SectionHeader;
    uint64_t txt_data_size;            // number of bytes in the original txt file (without source compression, potentially after ZIP modifications eg --optimize)
    uint64_t txt_num_lines;            // number of data (non-header) lines in the original txt file. Concat mode: entire file for first SectionHeaderTxtHeader, and only for that txt if not first
    uint32_t max_lines_per_vb;         // upper bound on how many data lines a VB can have in this file
    Codec    src_codec;                // codec of original txt file (none, bgzf, gz, bz2, cram...)
    uint8_t  codec_info[3];            // codec specific info: for CODEC_BGZF, these are the LSB, 2nd-LSB, 3rd-LSB of the source BGZF-compressed file size
    Digest   digest;                   // digest of original single txt file. Up to 15.0.59: 0 if modified or DVCF. v14: only if md5, not alder32 (adler32 digest, starting v14, is stored per VB) (bug in v14: this field is garbage instead of 0 for FASTQ_COMP_FQR2 if adler32)
    Digest   digest_header;            // MD5 or Adler32 of header. Up to 15.0.59: 0 if txt was modified by zip.
#define TXT_FILENAME_LEN 256
    char     txt_filename[TXT_FILENAME_LEN];// filename of this single component. without path, 0-terminated. always in base form like .vcf or .sam, even if the original is compressed .vcf.gz or .bam
    uint64_t txt_header_size;          // size of header in original txt file before any zip-side modifications. note: use Σdata_uncompress_len(fragᵢ) for the size after zip-size modifications (v12)
    QnameFlavorProp flav_prop[NUM_QTYPES]; // SAM/BAM/FASTQ. properties of QNAME flavor (v15) 
} SectionHeaderTxtHeader, *SectionHeaderTxtHeaderP; 

typedef struct {
    SectionHeader;     
    union {
        uint32_t sam_prim_seq_len;     // SAM PRIM: total number of bases of SEQ in this VB (v14) 
        uint32_t vcf_HT_n_lines;       // VCF starting 15.0.48: number of lines that 1. have FORMAT/GT 2. Samples were segged (i.e. not copy-from-mate)
        uint32_t v11_first_line;       // up to v11 - first_line; if 0, this is the terminating section of the components
    };    
    union {
        uint32_t sam_prim_num_sag_alns;// SAM PRIM: number of alns (prim + depn) in SAGs this VB (v14)
        uint32_t v13_top_level_repeats;// v12/13: repeats of TOPLEVEL container in this VB. Up to v12 - called num_lines.
    };
    uint32_t recon_size;               // size of vblock as it appears in the default reconstruction (i.e. after ZIP-side modifications)
    uint32_t z_data_bytes;             // total bytes of this vblock in the genozip file including all sections and their headers 
    uint32_t longest_line_len;         // length of the longest line in this vblock 
    Digest   digest;                   // stand-alone Adler32 or commulative MD5 up to and including this VB. Up to 15.0.59: not if VB was modified. Up to v13: Adler32 was commulative too.
   
    struct { // SAM PRIM - 16 bytes
    uint32_t sam_prim_first_grp_i;     // SAM PRIM: the index of first group of this PRIM VB, in z_file->sag_grps (v14)
    uint32_t sam_prim_comp_qual_len;   // SAM PRIM: total size of SA Group's QUAL, as compressed in-memory in ZIP (v14)
    uint32_t sam_prim_comp_qname_len;  // SAM PRIM: total length of QNAME in this VB (v14). since 15.0.65: huffman-compressed length ; v14-15.0.64: uncompressed length
    union {
    uint32_t sam_prim_comp_cigars_len; // SAM PRIM SAG_BY_SA: total size of sag's CIGARs, as compressed in-memory in ZIP (i.e. excluding CIGARs stored in OPTION_SA_CIGAR.dict) (v14)
    uint32_t sam_prim_solo_data_len;   // SAM PRIM SAG_BY_SOLO: size of solo_data: the huffman-compressed length since 15.0.68, and the uncompressed length before
    };
    };

    uint32_t longest_seq_len;          // SAM / FASTQ: largest seq_len in this VB (v15)
} SectionHeaderVbHeader, *SectionHeaderVbHeaderP; 
typedef const SectionHeaderVbHeader *ConstSectionHeaderVbHeaderP;

// Format of the the payload of SEC_VB_HEADER for SAM/BAM with gencomp
#define MAX_PLAN_ITEM_LINES 32767 // if there are more than this number of consecutive lines from a specific comp_i, we will need more than one VbPlanItem
#define VB_PLAN_COMP_ZIP(comp_i) \
  ( vb->vb_plan.prev_comp_i==SAM_COMP_MAIN ? ((comp_i) == SAM_COMP_PRIM)  /* comp_i must be DEPN or PRIM */ \
  : vb->vb_plan.prev_comp_i==SAM_COMP_DEPN ? ((comp_i) == SAM_COMP_PRIM)  /* comp_i must be MAIN or PRIM */ \
  : /*          prev_comp_i==SAM_COMP_PRIM*/ ((comp_i) == SAM_COMP_DEPN)) /* comp_i must be MAIN or DEPN */ \

#define VB_PLAN_COMP_PIZ(comp, prev_comp_i) \
  (  prev_comp_i==SAM_COMP_MAIN ? ((comp)==0?SAM_COMP_DEPN : SAM_COMP_PRIM) \
  :  prev_comp_i==SAM_COMP_DEPN ? ((comp)==0?SAM_COMP_MAIN : SAM_COMP_PRIM) \
  :/*prev_comp_i==SAM_COMP_PRIM*/ ((comp)==0?SAM_COMP_MAIN : SAM_COMP_DEPN) )

typedef struct {
    uint16_t comp    : 1;  // see VB_PLAN_COMP
    uint16_t n_lines : 15; // ∈[0, MAX_PLAN_ITEM_LINES]; 
} VbPlanItem;

typedef struct {
    SectionHeader;           
    uint32_t num_snips;        // number of items in dictionary
    DictId   dict_id;           
} SectionHeaderDictionary, *SectionHeaderDictionaryP;

typedef struct {
    SectionHeader;           
    int64_t  nodes_param;      // an extra piece of data transferred to/from Context.counts_extra
    DictId   dict_id;           
} SectionHeaderCounts, *SectionHeaderCountsP;     

typedef struct {
    SectionHeader;           
    int64_t  param;            // an extra piece of data transferred to/from zctx->subdicts.param
    DictId   dict_id;           
} SectionHeaderSubDicts, *SectionHeaderSubDictsP;     

typedef struct {
    SectionHeader;
    DictId dict_id;   
} SectionHeaderHuffman, *SectionHeaderHuffmanP;

// used for SEC_LOCAL and SEC_B250
typedef struct {
    SectionHeader;
    LocalType ltype;           // SEC_LOCAL: goes into ctx->ltype - type of data of the ctx->local buffer
                               // SEC_250:   unused. up to 15.0.17 populated with ctx->ltype and copied in PIZ to ctx->ltype if the corresponding local section is absent
    uint8_t param;             // Three options: 1. goes into ctx->local.param. (in PIZ: until v13: if flags.copy_local_param. since v14: always, except if ltype=LT_BITMAP) 
                               //                2. given to comp_uncompress as a codec parameter
                               //                3. starting 9.0.11 for ltype=LT_BITMAP: number of unused bits in top bits word
    union {
    struct {
    B250Size b250_size : 3;    // SEC_B250: size of each b250 element (v14, 2 bits up to 15.0.37)
    uint8_t unused2    : 5;
    };
    uint8_t nothing_char;      // SEC_LOCAL for integer ltypes: if integer==max_int of its ltype, reconstruct this character instead. 0xff means ignore, 0 means fallback to the hard-coded logic used up to 15.0.37. see piz_uncompress_all_ctxs, reconstruct_from_local_int. (15.0.39)
    };
    uint8_t unused;
    DictId dict_id;           
} SectionHeaderCtx, *SectionHeaderCtxP;         

// two ways of storing a range:
// uncompacted - we will have one section, SEC_REFERENCE, containing the data, and first/last_pos containing the coordinates of this range
// compacting we will have 2 sections:
// - SEC_REF_IS_SET - containing a bitmap (1 bit per base), and chrom,first/last_pos containing the coordinates of this range
// - SEC_REFERENCE - containing the compacted range (i.e. excluding bases that have a "0" in the bitmap), 
//   with chrom,first_pos same as SEC_REF_IS_SET and last_pos set so that (last_pos-first_pos+1) = number of '1' bits in SEC_REF_IS_SET
// SEC_REFERENCE (in both cases) contains 2 bits per base, and SEC_REF_IS_SET contains 1 bit per location.
typedef struct {
    SectionHeader;
    PosType64 pos;             // first pos within chrom (1-based) of this range         
    PosType64 gpos;            // first pos within genome (0-based) of this range
    uint32_t num_bases;        // number of bases (nucleotides) in this range
    uint32_t chrom_word_index; // index in contexts[CHROM].word_list of the chrom of this reference range    
} SectionHeaderReference, *SectionHeaderReferenceP;

typedef struct {
    SectionHeader;
    uint8_t num_layers;        // total number of layers
    uint8_t layer_i;           // layer number of this section (0=base layer, with the most bits)
    uint8_t layer_bits;        // number of bits in layer
    uint8_t ffu;
    uint32_t start_in_layer;   // start index within layer
} SectionHeaderRefHash, *SectionHeaderRefHashP;

// SEC_RECON_PLAN, contains ar array of ReconPlanItem (used v14-15.0.63 for SAM ; v12-15.0.52 for DVCF)
typedef struct SectionHeaderReconPlan {
    SectionHeader;
    VBIType conc_writing_vbs;  // max number of concurrent VBs in possesion of the writer thread needed to execute this plan    
    uint32_t vblock_mb;        // size of vblock in MB
} SectionHeaderReconPlan, *SectionHeaderReconPlanP;

// plan flavors (3 bit)
typedef enum { PLAN_RANGE=0, PLAN_FULL_VB=2, PLAN_END_OF_VB=7, // these appear in SEC_RECON_PLAN (up to 15.0.63)
               PLAN_VB_PLAN=1, PLAN_INTERLEAVE=3, PLAN_TXTHEADER=4, PLAN_REMOVE_ME=5, PLAN_DOWNSAMPLE=6 } PlanFlavor;
#define PLAN_FLAVOR_NAMES { "RANGE", "VB_PLAN", "FULL_VB", "INTERLEAVE", "TXTHEADER", "REMOVE_ME", "DOWNSAMPLE", "END_OF_VB" }
typedef struct { // 12 bytes
    VBIType vb_i;               
    union {
        uint32_t start_line;   // used for RANGE
        uint32_t vb2_i;        // used for INTERLEAVE
        uint32_t comp_i;       // used for TXTHEADER
        uint32_t word2;        // generic access to the value
    }; 
    uint32_t num_lines : 29;   // used by RANGE, FULL_VB (writer only, not file), INTERLEAVE, DOWNSAMPLE, PLAN_VB. note: for PLAN_VB, includes gencomp lines. note: in v12/13 END_OF_VB, this field was all 1s.
    PlanFlavor flavor  : 3;  
} ReconPlanItem, *ReconPlanItemP; 

// SEC_GENCOMP (since 15.0.64) contains ar array of GencompSecItem, an entry per SAM/BAM MAIN VBs, in the order of MAIN VB absorption.
// For each MAIN VB, we record the number of lines from PRIM and DEPN VBs embedded in it. The location within the MAIN VB in which
// each PRIM/DEPN line needs to be embedded, is recorded in the payload the VbHeader section of each MAIN VB.
typedef struct { uint32_t vb_i, num_gc_lines[2]/*PRIM, DEPN*/; } GencompSecItem;

// the data of SEC_SECTION_LIST is an array of the following type, as is the z_file->section_list
typedef struct __attribute__ ((packed)) SectionEntFileFormat { // 19 bytes (since v15)
    uint32_t offset_delta;     // delta vs previous offset (always positive)
    int32_t vblock_i_delta;    // delta vs previous section's vblock_i (may be negative) 
    CompIType comp_i_plus_1;   // stores 0 if same as prev section, COMP_NONE if COMP_NONE, and otherwise comp_i+1.
    SectionType st;            // 1 byte
    union {                    // Section-Type-specific field
        DictId dict_id;        // used for dicted sections (see IS_DICTED_SEC) (first occurance of this dict_id)
        struct {               // used for dicted sections (2nd+ occurance of this dict_id)
            char is_dict_id;   // if zero, dict_id is copied from earlier section
            char unused3[3];
            uint32_t dict_sec_i; // section from which to copy the dict_id
        };
        struct { 
            uint32_t num_lines;// VB_HEADER sections - number of lines in this VB (SAM MAIN: excluding gencomp lines)
            uint32_t unused;   // this was "deep_num_lines" v15-15.0.64, but never accessed in piz
        };
        uint64_t st_specific;  // generic access to the value
    };
    SectionFlags flags;        // same flags as in section header (since v12, previously "unused")
} SectionEntFileFormat;

typedef struct SectionEntFileFormatV14 { // 24 bytes (used up to v14)
    uint64_t offset;           // offset of this section in the file
    union {                    // Section-Type-specific field
        DictId dict_id;        // DICT, LOCAL, B250 or COUNT sections
        struct { 
            uint32_t num_lines;// VB_HEADER sections - number of lines in this VB. 
            uint32_t unused;
        };
        uint64_t st_specific;  // generic access to the value
    };
    VBIType vblock_i;          // 1-based
    SectionType st;            // 1 byte
    SectionFlags flags;        // same flags as in section header (since v12, previously "unused")
    uint8_t comp_i  : 2;       // used for component-related sections, 0-based (since v14, previously "unused"), (0 if COMP_NONE)
    uint8_t unused1 : 6;
    uint8_t unused2;         
} SectionEntFileFormatV14;     // up to v14 

// the data of SEC_RANDOM_ACCESS is an array of the following type, as is the z_file->ra_buf and vb->ra_buf
// we maintain one RA entry per vb per every chrom in the the VB
typedef struct RAEntry {
    VBIType vblock_i;          // the vb_i in which this range appears
    WordIndex chrom_index;     // before merge: node index into chrom context nodes, after merge - word index in CHROM dictionary
    PosType64 min_pos, max_pos;// POS field value of smallest and largest POS value of this chrom in this VB (regardless of whether the VB is sorted)
} RAEntry; 

// the data of SEC_REF_IUPACS (added v12)
typedef struct Iupac {
    PosType64 gpos;
    char iupac;
} Iupac;

typedef union {
    SectionHeader common;
    SectionHeaderGenozipHeader genozip_header;
    SectionHeaderTxtHeader txt_header;
    SectionHeaderVbHeader vb_header;
    SectionHeaderDictionary dict;
    SectionHeaderCounts counts;
    SectionHeaderSubDicts subdicts;
    SectionHeaderCtx ctx;
    SectionHeaderReference reference;
    SectionHeaderRefHash ref_hash;
    SectionHeaderReconPlan recon_plan;
    SectionHeaderHuffman huffman;
    char padding[sizeof(SectionHeaderGenozipHeader) + 15]; // SectionHeaderGenozipHeader is the largest, 15=crypt_max_padding_len()
} SectionHeaderUnion;

typedef union {
    SectionHeaderP common;
    SectionHeaderGenozipHeaderP genozip_header;
    SectionHeaderTxtHeaderP txt_header;
    SectionHeaderVbHeaderP vb_header;
    SectionHeaderDictionaryP dict;
    SectionHeaderCountsP counts;
    SectionHeaderSubDictsP subdicts;
    SectionHeaderCtxP ctx;
    SectionHeaderReferenceP reference;
    SectionHeaderRefHashP ref_hash;
    SectionHeaderReconPlanP recon_plan;
    SectionHeaderHuffmanP huffman;
} SectionHeaderUnionP __attribute__((__transparent_union__));

#pragma pack()

// in-memory section 
typedef const struct SectionEnt {
    uint64_t offset;            // offset of this section in the file
    union {                     // Section-Type-specific field
        DictId dict_id;         // DICT, LOCAL, B250 or COUNT sections
        struct {
            uint32_t num_lines; // VB_HEADER sections - number of lines in this VB (SAM MAIN: excluding gencomp lines).
            uint32_t unused;    // this was "deep_num_lines" v15-15.0.63, but never accessed in piz
        };
        struct { 
            char is_dict_id;    // if zero, dict_id is copied from dict_sec_i (dict_id.id[0] cannot be zero = a DTYPE_FIELD dict_id cannot start with '@')
            char unused3[3];
            uint32_t dict_sec_i;// section from which to copy the dict_id
        };
        uint64_t st_specific;   // generic access to the value
    };
    VBIType vblock_i;           // 1-based
    uint32_t size;        
    CompIType comp_i;           // 0-based. 255 if COMP_NONE.
    SectionType st;             // 1 byte
    SectionFlags flags;         // same flags as in section header, since v12 (before was "unused")
} SectionEnt;

// alias types in SEC_DICT_ID_ALIASES (since v15)
typedef packed_enum { ALIAS_NONE, ALIAS_CTX, ALIAS_DICT } AliasType;
#define ALIAS_TYPE_NAMES              { "NONE",     "CTX",     "DICT"    }

// ---------
// ZIP stuff
// ---------

extern void sections_add_to_list (VBlockP vb, ConstSectionHeaderP header);
extern void sections_remove_from_list (VBlockP vb, uint64_t offset, uint64_t len);
extern void sections_list_concat (BufferP section_list);

// ---------
// PIZ stuff
// ---------

extern Section section_next (Section sec);

extern Section sections_first_sec (SectionType st, FailType soft_fail);
extern Section sections_last_sec (SectionType st, FailType soft_fail);
extern bool sections_next_sec3 (Section *sl_ent, SectionType st1, SectionType st2, SectionType st3);
#define sections_next_sec(sl_ent,st)       sections_next_sec3((sl_ent),(st),SEC_NONE,SEC_NONE)
#define sections_next_sec2(sl_ent,st1,st2) sections_next_sec3((sl_ent),(st1),(st2),SEC_NONE)

extern bool sections_prev_sec2 (Section *sl_ent, SectionType st1, SectionType st2);
#define sections_prev_sec(sl_ent,st) sections_prev_sec2((sl_ent),(st),SEC_NONE)

extern uint32_t sections_count_sections_until (BufferP sec_list, SectionType st, Section first_sec, SectionType until_encountering);
#define sections_count_sections(st) sections_count_sections_until (&z_file->section_list, (st), 0, SEC_NONE)

extern void sections_create_index (void);
extern Section sections_vb_header (VBIType vb_i);
extern Section sections_vb_last_section (VBIType vb_i);
extern VBIType sections_get_num_vbs (CompIType comp_i);
extern VBIType sections_get_num_vbs_(CompIType first_comp_i, CompIType last_comp_i);
extern VBIType sections_get_num_vbs_up_to (CompIType comp_i, VBIType max_vb_i);
extern VBIType sections_get_first_vb_i (CompIType comp_i);
extern Section sections_get_comp_txt_header_sec (CompIType comp_i);
extern uint32_t sections_get_recon_plan (Section *recon_sec);
extern Section sections_get_comp_bgzf_sec (CompIType comp_i);
extern Section sections_get_next_vb_header_sec (CompIType comp_i, Section *vb_sec);
extern bool is_there_any_section_with_dict_id (DictId dict_id);
extern SectionEnt *section_get_section (VBIType vb_i, SectionType st, DictId dict_id);
extern uint32_t sections_txt_header_get_num_fragments (void);

extern void sections_reading_list_add_vb_header (VBIType vb_i);
extern void sections_reading_list_add_txt_header (CompIType comp_i);

extern void sections_list_memory_to_file_format (void);
extern void sections_list_file_to_memory_format (SectionHeaderGenozipHeaderP genozip_header);
extern bool sections_is_paired (void);

extern void sections_set_show_headers (char *name);
extern uint32_t st_header_size (SectionType sec_type);

// display functions
#define sections_read_prefix(P_prefix) ((P_prefix) ? 'P' : flag_loading_auxiliary ? 'L' : 'R')
extern void sections_show_header (ConstSectionHeaderP header, VBlockP vb, CompIType comp_i, uint64_t offset, char rw);
extern void noreturn genocat_show_headers (rom z_filename);
typedef struct { char s[128]; } FlagStr;
extern FlagStr sections_dis_flags (SectionFlags f, SectionType st, DataType dt, bool is_r2);
extern void sections_show_gheader (ConstSectionHeaderGenozipHeaderP header);
extern void sections_show_section_list (DataType dt, BufferP section_list, SectionType only_this_st);
extern rom st_name (SectionType sec_type);
extern rom lt_name (LocalType lt);
extern rom store_type_name (StoreType store);
extern DictId sections_get_dict_id (ConstSectionHeaderP header);

extern StrText vb_name (VBlockP vb);
#define VB_NAME vb_name(VB).s

extern StrText line_name (VBlockP vb);
#define LN_NAME line_name(VB).s

extern StrText comp_name_(CompIType comp_i);
#define comp_name(comp_i) comp_name_(comp_i).s

#define IS_TXT_HEADER(sec) ((sec)->st == SEC_TXT_HEADER)
#define IS_VB_HEADER(sec)  ((sec)->st == SEC_VB_HEADER)
#define IS_DICT(sec)       ((sec)->st == SEC_DICT)
#define IS_LOCAL(sec)      ((sec)->st == SEC_LOCAL)
#define IS_B250(sec)       ((sec)->st == SEC_B250)

#define IS_DICTED_SEC(st) ((st)==SEC_B250 || (st)==SEC_LOCAL || (st)==SEC_DICT || (st)==SEC_COUNTS || (st) == SEC_SUBDICTS || (st) == SEC_HUFFMAN)
#define IS_VB_SEC(st)     ((st)==SEC_VB_HEADER || (st)==SEC_B250 || (st)==SEC_LOCAL)
#define IS_COMP_SEC(st)   (IS_VB_SEC(st) || (st)==SEC_TXT_HEADER || (st)==SEC_MGZIP || (st)==SEC_RECON_PLAN)
#define IS_FRAG_SEC(st)   ((st)==SEC_DICT || (st)==SEC_TXT_HEADER || (st)==SEC_RECON_PLAN || (st)==SEC_REFERENCE || (st)==SEC_REF_IS_SET || (st)==SEC_REF_HASH) // global sections fragmented with a dispatcher, and hence use vb_i 
