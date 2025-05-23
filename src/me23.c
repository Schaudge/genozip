// ------------------------------------------------------------------
//   me23.c
//   Copyright (C) 2020-2025 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#include "seg.h"
#include "piz.h"
#include "random_access.h"
#include "reconstruct.h"
#include "stats.h"
#include "codec.h"
#include "chrom.h"

//-----------------------------------------
// Header functions for 23andMe files
//-----------------------------------------

bool me23_header_inspect (VBlockP txt_header_vb, BufferP txt_header, struct FlagsTxtHeader txt_header_flags)
{
    SAFE_NULB (*txt_header);

    ASSINP (strstr (txt_header->data, "23andMe"), "file %s is missing a 23andMe header and thus not identified as a 23andMe file", 
            txt_name);

    SAFE_RESTORE;
    return true;
}

// detect if a generic file is actually a 23andMe file
bool is_me23 (STRp(header), bool *need_more)
{
    if (header_len < 1 || header[0] != '#' || !str_is_printable (STRa(header))) return false;

    char *newline = memchr (header, '\n', header_len);
    if (!newline) {
        *need_more = true; // we can't tell yet - need more data
        return false;
    }

    SAFE_NUL (newline);
    bool is_me32 = strstr (header, "23andMe"); // in first line
    SAFE_RESTORE;

    return is_me32;
}

//-----------------------------------------
// Segmentation functions for 23andMe files
//-----------------------------------------

void me23_seg_initialize (VBlockP vb)
{
    ctx_set_no_stons (vb, ME23_CHROM, ME23_POS, ME23_TOPLEVEL, ME23_TOP2VCF, DID_EOL);

    CTX(ME23_CHROM)->flags.store = STORE_INDEX; // since v12
    CTX(ME23_POS)->flags.store   = STORE_INT;   // since v12
    CTX(ME23_GENOTYPE)->ltype    = LT_BLOB;
}

void me23_seg_finalize (VBlockP vb)
{
    // top level snip
    SmallContainer top_level = { 
        .repeats   = vb->lines.len,
        .is_toplevel = true,
        .nitems_lo = 5,
        .items     = { { .dict_id = { _ME23_ID },       .separator = "\t" },
                       { .dict_id = { _ME23_CHROM },    .separator = "\t" },
                       { .dict_id = { _ME23_POS },      .separator = "\t" },
                       { .dict_id = { _ME23_GENOTYPE }                    },
                       { .dict_id = { _ME23_EOL },                        } }
    };

    container_seg (vb, CTX(ME23_TOPLEVEL), (ContainerP)&top_level, 0, 0, 0);

    SmallContainer top_level_to_vcf = { 
        .repeats   = vb->lines.len,
        .is_toplevel = true,
        .nitems_lo = 5,
        .items     = { { .dict_id = { _ME23_CHROM },    .separator = "\t" },
                       { .dict_id = { _ME23_POS },      .separator = "\t" },
                       { .dict_id = { _ME23_ID },       .separator = "\t" },
                       { .dict_id = { _ME23_GENOTYPE }, .separator = "\n", .translator = ME232VCF_GENOTYPE } }
    };

    container_seg (vb, CTX(ME23_TOP2VCF), (ContainerP)&top_level_to_vcf, 0, 0, 0);
}

bool me23_seg_is_small (ConstVBlockP vb, DictId dict_id)
{
    return dict_id.num == _ME23_TOPLEVEL ||
           dict_id.num == _ME23_TOP2VCF  ||
           dict_id.num == _ME23_CHROM    ||
           dict_id.num == _ME23_EOL;
}

rom me23_seg_txt_line (VBlockP vb, rom field_start_line, uint32_t remaining_txt_len, bool *has_13)     // index in vb->txt_data where this line starts
{
    rom next_field=field_start_line, field_start;
    unsigned field_len=0;
    char separator;

    int32_t len = BAFTtxt - field_start_line;

    GET_NEXT_ITEM (ME23_ID);
    seg_id_field (vb, CTX(ME23_ID), field_start, field_len, false, field_len+1);

    GET_NEXT_ITEM (ME23_CHROM);
    chrom_seg (vb, field_start, field_len);

    GET_NEXT_ITEM (ME23_POS);
    seg_pos_field (vb, ME23_POS, ME23_POS, 0, 0, field_start, field_len, 0, field_len+1);
    random_access_update_pos (vb, ME23_POS);

    // Genotype (a combination of one or two bases or "--")
    GET_LAST_ITEM (ME23_GENOTYPE);
    
    ASSSEG (field_len == 1 || field_len == 2, "expecting all genotype data to be 1 or 2 characters, but found one with %u: %.*s",
            field_len, field_len, field_start);

    seg_add_to_local_fixed (vb, CTX(ME23_GENOTYPE), field_start, field_len, LOOKUP_NONE, 0); 
        
    char lookup[2] = { SNIP_LOOKUP, '0' + field_len };
    seg_by_did (VB, lookup, 2, ME23_GENOTYPE, field_len + 1);

    SEG_EOL (ME23_EOL, false);
    
    return next_field;
}

//------------------------------------------------------------
// Translators for reconstructing 23andMe txxt into VCF format
//------------------------------------------------------------

// creates VCF file header
TXTHEADER_TRANSLATOR (txtheader_me232vcf)
{
    #define VCF_HEAD_1 "##FILTER=<ID=PASS,Description=\"All filters passed\">\n" \
                       "##FORMAT=<ID=GT,Number=1,Type=String,Description=\"Genotype\">\n" \
                       "##genozip_reference=%s\n"
                       
    #define VCF_HEAD_2 "##contig=<ID=%s,length=%"PRId64">\n"
    
    #define VCF_HEAD_3p1 "##fileformat=VCFv4.1\n" \
                         "##source=Genozip_v%s %s\n"  \
                         "##genozip_Command=\"" 
    
    #define VCF_HEAD_3p2 "#CHROM\tPOS\tID\tREF\tALT\tQUAL\tFILTER\tINFO\tFORMAT\t%.*s\n"
    
    // move the me23 and header to the side for a sec
    buf_move (comp_vb, comp_vb->scratch, "scratch", *txtheader_buf);
    ARRAY (char, header23, comp_vb->scratch);

    Context *ctx = ZCTX(ME23_CHROM);
    uint32_t num_chroms = ctx->word_list.len32;
    
    buf_alloc (comp_vb, txtheader_buf, 0, 
               1.3*comp_vb->scratch.len32 + STRLEN(VCF_HEAD_1) + STRLEN(VCF_HEAD_3p1) + STRLEN(VCF_HEAD_3p2) +80 + num_chroms * (STRLEN (VCF_HEAD_2) + 100), 
               char, 1, "txt_data");
    
    // add genozip stuff
    bufprintf (comp_vb, txtheader_buf, VCF_HEAD_3p1, code_version().s, GENOZIP_URL);
    buf_append_string (comp_vb, txtheader_buf, flags_command_line());
    bufprint0 (comp_vb, txtheader_buf, "\"\n");

    bufprintf (comp_vb, txtheader_buf, VCF_HEAD_1, ref_get_filename());
    
    // add contigs used in this file
    for (uint32_t chrom_i=0; chrom_i < num_chroms; chrom_i++) {
        
        // get contig length from loaded reference
        STR(chrom_name);
        ctx_get_snip_by_word_index (ctx, chrom_i, chrom_name);
        PosType64 contig_len = ref_contigs_get_contig_length (WORD_INDEX_NONE, chrom_name, chrom_name_len, true);

        bufprintf (comp_vb, txtheader_buf, VCF_HEAD_2, chrom_name, contig_len);
    }

    // add original 23andMe header, prefixing lines with "##co=" instead of "#"
    uint64_t header23_line_start = txtheader_buf->len;
    for (uint64_t i=0; i < comp_vb->scratch.len; i++) {
        if (header23[i] == '#') {
            header23_line_start = txtheader_buf->len;
            buf_add (txtheader_buf, "##co=", 5);
        }
        else
            BNXTc (*txtheader_buf) = header23[i];
    }
    txtheader_buf->len = header23_line_start; // remove last 23andme line ("# rsid chromosome position genotype")
    
    // attempt to get sample name from 23andMe file name
    char *sample_name = "Person"; // default
    unsigned sample_name_len = strlen (sample_name);
    
    #define ME23_FILENAME_BEFORE_SAMPLE "genome_"
    #define ME23_FILENAME_AFTER_SAMPLE  "_Full_"
    
    char *start, *after;
    if ((start = strstr (z_name, ME23_FILENAME_BEFORE_SAMPLE)) && 
        (after = strstr (start,  ME23_FILENAME_AFTER_SAMPLE ))) {
        start += sizeof ME23_FILENAME_BEFORE_SAMPLE -1;
        sample_name = start;;
        sample_name_len = after - start;
    }

    bufprintf (comp_vb, txtheader_buf, VCF_HEAD_3p2, sample_name_len, sample_name);

    buf_free (comp_vb->scratch);
}

// reconstruct VCF GENOTYPE field as VCF - REF,ALT,QUAL,FILTER,INFO,FORMAT,Sample
TRANSLATOR_FUNC (sam_piz_m232vcf_GENOTYPE)
{
    // Genotype length expected to be 2 or 1 (for MT, Y)
    ASSERT (recon_len==1 || recon_len==2, "bad recon_len=%u", recon_len);

    PosType64 pos = CTX(ME23_POS)->last_value.i;

    // chroms don't have the same index in the ME23 z_file and in the reference file - we need to translate chrom_index
    WordIndex save_chrom_node_index = vb->chrom_node_index;
    vb->chrom_node_index = ref_contigs_get_by_name (vb->chrom_name, vb->chrom_name_len, true, false);

    // get the value of the loaded reference at this position    
    ConstRangeP range = ref_piz_get_range (vb, HARD_FAIL);
    vb->chrom_node_index = save_chrom_node_index; // restore

    uint32_t idx = pos - range->first_pos;

    ASSPIZ (ref_is_idx_in_range (range, idx), "idx=%u but range has only %"PRIu64" nucleotides. pos=%"PRId64" range=%s", 
            idx, range->ref.nbits / 2, pos, ref_display_range (range).s);

    decl_acgt_decode;
    char ref_b = REF (idx);

    // get GENOTYPE from txt_data
    char b1 = recon[0];
    char b2 = (recon_len==2) ? recon[1] : 0;
    Ltxt -= recon_len; // rollback - we will reconstruct it differently

    
    if (b1 == '-' || b2 == '-' || // filter out variants if the genotype is not fully called
       (b1 == 'D' || b2 == 'D' || b1 == 'I' || b2 == 'I')) { // discard INDELs
        vb->drop_curr_line = "indel";
        return 0;
    }

    // REF
    RECONSTRUCT1 (ref_b);
    RECONSTRUCT1 ('\t');

    // ALT
    bool is_alt_1 = (b1 != ref_b) ;
    bool is_alt_2 = (b2 != ref_b) && (recon_len==2);
    int num_uniq_alts = is_alt_1 + is_alt_2 - (is_alt_1 && (b1==b2));

    switch (num_uniq_alts) {
        case 0: RECONSTRUCT1 ('.'); break; // no alt allele
        case 1: RECONSTRUCT1 (is_alt_1 ? b1 : b2); break;
        case 2: RECONSTRUCT1 (b1);  // both are (different) alts
                RECONSTRUCT1 (','); 
                RECONSTRUCT1 (b2); 
    }

    #define FIXED_VCF_VARDATA "\t.\tPASS\t.\tGT\t"
    RECONSTRUCT (FIXED_VCF_VARDATA, sizeof FIXED_VCF_VARDATA - 1);

    // Sample data
    RECONSTRUCT1 (is_alt_1 ? '1' : '0');
    
    if (recon_len==2) {
        RECONSTRUCT1 ('/');
        RECONSTRUCT1 (is_alt_2 ? '0'+num_uniq_alts : '0');
    }
    
    return 0;
}
