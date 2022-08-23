// ------------------------------------------------------------------
//   lookback.h
//   Copyright (C) 2021-2022 Genozip Limited
//   Please see terms and conditions in the file LICENSE.txt

#pragma once

#include "genozip.h"
#include "sections.h"

extern void lookback_init (VBlockP vb, ContextP lb_ctx, ContextP ctx, StoreType store_type);

extern void lookback_insert (VBlockP vb, Did lb_did_i, Did did_i, bool copy_last_value, ValueType value);
extern void lookback_insert_container (VBlockP vb, ConstContainerP con, unsigned num_items, ContextP *item_ctxs);

extern const void *lookback_get_do (VBlockP vb, ContextP lb_ctx, ContextP ctx, uint32_t lookback);
#define lookback_get_index(vb, lb_ctx, ctx, lookback) (*(WordIndex *)lookback_get_do ((VBlockP)vb, (lb_ctx), (ctx), lookback))
#define lookback_get_value(vb, lb_ctx, ctx, lookback) (*(ValueType *)lookback_get_do ((VBlockP)vb, (lb_ctx), (ctx), lookback))

extern bool lookback_is_same_txt (VBlockP vb, Did lb_did_i, ContextP ctx, uint32_t lookback, STRp(str));

extern uint32_t lookback_get_next (VBlockP vb, ContextP lb_ctx, ContextP ctx, WordIndex search_for, int64_t *iterator);

extern void lookback_flush (VBlockP vb, ConstMediumContainerP con);

extern uint8_t lookback_size_to_local_param (uint32_t size);
