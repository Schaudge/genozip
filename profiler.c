// ------------------------------------------------------------------
//   profiler.c
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "genozip.h"
#include "profiler.h"

void profiler_add (ProfilerRec *dst, const ProfilerRec *src)
{
#   define ADD(x) dst->x += src->x

    ADD(read);
    ADD(compute);
    ADD(write);
    ADD(compressor_bz2);
    ADD(compressor_lzma);
    ADD(compressor_bsc);
    ADD(compressor_domq);
    ADD(compressor_actg);
    ADD(zip_generate_and_compress_ctxs);
    ADD(codec_assign_best_codec);
    ADD(bgzf_io_thread);
    ADD(bgzf_compute_thread);
    ADD(piz_reconstruct_vb);
    ADD(piz_get_line_subfields);
    ADD(piz_read_one_vb);
    ADD(compressor_hapmat);
    ADD(codec_hapmat_piz_get_one_line);
    ADD(sam_seg_seq_field);
    ADD(zfile_compress_dictionary_data);
    ADD(zfile_uncompress_section);
    ADD(buf_alloc);
    ADD(txtfile_read_vblock);
    ADD(txtfile_read_header);
    ADD(seg_all_data_lines);
    ADD(seg_initialize);
    ADD(ctx_merge_in_vb_ctx);
    ADD(codec_hapmat_count_alt_alleles);
    ADD(md5);
    ADD(lock_mutex_compress_dict);
    ADD(lock_mutex_zf_ctx);
    ADD(ctx_merge_in_vb_ctx_one_dict_id);
    ADD(ctx_clone_ctx);
    ADD(ctx_integrate_dictionary_fragment);
    ADD(aligner_best_match);
    ADD(aligner_get_match_len);
    ADD(aligner_get_word_from_seq);
    ADD(generate_rev_complement_genome);
    ADD(tmp1);
    ADD(tmp2);
    ADD(tmp3);
    ADD(tmp4);
    ADD(tmp5);

#undef ADD
}

static inline uint32_t ms(uint64_t ns) { return (uint32_t)(ns / 1000000);}

const char *profiler_print_short (const ProfilerRec *p)
{
    static char str[200]; // not thread safe
    sprintf (str, "read: %u compute:%u write: %u", ms(p->read), ms(p->compute), ms(p->write));
    return str;
}

void profiler_print_report (const ProfilerRec *p, unsigned max_threads, unsigned used_threads, const char *filename, unsigned num_vbs)
{
    static const char *space = "                                                   ";
#   define PRINT(x, level) if (p->x) fprintf (stderr, "%.*s" #x ": %u\n", level*3, space, ms(p->x));
    
#if defined _WIN32
    static const char *os ="Windows";
#elif defined __APPLE__
    static const char *os ="MacOS";
#elif defined __linux__
    static const char *os ="Linux";
#else
    static const char *os ="Unknown OS";
#endif

    fprintf (stderr, "\n%s PROFILER:\n", command == ZIP ? "ZIP" : "PIZ");
    fprintf (stderr, "OS=%s\n", os);
#ifdef DEBUG
    fprintf (stderr, "Build=Debug\n");
#else
    fprintf (stderr, "Build=Optimized\n");
#endif
    fprintf (stderr, "Compute threads: max_permitted=%u actually_used=%u\n", max_threads, used_threads);
    fprintf (stderr, "file=%s\n\n", filename ? filename : "(not file)");
    
    fprintf (stderr, "Wallclock: %u milliseconds\n", ms (p->wallclock));

    if (command != ZIP) { // this is a uncompress operation

        fprintf (stderr, "GENOUNZIP I/O thread (piz_one_file):\n");
        PRINT (piz_read_one_vb, 1);
        PRINT (read, 2);
        PRINT (ctx_integrate_dictionary_fragment, 2);
        PRINT (bgzf_io_thread, 1);
        PRINT (write, 1);
        fprintf (stderr, "GENOUNZIP compute threads: %u\n", ms(p->compute));
        PRINT (zfile_uncompress_section, 1);
        PRINT (piz_reconstruct_vb, 1);
        PRINT (md5, 1);
        PRINT (bgzf_compute_thread, 1);
        PRINT (piz_get_line_subfields, 2);
        PRINT (codec_hapmat_piz_get_one_line, 2);
    }
    else { // compress
        fprintf (stderr, "GENOZIP I/O thread (zip_one_file):\n");
        PRINT (txtfile_read_header, 1);
        PRINT (txtfile_read_vblock, 1);
        PRINT (read, 2);
        PRINT (write, 1);
        PRINT (bgzf_io_thread, 1);
        fprintf (stderr, "GENOZIP compute threads %u\n", ms(p->compute));
        PRINT (ctx_clone_ctx, 1);
        PRINT (seg_all_data_lines, 1);
        PRINT (aligner_best_match, 2);
        PRINT (aligner_get_match_len, 3);
        PRINT (aligner_get_word_from_seq, 3);
        PRINT (seg_initialize, 2);
        PRINT (sam_seg_seq_field,2);
        PRINT (ctx_merge_in_vb_ctx, 1);
        PRINT (lock_mutex_zf_ctx, 2);
        PRINT (ctx_merge_in_vb_ctx_one_dict_id, 2);
        PRINT (lock_mutex_compress_dict, 2);
        PRINT (zfile_compress_dictionary_data, 2);
        PRINT (zip_generate_and_compress_ctxs, 1);
        PRINT (codec_assign_best_codec, 1);
        PRINT (md5, 1);
        PRINT (bgzf_compute_thread, 1);
        PRINT (compressor_bz2,  1);
        PRINT (compressor_lzma, 1);
        PRINT (compressor_bsc,  1);
        PRINT (compressor_domq, 1);
        PRINT (compressor_actg, 1);
        PRINT (compressor_hapmat, 1);
        PRINT (codec_hapmat_count_alt_alleles, 2);
    }    

    PRINT (buf_alloc, 0);
    PRINT (generate_rev_complement_genome, 0);
    
    fprintf (stderr, "tmp1: %u tmp2: %u tmp3: %u tmp4: %u tmp5: %u\n\n", ms(p->tmp1), ms(p->tmp2), ms(p->tmp3), ms(p->tmp4), ms(p->tmp5));

    fprintf (stderr, "\nVblock stats:\n");
    fprintf (stderr, "  Vblocks: %u\n", num_vbs);
    fprintf (stderr, "  Maximum vblock size: %u MB\n", global_max_memory_per_vb / (1024 * 1024));
    fprintf (stderr, "  Average wallclock: %u\n", ms(p->wallclock) / num_vbs);
    fprintf (stderr, "  Average read time: %u\n", ms(p->read) / num_vbs);
    fprintf (stderr, "  Average compute time: %u\n", ms(p->compute) / num_vbs);
    fprintf (stderr, "  Average write time: %u\n", ms(p->write) / num_vbs);
    fprintf (stderr, "\n");
}
