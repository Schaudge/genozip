// ------------------------------------------------------------------
//   seg.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#ifndef SEGREGATE_INCLUDED
#define SEGREGATE_INCLUDED

#include <stdint.h>
#include "genozip.h"
#include "sections.h"
#include "context.h"

extern void seg_all_data_lines (VBlockP vb); 

extern const char *seg_get_next_item (void *vb, const char *str, int *str_len, 
                                      bool allow_newline, bool allow_tab, bool allow_colon, 
                                      unsigned *len, char *separator, bool *has_13, // out
                                      const char *item_name);
extern const char *seg_get_next_line (void *vb_, const char *str, int *str_len, unsigned *len, bool *has_13 /* out */, const char *item_name);

extern WordIndex seg_by_ctx (VBlockP vb, const char *snip, unsigned snip_len, ContextP ctx, uint32_t add_bytes, bool *is_new);
#define seg_by_dict_id(vb,snip,snip_len,dict_id,add_bytes)       seg_by_ctx ((VBlockP)vb, snip, snip_len, mtf_get_ctx (vb, (DictId)dict_id), add_bytes, NULL)
#define seg_by_did_i_ex(vb,snip,snip_len,did_i,add_bytes,is_new) seg_by_ctx ((VBlockP)vb, snip, snip_len, &vb->contexts[did_i], add_bytes, is_new);
#define seg_by_did_i(vb,snip,snip_len,did_i,add_bytes)           seg_by_ctx ((VBlockP)vb, snip, snip_len, &vb->contexts[did_i], add_bytes, NULL);

extern WordIndex seg_chrom_field (VBlockP vb, const char *chrom_str, unsigned chrom_str_len);

extern PosType seg_scan_pos_snip (VBlockP vb, const char *snip, unsigned snip_len, bool allow_nonsense);

#define MAX_POS_DELTA 32000 // the max delta (in either direction) that we will put in a dictionary - above this it goes to random_pos. This number can be changed at any time without affecting backward compatability - it is used only by ZIP, not PIZ
extern PosType seg_pos_field (VBlockP vb, 
                              DidIType snip_did_i,    // mandatory: the ctx the snip belongs to
                              DidIType base_did_i,    // mandatory: base for delta
                              bool allow_non_number,      // should be FALSE if the file format spec expects this field to by a numeric POS, and true if we empirically see it is a POS, but we have no guarantee of it
                              const char *pos_str, unsigned pos_len, 
                              uint32_t this_pos,
                              unsigned add_bytes);

extern void seg_id_field (VBlockP vb, DictId dict_id, const char *id_snip, unsigned id_snip_len, bool account_for_separator);

typedef bool (*SegSpecialInfoSubfields)(VBlockP vb, DictId dict_id, const char **this_value, unsigned *this_value_len, char *optimized_snip);

extern WordIndex seg_container_by_ctx (VBlockP vb, ContextP ctx, ContainerP con, const char *prefixes, unsigned prefixes_len, unsigned add_bytes);
#define seg_container_by_dict_id(vb,dict_id,con,add_bytes) seg_container_by_ctx ((VBlockP)vb, mtf_get_ctx (vb, dict_id), con, NULL, 0, add_bytes)

extern void seg_info_field (VBlockP vb, SegSpecialInfoSubfields seg_special_subfields, const char *info_str, unsigned info_len);

extern void seg_add_to_local_text   (VBlockP vb, ContextP ctx, const char *snip, unsigned snip_len, unsigned add_bytes);
extern void seg_add_to_local_fixed  (VBlockP vb, ContextP ctx, const void *data, unsigned data_len);
extern void seg_add_to_local_uint8  (VBlockP vb, ContextP ctx, uint8_t  value, unsigned add_bytes);
extern void seg_add_to_local_uint16 (VBlockP vb, ContextP ctx, uint16_t value, unsigned add_bytes);
extern void seg_add_to_local_uint32 (VBlockP vb, ContextP ctx, uint32_t value, unsigned add_bytes);
extern void seg_add_to_local_uint64 (VBlockP vb, ContextP ctx, uint64_t value, unsigned add_bytes);

extern WordIndex vcf_seg_delta_vs_other (VBlockP vb, Context *ctx, Context *other_ctx, const char *value, unsigned value_len, int64_t max_delta);

extern void seg_compound_field (VBlockP vb, ContextP field_ctx, const char *field, unsigned field_len, 
                                bool ws_is_sep, unsigned nonoptimized_len, unsigned add_for_eol);

typedef void (*SegOptimize)(const char **snip, unsigned *snip_len, char *space_for_new_str);
extern uint32_t seg_array_field (VBlockP vb, DictId dict_id, const char *value, unsigned value_len, bool add_bytes_by_textual, ContainerItemTransform transform, SegOptimize optimize);
extern WordIndex seg_hetero_array_field (VBlockP vb, DictId dict_id, const char *value, int value_len);

extern void seg_prepare_snip_other (uint8_t snip_code, DictId other_dict_id, bool has_parameter, int32_t parameter, 
                                    char *snip, unsigned *snip_len);

// ------------------
// Seg utilities
// ------------------

#define GET_NEXT_ITEM(item_name) \
    { field_start = next_field; \
      next_field = seg_get_next_item (vb, field_start, &len, false, true, false, &field_len, &separator, NULL, (item_name)); }

#define SEG_NEXT_ITEM(f) \
    { GET_NEXT_ITEM (DTF(names)[f]); \
      seg_by_did_i (vb, field_start, field_len, f, field_len+1); }

#define GET_LAST_ITEM(item_name) \
    { field_start = next_field; \
      next_field = seg_get_next_item (vb, field_start, &len, true, false, false, &field_len, &separator, has_13, item_name); }

#define GET_MAYBE_LAST_ITEM(item_name) \
    { field_start = next_field; \
      next_field = seg_get_next_item (vb, field_start, &len, true, true, false, &field_len, &separator, has_13, item_name); }

// create extendent field contexts in the correct order of the fields
#define EXTENDED_FIELD_CTX(extended_field, dict_id_num) { \
    Context *ctx = mtf_get_ctx (vb, (DictId)dict_id_num); \
    ASSERT (ctx->did_i == extended_field, "Error: expecting ctx->did_i=%u to be %u", ctx->did_i, extended_field); \
    dict_id_fields[ctx->did_i] = ctx->dict_id.num; \
}

#define SAFE_ASSIGN(reg,addr,char_val) /* we are careful to evaluate addr, char_val only once, less they contain eg ++ */ \
    char *__addr##reg = (char*)(addr); \
    char __save##reg = *__addr##reg; \
    *__addr##reg = (char_val);

#define SAFE_RESTORE(reg) *__addr##reg = __save##reg; 

#define SEG_EOL(f,account_for_ascii10) seg_by_did_i (vb, *(has_13) ? "\r\n" : "\n", 1 + *(has_13), (f), (account_for_ascii10) + *(has_13)); 

#define ASSSEG(condition, p_into_txt, format, ...) \
    ASSERT (condition, format "\n\nFile: %s vb_line_i:%u vb_i:%u pos_in_vb: %"PRIi64" pos_in_file: %"PRIi64\
                              "\nvb pos in file (0-based):%"PRIu64" - %"PRIu64" (length %"PRIu64")" \
                              "\n%d characters before to %d characters after (in quotes): \"%.*s\""\
                              "\n%d characters before to %d characters after (in quotes): \"%.*s\""\
                              "\nTo get vblock: %s %s | head -c %"PRIu64" | tail -c %"PRIu64 " > vb%s", \
            __VA_ARGS__, txt_name, vb->line_i, vb->vblock_i, \
            /* pos_in_vb:         */ (PosType)(p_into_txt ? (p_into_txt - vb->txt_data.data) : -1), \
            /* pos_in_file:       */ (PosType)(p_into_txt ? (vb->vb_position_txt_file + (p_into_txt - vb->txt_data.data)) : -1),\
            /* vb start pos file: */ vb->vb_position_txt_file, \
            /* vb end pos file:   */ vb->vb_position_txt_file + vb->txt_data.len-1, \
            /* vb length:         */ vb->txt_data.len,\
            /* +- 30 char snip    */\
            /* chars before:      */ p_into_txt ? MIN (30, (unsigned)(p_into_txt - vb->txt_data.data)) : -1, \
            /* chars after:       */ p_into_txt ? MIN (30, (unsigned)(vb->txt_data.data + vb->txt_data.len - p_into_txt)) : -1,\
            /* snip len:          */ p_into_txt ? (unsigned)(MIN (p_into_txt+31, vb->txt_data.data + vb->txt_data.len) /* end pos */ - MAX (p_into_txt-30, vb->txt_data.data) /* start_pos */) : -1,\
            /* condition for snip */ (vb->txt_data.data && p_into_txt && (p_into_txt >= vb->txt_data.data) && (p_into_txt <= /* = too */ vb->txt_data.data + vb->txt_data.len) ? \
            /* snip start:        */    MAX (p_into_txt-30, vb->txt_data.data) : "(inaccessible)"),\
            /* +- 2 char snip    */\
            /* chars before:      */ p_into_txt ? MIN (2, (unsigned)(p_into_txt - vb->txt_data.data)) : -1, \
            /* chars after:       */ p_into_txt ? MIN (2, (unsigned)(vb->txt_data.data + vb->txt_data.len - p_into_txt)) : -1,\
            /* snip len:          */ p_into_txt ? (unsigned)(MIN (p_into_txt+3, vb->txt_data.data + vb->txt_data.len) /* end pos */ - MAX (p_into_txt-2, vb->txt_data.data) /* start_pos */) : -1,\
            /* condition for snip */ (vb->txt_data.data && p_into_txt && (p_into_txt >= vb->txt_data.data) && (p_into_txt <= /* = too */ vb->txt_data.data + vb->txt_data.len) ? \
            /* snip start:        */    MAX (p_into_txt-3, vb->txt_data.data) : "(inaccessible)"),\
            /* head, tail params: */ file_viewer (txt_file), txt_name, vb->vb_position_txt_file + vb->txt_data.len, vb->txt_data.len,\
            /* plain file ext:    */ file_plain_ext_by_dt (vb->data_type))

#define ASSSEG0(condition, p_into_txt, err_str) ASSSEG (condition, p_into_txt, err_str "%s", "")

#define ABOSEG(p_into_txt, format, ...) ASSSEG(false, p_into_txt, format, __VA_ARGS__)

#define ABOSEG0(p_into_txt, err_str) ABOSEG(false, p_into_txt, format, err_str "%s", "")

#endif