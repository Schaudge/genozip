// ------------------------------------------------------------------
//   vcffile.c
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt
 
#include "profiler.h"

#ifdef __APPLE__
#define off64_t __int64_t // needed for for conda mac - otherwise zlib.h throws compilation errors
#endif
#define Z_LARGE64
#include <zlib.h>
#include <bzlib.h>

#include "genozip.h"
#include "vcffile.h"
#include "vb.h"
#include "file.h"

// peformms a single I/O read operation - returns number of bytes read 
static uint32_t vcffile_read_block (File *file, char *data)
{
    START_TIMER;

    uint32_t bytes_read;

    if (file->type == VCF || file->type == STDIN) {
        
        bytes_read = read (fileno((FILE *)file->file), data, READ_BUFFER_SIZE);

        file->disk_so_far += (int64_t)bytes_read;

        if (file->type == STDIN) {
#ifdef _WIN32
            // in Windows using Powershell, the first 3 characters on an stdin pipe are BOM: 0xEF,0xBB,0xBF https://en.wikipedia.org/wiki/Byte_order_mark
            if (file->disk_so_far == (int64_t)bytes_read &&  // start of file
                bytes_read >= 3  && 
                (uint8_t)file->read_buffer[0] == 0xEF && 
                (uint8_t)file->read_buffer[1] == 0xBB && 
                (uint8_t)file->read_buffer[2] == 0xBF) {

                // Bomb the BOM
                bytes_read -= 3;
                memcpy (data, data + 3, bytes_read);
                file->disk_so_far -= 3;
            }
#endif
            file->type = VCF; // we only accept VCF from stdin. TO DO: identify the file type by the magic number (first 2 bytes for gz and bz2)
        }
    }
    else if (file->type == VCF_GZ) { 
        bytes_read = gzfread (data, 1, READ_BUFFER_SIZE, (gzFile)file->file);
        
        if (bytes_read)
            file->disk_so_far = gzoffset64 ((gzFile)file->file); // for compressed files, we update by block read
    }
    else if (file->type == VCF_BZ2) { 
        bytes_read = BZ2_bzread ((BZFILE *)file->file, data, READ_BUFFER_SIZE);

        if (bytes_read)
            file->disk_so_far = BZ2_bzoffset ((BZFILE *)file->file); // for compressed files, we update by block read
    } 
    else {
        ABORT0 ("Invalid file type");
    }
    
    COPY_TIMER (evb->profile.read);

    return bytes_read;
}

void vcffile_read_variant_block (VariantBlock *vb) 
{
    START_TIMER;

    File *file = vb->vcf_file;

    buf_alloc (vb, &vb->vcf_data, global_max_memory_per_vb, 1, "vcf_data", vb->variant_block_i);    

    // start with using the unconsumed data from the previous VB (note: copy & free and not move! so we can reuse vcf_data next vb)
    if (buf_is_allocated (&file->vcf_unconsumed_data)) {
        buf_copy (vb, &vb->vcf_data, &file->vcf_unconsumed_data, 0 ,0 ,0, "vcf_data", vb->variant_block_i);
        buf_free (&file->vcf_unconsumed_data);
    }

    // read data from the file until either 1. EOF is reached 2. end of block is reached
    while (vb->vcf_data.len <= global_max_memory_per_vb - READ_BUFFER_SIZE) {  // make sure there's at least READ_BUFFER_SIZE space available

        uint32_t bytes_one_read = vcffile_read_block (file, &vb->vcf_data.data[vb->vcf_data.len]);

        if (!bytes_one_read) { // EOF - we're expecting to have consumed all lines when reaching EOF (this will happen if the last line ends with newline as expected)
            ASSERT (!vb->vcf_data.len || vb->vcf_data.data[vb->vcf_data.len-1] == '\n', "Error: invalid VCF file %s - expecting it to end with a newline", file_printname (file));
            break;
        }

        vb->vcf_data.len += bytes_one_read;
    }

    // drop the final partial line which we will move to the next vb
    for (int32_t i=vb->vcf_data.len-1; i >= 0; i--) {

        if (vb->vcf_data.data[i] == '\n') {
            // case: still have some unconsumed data, that we wish  to pass to the next vb
            uint32_t unconsumed_len = vb->vcf_data.len-1 - i;
            if (unconsumed_len) {

                // the unconcusmed data is for the next vb to read 
                buf_copy (evb, &file->vcf_unconsumed_data, &vb->vcf_data, 1, // evb, because dst buffer belongs to File
                        vb->vcf_data.len - unconsumed_len, unconsumed_len, "vcf_file->vcf_unconsumed_data", vb->variant_block_i);

                vb->vcf_data.len -= unconsumed_len;
            }
            break;
        }
    }

    file->vcf_data_so_far += vb->vcf_data.len;
    file->disk_size        = file->disk_so_far; // in case it was not known
    vb->vb_data_size       = vb->vcf_data.len; // vb_data_size is redundant in ZIP at least, we can get rid of it one day

    if (flag_concat)
        md5_update (&vb->z_file->md5_ctx_concat, vb->vcf_data.data, vb->vcf_data.len);
    
    md5_update (&vb->z_file->md5_ctx_single, vb->vcf_data.data, vb->vcf_data.len);

    COPY_TIMER (vb->profile.vcffile_read_variant_block);
}

// returns the number of lines read 
void vcffile_read_vcf_header (bool is_first_vcf) 
{
    START_TIMER;

    File *file = evb->vcf_file;
    uint32_t bytes_read;

    // read data from the file until either 1. EOF is reached 2. end of vcf header is reached
    while (1) { 

        // enlarge if needed        
        if (evb->vcf_data.size - evb->vcf_data.len < READ_BUFFER_SIZE) 
            buf_alloc (evb, &evb->vcf_data, evb->vcf_data.size + READ_BUFFER_SIZE, 1.2, "vcf_data", 0);    

        bytes_read = vcffile_read_block (file, &evb->vcf_data.data[evb->vcf_data.len]);

        if (!bytes_read) { // EOF
            ASSERT (!evb->vcf_data.len || evb->vcf_data.data[evb->vcf_data.len-1] == '\n', 
                    "Error: invalid VCF file %s while reading VCF header - expecting it to end with a newline", file_printname (file));
            goto finish;
        }

        const char *this_read = &evb->vcf_data.data[evb->vcf_data.len];

        ASSERT (evb->vcf_data.len || this_read[0] == '#',
                "Error: %s is missing a VCF header - expecting first character in file to be #", file_printname (file));

        // case VB header: check stop condition - a line beginning with a non-#
        for (int i=0; i < bytes_read; i++) { // start from 1 back just in case it is a newline, and end 1 char before bc our test is 2 chars
            if (this_read[i] == '\n') 
                evb->num_lines++;   
                
            if ((i < bytes_read - 1 && this_read[i+1] != '#' && this_read[i] == '\n') ||   // vcf header ended if a line begins with a non-#
                (i==0 && evb->vcf_data.len>0 && this_read[i] != '#' && evb->vcf_data.data[evb->vcf_data.len-1]=='\n')) {

                uint32_t vcf_header_len = evb->vcf_data.len + i + 1;
                evb->vcf_data.len += bytes_read; // increase all the way - just for buf_copy

                // the excess data is for the next vb to read 
                buf_copy (evb, &file->vcf_unconsumed_data, &evb->vcf_data, 1, vcf_header_len,
                          bytes_read - (i+1), "vcf_file->vcf_unconsumed_data", 0);

                file->vcf_data_so_far += i+1; 
                evb->vcf_data.len = vcf_header_len;

                goto finish;
            }
        }

        evb->vcf_data.len += bytes_read;
        file->vcf_data_so_far += bytes_read;
    }

    if (flag_concat && is_first_vcf)
        md5_update (&evb->z_file->md5_ctx_concat, evb->vcf_data.data, evb->vcf_data.len);
    
    md5_update (&evb->z_file->md5_ctx_single, evb->vcf_data.data, evb->vcf_data.len);

finish:        
    file->disk_size = file->disk_so_far; // in case it was not known

    COPY_TIMER (evb->profile.vcffile_read_vcf_header);
}

unsigned vcffile_write_to_disk(File *vcf_file, const Buffer *buf)
{
    unsigned len = buf->len;
    char *next = buf->data;

    if (!flag_test) {
        while (len) {
            unsigned bytes_written = file_write (vcf_file, next, len);
            len  -= bytes_written;
            next += bytes_written;
        }
    }

    md5_update (&vcf_file->md5_ctx_concat, buf->data, buf->len);
    

    vcf_file->vcf_data_so_far += buf->len;
    vcf_file->disk_so_far     += buf->len;

    return buf->len;
}

void vcffile_write_one_variant_block (File *vcf_file, VariantBlock *vb)
{
    START_TIMER;

    unsigned size_written_this_vb = 0;

    for (unsigned line_i=0; line_i < vb->num_lines; line_i++) {
        Buffer *line = &vb->data_lines[line_i].line;

        if (line->len) // if this line is not filtered out
            size_written_this_vb += vcffile_write_to_disk (vcf_file, line);
    }

    ASSERTW (size_written_this_vb == vb->vb_data_size || exe_type == EXE_GENOCAT, 
            "Warning: Variant block %u (first_line=%u last_line=%u num_lines=%u) had %u bytes in the original VCF file but %u bytes in the reconstructed file", 
            vb->variant_block_i, vb->first_line, vb->first_line+vb->num_lines-1, vb->num_lines, vb->vb_data_size, size_written_this_vb);

    COPY_TIMER (vb->profile.write);
}
