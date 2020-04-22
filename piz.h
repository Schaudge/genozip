// ------------------------------------------------------------------
//   piz.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#ifndef PIZ_INCLUDED
#define PIZ_INCLUDED

#include "genozip.h"

extern bool piz_dispatcher (const char *z_basename, unsigned max_threads, bool is_first_vcf_component, bool is_last_file);

extern int32_t piz_decode_pos (int32_t last_pos, const char *delta_snip, unsigned delta_snip_len, char *pos_str, unsigned *pos_len);
extern void piz_uncompress_fields (VBlockP vb, const unsigned *section_index, unsigned *section_i);

extern void piz_vcf_uncompress_one_vb (VBlockP vb);
extern void piz_sam_uncompress_one_vb (VBlockP vb);
extern void piz_fast_uncompress_one_vb (VBlockP vb);
extern void piz_me23_uncompress_one_vb (VBlockP vb);

// ----------------------------------------------
// utilities for use by piz_*_reconstruct_vb
// ----------------------------------------------

// gets snip, snip_len from b250 data
#define LOAD_SNIP(did_i) mtf_get_next_snip ((VBlockP)vb, &vb->mtf_ctx[(did_i)], NULL, &snip, &snip_len, txt_line_i); 

#define RECONSTRUCT_FROM_DICT(did_i)  /* we don't put in a {} so that caller can use index=RECONSTRUCT_FROM_DICT() */ \
    LOAD_SNIP(did_i) \
    buf_add (&vb->reconstructed_line, snip, snip_len); \
    buf_add (&vb->reconstructed_line, "\t", 1); 

#define RECONSTRUCT_FROM_DICT_POS(did_i,update_last_pos,add_tab) { \
    if ((did_i) != DID_I_NONE) LOAD_SNIP(did_i);\
    char pos_str[30];\
    uint32_t new_pos = piz_decode_pos (vb->last_pos, snip, snip_len, pos_str, &snip_len); \
    if (update_last_pos) vb->last_pos = new_pos;\
    buf_add (&vb->reconstructed_line, pos_str, snip_len);\
    if (add_tab) buf_add (&vb->reconstructed_line, "\t", 1); }

#define LOAD_SNIP_FROM_BUF(buf,next,field_name) { \
    uint32_t start = next; \
    ARRAY (char, data, buf);\
    for (; next < buf.len && data[next] != '\t'; next++);\
    ASSERT (next < buf.len, \
            "Error reconstructing txt_line=%u: unexpected end of " field_name " data (len=%u)", txt_line_i, (uint32_t)buf.len); \
    snip = &data[start];\
    snip_len = next - start; \
    next++; /* skip the tab */ }

// reconstructs from the buffer up to a tab    
#define RECONSTRUCT_FROM_BUF(buf,next,field_name) { \
    LOAD_SNIP_FROM_BUF(buf,next,field_name) \
    buf_add (&vb->reconstructed_line, snip, snip_len); \
    buf_add (&vb->reconstructed_line, "\t", 1);  }

// reconstructs a fix number of characters from a tab-less buffer
#define RECONSTRUCT_FROM_TABLESS_BUF(buf,next,fixed_len,add_tab,field_name) { \
    ARRAY (char, data, buf);\
    ASSERT (next + (fixed_len) <= buf.len, \
            "Error reconstructing txt_line=%u: unexpected end of " field_name " data (buf.len=%u next=%u fixed_len=%u)", \
            txt_line_i, (uint32_t)buf.len, next, fixed_len); \
    buf_add (&vb->reconstructed_line, &data[next], (fixed_len)); \
    if (add_tab) buf_add (&vb->reconstructed_line, "\t", 1);  \
    next += fixed_len; }

#endif

