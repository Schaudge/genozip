// ------------------------------------------------------------------
//   seg.h
//   Copyright (C) 2019-2025 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#pragma once

#include <stdint.h>
#include "genozip.h"
#include "sections.h"
#include "context.h"
#include "container.h"
#include "multiplexer.h"

typedef enum { ERR_SEG_NO_ERROR=0, ERR_SEG_OUT_OF_RANGE, ERR_SEG_NOT_INTEGER } SegError;

extern void zip_modify (VBlockP vb);
extern uint32_t seg_all_data_lines (VBlockP vb); 

typedef enum { GN_FORBIDEN, GN_SEP, GN_IGNORE } GetNextAllow;
extern rom seg_get_next_item (VBlockP vb, rom str, int *str_len, GetNextAllow newline, GetNextAllow tab, GetNextAllow space,
                              unsigned *len, char *separator, bool *has_13, // out
                              rom item_name);
extern rom seg_get_next_line (VBlockP vb, rom str, int *str_len, unsigned *len, bool must_have_newline, bool *has_13 /* out */, rom item_name);

extern WordIndex seg_by_ctx_ex (VBlockP vb, STRp(snip), ContextP ctx, uint32_t add_bytes, bool *is_new);
static inline WordIndex seg_by_ctx (VBlockP vb, STRp(snip), ContextP ctx, unsigned add_bytes)                 { return seg_by_ctx_ex (vb, STRa(snip), ctx, add_bytes, NULL); }
static inline WordIndex seg_by_dict_id (VBlockP vb, STRp(snip), DictId dict_id, unsigned add_bytes)           { return seg_by_ctx_ex (vb, STRa(snip), ctx_get_ctx (vb, dict_id), add_bytes, NULL); }
static inline WordIndex seg_by_did_i_ex (VBlockP vb, STRp(snip), Did did_i, unsigned add_bytes, bool *is_new) { return seg_by_ctx_ex (vb, STRa(snip), CTX(did_i), add_bytes, is_new); }
static inline WordIndex seg_by_did (VBlockP vb, STRp(snip), Did did_i, unsigned add_bytes)                    { return seg_by_ctx_ex (vb, STRa(snip), CTX(did_i), add_bytes, NULL); }
static inline void seg_special0 (VBlockP vb, uint8_t special, ContextP ctx, unsigned add_bytes)                  { seg_by_ctx (vb, (char[]){ SNIP_SPECIAL, (char)special }, 2, ctx, add_bytes); }
static inline void seg_special1 (VBlockP vb, uint8_t special, char c1, ContextP ctx, unsigned add_bytes)         { seg_by_ctx (vb, (char[]){ SNIP_SPECIAL, (char)special, c1 }, 3, ctx, add_bytes); }
static inline void seg_special2 (VBlockP vb, uint8_t special, char c1, char c2, ContextP ctx, unsigned add_bytes){ seg_by_ctx (vb, (char[]){ SNIP_SPECIAL, (char)special, c1, c2 }, 4, ctx, add_bytes); }
static inline void seg_special3 (VBlockP vb, uint8_t special, char c1, char c2, char c3, ContextP ctx, unsigned add_bytes){ seg_by_ctx (vb, (char[]){ SNIP_SPECIAL, (char)special, c1, c2, c3 }, 5, ctx, add_bytes); }
static inline void seg_special4 (VBlockP vb, uint8_t special, char c1, char c2, char c3, char c4, ContextP ctx, unsigned add_bytes){ seg_by_ctx (vb, (char[]){ SNIP_SPECIAL, (char)special, c1, c2, c3, c4 }, 6, ctx, add_bytes); }
static inline void seg_init_all_the_same (VBlockP vb, Did did_i, STRp(snip)) /*use in seg_initialize*/        { seg_by_ctx_ex (vb, STRa(snip), CTX(did_i), 0, NULL); }
static inline void seg_all_the_same (VBlockP vb, ContextP ctx, uint32_t add_bytes)                            { ctx_increment_count (vb, ctx, 0); ctx->txt_len += add_bytes; }

extern WordIndex seg_known_node_index (VBlockP vb, ContextP ctx, WordIndex node_index, unsigned add_bytes);
extern WordIndex seg_duplicate_last (VBlockP vb, ContextP ctx, unsigned add_bytes);

extern void seg_integer (VBlockP vb, ContextP ctx, int64_t n, bool with_lookup, unsigned add_bytes);

extern WordIndex seg_integer_as_snip_do (VBlockP vb, ContextP ctx, int64_t n, unsigned add_bytes); // segs integer as normal textual snip
#define seg_integer_as_snip(vb,did_i,n,add_sizeof_n) seg_integer_as_snip_do((VBlockP)(vb), &vb->contexts[did_i], (n), (add_sizeof_n) ? sizeof(n) : 0)

extern void seg_simple_lookup (VBlockP vb, ContextP ctx, unsigned add_bytes);
extern void seg_lookup_with_length (VBlockP vb, ContextP ctx, int32_t length, unsigned add_bytes);

extern bool seg_integer_or_not (VBlockP vb, ContextP ctx, STRp(this_value), unsigned add_bytes); // segs integer in local if possible
extern bool seg_integer_or_not_cb (VBlockP vb, ContextP ctx, STRp(int_str), uint32_t repeat);
extern void seg_numeric_or_not (VBlockP vb, ContextP ctx, STRp(value), unsigned add_bytes);
extern bool seg_float_or_not (VBlockP vb, ContextP ctx, STRp(this_value), unsigned add_bytes);

extern void seg_maybe_copy (VBlockP vb, ContextP ctx, Did other_did, STRp(value), STRp(copy_snip));

#define SPF_BAD_SNIPS_TOO   1  // should be FALSE if the file format spec expects this field to by a numeric POS, and true if we empirically see it is a POS, but we have no guarantee of it
#define SPF_ZERO_IS_BAD     2  // whether 0 is considered a bad POS (if true and POS is 0, to be handled according to seg_bad_snips_too)
#define SPF_NO_DELTA        8  // All integer data goes into local
extern PosType64 seg_pos_field (VBlockP vb, Did snip_did_i, Did base_did_i, unsigned opt, 
                              char missing, STRp(pos_str), PosType64 this_pos, unsigned add_bytes);
extern bool seg_pos_field_cb (VBlockP vb, ContextP ctx, STRp(pos_str), uint32_t repeat);

extern void seg_id_field (VBlockP vb, ContextP ctx, STRp(id), bool hint_zero_padded_fixed_len, unsigned add_bytes);     

extern bool seg_id_field_varlen_int_cb (VBlockP vb, ContextP ctx, STRp(id), uint32_t repeat);
extern bool seg_id_field_fixed_int_cb (VBlockP vb, ContextP ctx, STRp(id), uint32_t repeat);

typedef enum { LOOKUP_NONE, LOOKUP_SIMPLE, LOOKUP_WITH_LENGTH } Lookup;
extern void seg_add_to_local_fixed_do (VBlockP vb, ContextP ctx, const void *const data, uint32_t data_len, bool add_nul, Lookup lookup_type, bool is_singleton, unsigned add_bytes);

extern void seg_add_to_local_string (VBlockP vb, ContextP ctx, STRp(snip), Lookup lookup_type, unsigned add_bytes);
extern bool seg_add_to_local_string_cb (VBlockP vb, ContextP ctx, STRp(str), uint32_t repeat);
extern bool seg_add_to_local_fixed_len_cb (VBlockP vb, ContextP ctx, STRp(str), uint32_t repeat);

static inline void seg_add_to_local_blob (VBlockP vb, ContextP ctx, STRp(blob), unsigned add_bytes) 
{ 
#ifdef DEBUG
    ASSERT (segconf_running || ctx->ltype == LT_BLOB, "%s: Expecting %s.ltype=LT_BLOB but found %s", LN_NAME, ctx->tag_name, lt_name (ctx->ltype));
#endif
    seg_add_to_local_fixed_do (vb, ctx, STRa(blob), false, LOOKUP_WITH_LENGTH, false, add_bytes); 
}

static inline void seg_add_to_local_fixed (VBlockP vb, ContextP ctx, const void *data, uint32_t data_len, Lookup lookup_type, unsigned add_bytes)
    { seg_add_to_local_fixed_do (vb, ctx, STRa(data), false, lookup_type, false, add_bytes); }

extern void seg_integer_fixed (VBlockP vb, ContextP ctx, void *number, bool with_lookup, unsigned add_bytes);

extern WordIndex seg_self_delta (VBlockP vb, ContextP ctx, int64_t value, char format, unsigned fixed_len, uint32_t add_bytes);

extern void seg_delta_vs_other_do (VBlockP vb, ContextP ctx, ContextP other_ctx, STRp(value), int64_t value_n, int64_t max_delta, bool delta_in_local, unsigned add_bytes);

static inline void seg_delta_vs_other_localN (VBlockP vb, ContextP ctx, ContextP other_ctx, int64_t value, int64_t max_delta, unsigned add_bytes) 
    { seg_delta_vs_other_do (vb, ctx, other_ctx, 0, 0, value, max_delta, true, add_bytes); }

static inline void seg_delta_vs_other_localS (VBlockP vb, ContextP ctx, ContextP other_ctx, STRp(value), int64_t max_delta)
    { seg_delta_vs_other_do (vb, ctx, other_ctx, STRa(value), 0, max_delta, true, value_len); }

static inline void seg_delta_vs_other_dictN (VBlockP vb, ContextP ctx, ContextP other_ctx, int64_t value, int64_t max_delta, unsigned add_bytes) 
    { seg_delta_vs_other_do (vb, ctx, other_ctx, 0, 0, value, max_delta, false, add_bytes); }

static inline void seg_delta_vs_other_dictS (VBlockP vb, ContextP ctx, ContextP other_ctx, STRp(value), int64_t max_delta)
    { seg_delta_vs_other_do (vb, ctx, other_ctx, STRa(value), 0, max_delta, false, value_len); }

extern void seg_diff (VBlockP vb, ContextP ctx, ContextP base_ctx, STRp(value), bool entire_snip_if_same, unsigned add_bytes);

typedef bool (*SegCallback) (VBlockP vb, ContextP ctx, STRp(value), uint32_t repeat); // returns true if segged successfully

extern WordIndex seg_array_(VBlockP vb, ContextP container_ctx, Did stats_conslidation_did_i, rom value, int32_t value_len, char sep, char subarray_sep, bool use_integer_delta, StoreType store_in_local, DictId arr_dict_id, uint8_t con_rep_special, uint32_t expected_num_repeats, int add_bytes);

static inline WordIndex seg_array (VBlockP vb, ContextP container_ctx, Did stats_conslidation_did_i, rom value, int32_t value_len, char sep, char subarray_sep, bool use_integer_delta, StoreType store_in_local, DictId arr_dict_id, int add_bytes)
{ return seg_array_(vb, container_ctx, stats_conslidation_did_i, STRa(value), sep, subarray_sep, use_integer_delta, store_in_local, arr_dict_id, 0, 0, add_bytes); }

extern void seg_array_by_callback (VBlockP vb, ContextP container_ctx, STRp(arr), char sep, SegCallback item_seg, uint8_t con_rep_special, uint32_t expected_num_repeats, unsigned add_bytes);

extern void seg_integer_matrix (VBlockP vb, ContextP container_ctx, Did stats_conslidation_did_i, STRp(value), char row_sep/*255 if always single row*/, char col_sep, bool is_transposed, uint8_t con_rep_special, uint32_t expected_num_repeats, int add_bytes);

extern bool seg_do_nothing_cb (VBlockP vb, ContextP ctx, STRp(field), uint32_t rep);

typedef void (*SplitCorrectionCallback) (uint32_t *n_repeats, rom *repeats, uint32_t *repeat_lens);

extern bool seg_struct (VBlockP vb, ContextP ctx, MediumContainer con, STRp(snip), const SegCallback *callbacks, unsigned add_bytes, bool account_in_subfields);

extern int32_t seg_array_of_struct_ (VBlockP vb, ContextP ctx, MediumContainer con, STRp(prefixes), STRp(snip), const SegCallback *callbacks, uint8_t con_rep_special, uint32_t expected_num_repeats, SplitCorrectionCallback split_correction_callback, unsigned add_bytes);

static inline int32_t seg_array_of_struct (VBlockP vb, ContextP ctx, MediumContainer con, STRp(snip), const SegCallback *callbacks, SplitCorrectionCallback split_correction_callback, unsigned add_bytes)
{
    return seg_array_of_struct_ (vb, ctx, con, 0, 0, STRa(snip), callbacks, 0, 0, split_correction_callback, add_bytes);
}

extern void seg_array_of_array_of_struct (VBlockP vb, ContextP ctx, char outer_sep, MediumContainer inner_con, STRp(snip), const SegCallback *callbacks);

extern bool seg_by_container (VBlockP vb, ContextP ctx, ContainerP con, STRp(value), STRp(container_snip), SegCallback item_seg, bool normal_seg_if_fail, unsigned add_bytes);

// common SPECIAL methods
extern void seg_LEN_OF (VBlockP vb, ContextP ctx, STRp(len_str), uint32_t other_str_len, STRp(special_snip));
extern void seg_by_ARRAY_LEN_OF (VBlockP vb, ContextP ctx, STRp(value), STRp(other_array), STRp(snip));
extern void seg_textual_float (VBlockP vb, ContextP ctx, STRp(f), unsigned add_bytes);

extern void seg_prepare_snip_other_do (uint8_t snip_code, DictId other_dict_id, bool has_parameter, int64_t int_param, char char_param, qSTRp(snip));
#define seg_prepare_snip_other(snip_code, other_dict_id, has_parameter, parameter, snip) \
    snip##_len = sizeof (snip);\
    seg_prepare_snip_other_do ((snip_code), (DictId)(other_dict_id), (has_parameter), (parameter), 0, (snip), &snip##_len)

#define seg_prepare_snip_other_char(snip_code, other_dict_id, char_param, snip) \
    ({ snip##_len = sizeof (snip);\
       seg_prepare_snip_other_do ((snip_code), (DictId)(other_dict_id), true, 0, (char_param), (snip), &snip##_len); })

#define seg_prepare_snip_other_chari(snip_code, other_dict_id, char_param, snip, i) \
    ({ snip##_lens[i] = sizeof (snip##s);\
       seg_prepare_snip_other_do ((snip_code), (DictId)(other_dict_id), true, 0, (char_param), (snip##s)[i], &snip##_lens[i]); })

#define seg_prepare_snip_special_other(special_code, other_dict_id, snip, char_param/*0=no parameter*/) \
    ({ snip[0]=SNIP_SPECIAL; snip##_len=sizeof(snip)-1; \
       seg_prepare_snip_other_do ((special_code), (DictId)(other_dict_id), char_param, 0, char_param, &snip[1], &snip##_len); \
       snip##_len++; })

#define seg_prepare_snip_special_otheri(special_code, other_dict_id, snip, i, char_param/*0=no parameter*/) \
    ({ snip##s[i][0]=SNIP_SPECIAL; snip##_lens[i]=sizeof(snip##s[i])-1; \
       seg_prepare_snip_other_do ((special_code), (DictId)(other_dict_id), char_param, 0, char_param, &snip##s[i][1], &snip##_lens[i]); \
       snip##_lens[i]++; })

#define seg_prepare_snip_special_other_char(special_code, other_dict_id, snip, char_param) \
    ({ snip[0]=SNIP_SPECIAL; snip##_len=sizeof(snip)-1; \
       seg_prepare_snip_other_do ((special_code), (DictId)(other_dict_id), true, 0, (char_param), &snip[1], &snip##_len); \
       snip##_len++; })

#define seg_prepare_snip_special_other_int(special_code, other_dict_id, snip, int_param) \
    ({ snip[0]=SNIP_SPECIAL; snip##_len=sizeof(snip)-1; \
       seg_prepare_snip_other_do ((special_code), (DictId)(other_dict_id), true, int_param, 0, &snip[1], &snip##_len); \
       snip##_len++; })

extern void seg_prepare_multi_dict_id_special_snip (uint8_t special_code, unsigned num_dict_ids, DictId *dict_ids, char *out_snip, unsigned *out_snip_len);

extern void seg_prepare_array_dict_id_special_snip (int num_dict_ids, DictId *dict_ids, uint8_t special_code, qSTRp(snip));

#define seg_prepare_plus_snip(dt, num_dict_ids, dict_ids, snip) \
    ({ ASSERT ((num_dict_ids) <= MAX_SNIP_DICTS, "num_dict_ids=%d too large. increase MAX_SNIP_DICTS=%d", (num_dict_ids), MAX_SNIP_DICTS); \
       snip##_len = sizeof (snip);\
       seg_prepare_array_dict_id_special_snip ((num_dict_ids), (dict_ids), dt##_SPECIAL_PLUS, (snip), &snip##_len); })

// A minus B
#define seg_prepare_minus_snip(dt, dict_id_a, dict_id_b, snip) \
    ({ snip##_len = sizeof (snip);\
       seg_prepare_array_dict_id_special_snip (2, (DictId[]){(DictId)(dict_id_a), (DictId)(dict_id_b)}, dt##_SPECIAL_MINUS, (snip), &snip##_len); })

#define seg_prepare_abs_minus_snip(dt, dict_id_a, dict_id_b, snip) \
    ({ snip##_len = sizeof (snip) - 1;\
       seg_prepare_array_dict_id_special_snip (2, (DictId[]){(DictId)(dict_id_a), (DictId)(dict_id_b)}, dt##_SPECIAL_MINUS, (snip), &snip##_len); \
       snip[snip##_len++] = 'A' /* ABS() */})

#define seg_prepare_minus_snip_i(dt, dict_id_a, dict_id_b, snip, i) \
    ({ snip##_lens[i] = sizeof (snip##s[i]);\
       seg_prepare_array_dict_id_special_snip (2, (DictId[]){(DictId)(dict_id_a), (DictId)(dict_id_b)}, dt##_SPECIAL_MINUS, snip##s[i], &snip##_lens[i]); })

static void inline seg_set_last_txt (VBlockP vb, ContextP ctx, STRp(value))
{
    bool is_value_in_txt_data = value >= B1STtxt &&
                                value <= BLST  (char, vb->txt_data);

    ctx->last_txt = (TxtWord){ .index = is_value_in_txt_data ? BNUMtxt (value) : INVALID_LAST_TXT_INDEX,
                               .len   = value_len };

    ctx_set_encountered (vb, ctx);
}

bool seg_set_last_txt_store_value (VBlockP vb, ContextP ctx, STRp(value), StoreType store_type);

extern void seg_create_rollback_point (VBlockP vb, ContainerP con, unsigned num_ctxs, ...); // list of did_i
extern void seg_add_ctx_to_rollback_point (VBlockP vb, ContextP ctx);
extern void seg_rollback (VBlockP vb);

extern void seg_mux_init_(VBlockP vb, Did did_i, unsigned num_channels, uint8_t special_code, bool no_stons, MultiplexerP mux);
#define seg_mux_init(vb,did_i,special_code,no_stons,mux_name) \
    seg_mux_init_((VBlockP)(vb), (did_i), MUX_CAPACITY((vb)->mux_##mux_name), (special_code), (no_stons), (MultiplexerP)&(vb)->mux_##mux_name)

extern ContextP seg_mux_get_channel_ctx (VBlockP vb, Did did_i, MultiplexerP mux, uint32_t channel_i);

// --------------------
// handling binary data
// --------------------

// getting integers from the BAM data
#define NEXT_UINT8    ({ uint8_t  value = GET_UINT8   (next_field); next_field += sizeof (uint8_t ); value; })
#define NEXT_UINT16   ({ uint16_t value = GET_UINT16  (next_field); next_field += sizeof (uint16_t); value; })
#define NEXT_UINT32   ({ uint32_t value = GET_UINT32  (next_field); next_field += sizeof (uint32_t); value; })
#define NEXT_UINT64   ({ uint64_t value = GET_UINT64  (next_field); next_field += sizeof (uint64_t); value; })
#define NEXT_FLOAT32  ({ float    value = GET_FLOAT32 (next_field); next_field += sizeof (float);    value; })

#define NEXTP_UINT8   ({ uint8_t  value = GET_UINT8   (*next_field_p); *next_field_p += sizeof (uint8_t);  value; })
#define NEXTP_UINT16  ({ uint16_t value = GET_UINT16  (*next_field_p); *next_field_p += sizeof (uint16_t); value; })
#define NEXTP_UINT32  ({ uint32_t value = GET_UINT32  (*next_field_p); *next_field_p += sizeof (uint32_t); value; })
#define NEXTP_UINT64  ({ uint64_t value = GET_UINT64  (*next_field_p); *next_field_p += sizeof (uint64_t); value; })
#define NEXTP_FLOAT32 ({ uint32_t value = GET_FLOAT32 (*next_field_p); *next_field_p += sizeof (float);    value; })

// ------------------
// Seg utilities
// ------------------

// TAB separator between fields

#define FIELD(f) \
    rom f##_str __attribute__((unused)) = field_start;  \
    unsigned    f##_len __attribute__((unused)) = field_len

#define GET_NEXT_ITEM(f) \
    field_start = next_field; \
    next_field = seg_get_next_item (VB, field_start, &len, GN_FORBIDEN, GN_SEP, GN_IGNORE, &field_len, &separator, NULL, #f); \
    FIELD (f)

#define SEG_NEXT_ITEM(f) \
    GET_NEXT_ITEM (f); \
    seg_by_did (VB, field_start, field_len, f, field_len+1)

#define GET_LAST_ITEM(f) \
    field_start = next_field; \
    next_field = seg_get_next_item (VB, field_start, &len, GN_SEP, GN_FORBIDEN, GN_IGNORE, &field_len, &separator, has_13, #f); \
    FIELD (f)

#define SEG_LAST_ITEM(f) \
    GET_LAST_ITEM (f);\
    seg_by_did (VB, field_start, field_len, f, field_len+1)

#define GET_MAYBE_LAST_ITEM(f) \
    field_start = next_field; \
    next_field = seg_get_next_item (VB, field_start, &len, GN_SEP, GN_SEP, GN_IGNORE, &field_len, &separator, has_13, #f); \
    FIELD (f)

#define SEG_MAYBE_LAST_ITEM(f)  \
    GET_MAYBE_LAST_ITEM (f); \
    seg_by_did (VB, field_start, field_len, f, field_len+1)

// SPACE separator between fields

#define GET_NEXT_ITEM_SP(f) \
    field_start = next_field; \
    next_field = seg_get_next_item (VB, field_start, &len, GN_FORBIDEN, GN_SEP, GN_SEP, &field_len, &separator, NULL, #f); \
    FIELD (f)

#define SEG_NEXT_ITEM_SP(f) \
    GET_NEXT_ITEM_SP (f); \
    seg_by_did (VB, field_start, field_len, f, field_len+1); 

#define GET_LAST_ITEM_SP(f)  \
    field_start = next_field; \
    next_field = seg_get_next_item (VB, field_start, &len, GN_SEP, GN_FORBIDEN, GN_FORBIDEN, &field_len, &separator, has_13, #f); \
    FIELD (f)

#define SEG_LAST_ITEM_SP(f)  \
    GET_LAST_ITEM_SP (f); \
    seg_by_did (VB, field_start, field_len, f, field_len+1)

#define GET_MAYBE_LAST_ITEM_SP(f)  \
    field_start = next_field; \
    next_field = seg_get_next_item (VB, field_start, &len, GN_SEP, GN_SEP, GN_SEP, &field_len, &separator, has_13, #f); \
    FIELD (f)

#define SEG_MAYBE_LAST_ITEM_SP(f)  \
    GET_MAYBE_LAST_ITEM_SP (f); \
    seg_by_did (VB, field_start, field_len, f, field_len+1)

// NEWLINE separator

#define GET_NEXT_ITEM_NL(f) \
    field_start = next_field; \
    next_field = seg_get_next_item (VB, field_start, &len, GN_SEP, GN_IGNORE, GN_IGNORE, &field_len, &separator, has_13, #f); \
    FIELD (f)

#define SEG_EOL(f,account_for_ascii10) ({ seg_by_did (VB, *(has_13) ? "\r\n" : "\n", 1 + *(has_13), (f), (account_for_ascii10) + *(has_13)); })

extern StrTextLong seg_error (VBlockP vb);

#define ASSSEG(condition, format, ...)                                                                  \
    ASSINP ((condition), "%s %s:%s: " format "%s%s\n", str_time().s, txt_name, LN_NAME, __VA_ARGS__,    \
            seg_error(VB).s, DT_FUNC (vb, assseg_line)((VBlockP)(vb)))

#define ASSSEG0(condition, err_str) ASSSEG (condition, err_str "%s", "")

#define ASSSEGNOTNULL(p)    ASSSEG0 (p, #p" is NULL")

#define ABOSEG(format, ...) ASSSEG(false, format, __VA_ARGS__)

#define ABOSEG0(err_str)    ASSSEG(false, err_str "%s", "")
