// ------------------------------------------------------------------
//   piz.h
//   Copyright (C) 2019-2021 Black Paw Ventures Limited
//   Please see terms and conditions in the file LICENSE.txt

#pragma once

#include "genozip.h"
#include "dispatcher.h"

extern bool piz_default_skip_section (VBlockP vb, SectionType st, DictId dict_id);
#define piz_is_skip_section(vb,st,dict_id) (vb->data_type     != DT_NONE && (piz_default_skip_section ((VBlockP)(vb), (st), (dict_id)) || (DT_FUNC (vb, is_skip_section)((VBlockP)(vb), (st), (dict_id)))))
#define piz_is_skip_sectionz(st,dict_id)   (z_file->data_type != DT_NONE && (piz_default_skip_section (NULL, (st), (dict_id)) || (DTPZ(is_skip_section) && DTPZ(is_skip_section)(NULL, (st), (dict_id)))))

extern Dispatcher piz_z_file_initialize (bool is_last_z_file);
extern DataType piz_read_global_area (Reference ref);
extern bool piz_one_txt_file (Dispatcher dispatcher, bool is_first_z_file);
extern uint32_t piz_uncompress_all_ctxs (VBlockP vb, uint32_t pair_vb_i);

extern bool piz_grep_match (const char *start, const char *after);
extern bool piz_test_grep (VBlockP vb);

extern ContextP piz_multi_dict_id_get_ctx_first_time (VBlockP vb, ContextP ctx, unsigned num_dict_ids, STRp(snip), unsigned ctx_i);
#define MCTX(ctx_i,num_dict_ids,snip,snip_len) (ctx->con_cache.len && *ENT(ContextP, ctx->con_cache, ctx_i)) \
                                                    ? *ENT(ContextP, ctx->con_cache, ctx_i)                  \
                                                    : piz_multi_dict_id_get_ctx_first_time ((VBlockP)vb, ctx, (num_dict_ids), (snip), (snip_len), (ctx_i))

typedef struct { char s[100]; } PizDisCoords; 
extern PizDisCoords piz_dis_coords (VBlockP vb); // for ASSPIZ

#define ASSPIZ(condition, format, ...) do { if (!(condition)) { progress_newline(); fprintf (stderr, "Error in %s:%u vb_i=%u line_i=%"PRIu64" line_in_vb=%"PRId64"%s: ",     __FUNCTION__, __LINE__, vb->vblock_i, vb->line_i, vb->line_i ? (vb->line_i - vb->first_line) : 0, piz_dis_coords(vb).s); fprintf (stderr, (format), __VA_ARGS__); fprintf (stderr, "\n"); exit_on_error(true); }} while(0)
#define ASSPIZ0(condition, string)     do { if (!(condition)) { progress_newline(); fprintf (stderr, "Error in %s:%u vb_i=%u line_i=%"PRIu64" line_in_vb=%"PRId64"%s: %s\n", __FUNCTION__, __LINE__, vb->vblock_i, vb->line_i, vb->line_i ? (vb->line_i - vb->first_line) : 0, piz_dis_coords(vb).s, string); exit_on_error(true); }} while(0)
