// ------------------------------------------------------------------
//   reconstruct.c
//   Copyright (C) 2019-2022 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#include "reconstruct.h"
#include "vblock.h"
#include "context.h"
#include "file.h"
#include "strings.h"
#include "dict_id.h"
#include "codec.h"
#include "container.h"
#include "flags.h"
#include "piz.h"
#include "base64.h"
#include "regions.h"
#include "lookback.h"
#include "aligner.h"

// parameter is two dict_id's (in base64). reconstructs dict1.last_value - dict2.last_value
SPECIAL_RECONSTRUCTOR (piz_special_MINUS)
{
    // decode and store the the contexts in the first call for ctx (only one MINUS snip allowed per ctx)
    if (!ctx->con_cache.len32) {
        buf_alloc_zero (vb, &ctx->con_cache, 0, 2, ContextP, 1, "con_cache");

        DictId two_dicts[2];
        base64_decode (snip, &snip_len, (uint8_t *)two_dicts);

        *B(ContextP, ctx->con_cache, 0) = ECTX (two_dicts[0]);
        *B(ContextP, ctx->con_cache, 1) = ECTX (two_dicts[1]);
    }

    new_value->i = (*B(ContextP, ctx->con_cache, 0))->last_value.i - 
                   (*B(ContextP, ctx->con_cache, 1))->last_value.i;

    if (reconstruct)
        RECONSTRUCT_INT (new_value->i); 

    return HAS_NEW_VALUE; 
}

// Compute threads: decode the delta-encoded value of the POS field, and returns the new lacon_pos
// Special values:
// "-" - negated previous value
// ""  - negated previous delta
static int64_t reconstruct_from_delta (VBlockP vb, 
                                       ContextP my_ctx,   // use and store last_delta
                                       ContextP base_ctx, // get last_value
                                       STRp(delta_snip),
                                       bool reconstruct) 
{
    ASSISLOADED (base_ctx);
    ASSPIZ0 (delta_snip, "delta_snip is NULL");
    ASSPIZ (base_ctx->flags.store == STORE_INT, "reconstructing %s - calculating delta \"%.*s\" from a base of %s, but %s, doesn't have STORE_INT",
            my_ctx->tag_name, STRf(delta_snip), base_ctx->tag_name, base_ctx->tag_name);

    int64_t base_value = (my_ctx->flags.same_line && my_ctx != base_ctx)
        ? reconstruct_peek (vb, base_ctx, 0, 0).i // value of this line/sample - whether already encountered or peek a future value
        : base_ctx->last_value.i; // use last_value even if base_ctx not encountered yet

    bool hex = delta_snip_len && delta_snip[0] == 'x';
    if (hex) {
        delta_snip_len--;
        delta_snip++;
    }

    if (delta_snip_len == 1 && delta_snip[0] == '-')
        my_ctx->last_delta = -2 * base_value; // negated previous value

    else if (!delta_snip_len)
        my_ctx->last_delta = -my_ctx->last_delta; // negated previous delta

    else 
        my_ctx->last_delta = (int64_t)strtoull (delta_snip, NULL, 10 /* base 10 */); // strtoull can handle negative numbers, despite its name

    int64_t new_value = base_value + my_ctx->last_delta;  
    if (reconstruct && !hex)
        RECONSTRUCT_INT (new_value);
    else if (reconstruct && hex)
        RECONSTRUCT_HEX (new_value, false);

    return new_value;
}

#define ASSERT_IN_BOUNDS \
    ASSPIZ (ctx->next_local < ctx->local.len32, \
            "unexpected end of ctx->local data in %s (len=%u next_local=%u ltype=%s lcodec=%s did_i=%u preprocessing=%u). %s", \
            ctx->tag_name, ctx->local.len32, ctx->next_local, lt_name (ctx->ltype), codec_name (ctx->lcodec), ctx->did_i, vb->preprocessing, ctx->local.len ? "" : "since len=0, perhaps a Skip function issue?");

#define ASSERT_IN_BOUNDS_BEFORE(recon_len) \
    ASSPIZ (ctx->next_local + (recon_len) <= ctx->local.len, \
            "unexpected end of ctx->local data in %s (len=%u next_local=%u ltype=%s lcodec=%s did_i=%u, preprocessing=%u)", \
            ctx->tag_name, ctx->local.len32, ctx->next_local, lt_name (ctx->ltype), codec_name (ctx->lcodec), ctx->did_i, vb->preprocessing)

static uint32_t reconstruct_from_local_text (VBlockP vb, ContextP ctx, bool reconstruct)
{
    uint32_t start = ctx->next_local; 
    ARRAY (char, data, ctx->local);

    while (ctx->next_local < ctx->local.len && data[ctx->next_local] != 0) ctx->next_local++;
    ASSERT_IN_BOUNDS;

    rom snip = &data[start];
    uint32_t snip_len = ctx->next_local - start; 
    ctx->next_local++; // skip the separator 

    reconstruct_one_snip (vb, ctx, WORD_INDEX_NONE, STRa(snip), reconstruct);

    return snip_len;
}

static void reconstruct_from_diff (VBlockP vb, ContextP ctx, STRp(snip), bool reconstruct)
{
    ContextP base_ctx;
    if (snip_len >= 10) // a base64 dict_id is of length 14, larger than the largest uint32_t = 10  
        base_ctx = reconstruct_get_other_ctx_from_snip (vb, ctx, pSTRa(snip)); // also updates snip and snip_len
    else {
        base_ctx = ctx;
        snip++;
        snip_len--;
    }
    STR(base);

    // case: we get a value from the same line - set last_txt for the value on this line
    // note: we don't check here that there is actually a value on the line - the segger should do that
    if (ctx->flags.same_line && ctx != base_ctx) 
        reconstruct_peek (vb, base_ctx, pSTRa(base));
    
    else {
        base = last_txtx (vb, base_ctx);
        base_len = base_ctx->last_txt.len;
    }
        
    int64_t diff_len;
    ASSPIZ (str_get_int (STRa(snip), &diff_len), "In ctx=%s: Invalid XOR_DIFF snip: \"%.*s", ctx->tag_name, STRf(snip));
    bool exact = diff_len < 0; // a negative number indicates an exact match - no xor in local
    diff_len = ABS(diff_len);

    bytes diff = B8 (ctx->local, ctx->next_local); 
    uint8_t *recon = BAFT8 (vb->txt_data);

    if (exact)
        memcpy (recon, base, diff_len);

    else { 
        ASSERT_IN_BOUNDS_BEFORE(diff_len);
    
        if (VER(14))
            for (int64_t i=0; i < diff_len; i++)
                recon[i] = diff[i] ? diff[i] : base[i];

        else // up to v13 this was a xor
            for (int64_t i=0; i < diff_len; i++)
                recon[i] = base[i] ^ diff[i];

        ctx->next_local += diff_len;    
    }

    vb->txt_data.len32 += diff_len;
}

int64_t reconstruct_from_local_int (VBlockP vb, ContextP ctx, char separator /* 0 if none */, bool reconstruct)
{
    ASSERT_IN_BOUNDS;

    int64_t num=0;

    switch (ctx->ltype) {
        case LT_UINT8 : case LT_hex8 : case LT_HEX8 :  num = NEXTLOCAL(uint8_t,  ctx); break;
        case LT_INT8  :                                num = NEXTLOCAL(int8_t,   ctx); break;
        case LT_UINT16: case LT_hex16: case LT_HEX16:  num = NEXTLOCAL(uint16_t, ctx); break;
        case LT_INT16 :                                num = NEXTLOCAL(int16_t,  ctx); break;
        case LT_UINT32: case LT_hex32: case LT_HEX32:  num = NEXTLOCAL(uint32_t, ctx); break;
        case LT_INT32 :                                num = NEXTLOCAL(int32_t,  ctx); break;
        case LT_UINT64: case LT_hex64: case LT_HEX64:  num = NEXTLOCAL(uint64_t, ctx); break;
        case LT_INT64 :                                num = NEXTLOCAL(int64_t,  ctx); break;
        default: 
            ASSPIZ (false, "Unexpected ltype=%s(%u) for ctx=\"%s\"", lt_name(ctx->ltype), ctx->ltype, ctx->tag_name); 
    }

    if (reconstruct) { 
        if (VB_DT(VCF) && num==lt_desc[ctx->ltype].max_int && dict_id_is_vcf_format_sf (ctx->dict_id)
            && !lt_desc[ctx->ltype].is_signed) {
            RECONSTRUCT1 ('.');
            num = 0; // we consider FORMAT fields that are . to be 0.
        }

        else if (ctx->ltype >= LT_hex8)
            RECONSTRUCT_HEX (num, (ctx->ltype & 1)); // odd-number hex ltypes are upper case

        else 
            RECONSTRUCT_INT (num);
        
        if (separator) RECONSTRUCT1 (separator);
    }

    return num;
}

int64_t reconstruct_peek_local_int (VBlockP vb, ContextP ctx, int offset /*0=next_local*/)
{
    ASSERT_IN_BOUNDS;

    int64_t num=0;

    switch (ctx->ltype) {
        case LT_UINT8:  num = PEEKNEXTLOCAL(uint8_t,  ctx, offset); break;
        case LT_UINT32: num = PEEKNEXTLOCAL(uint32_t, ctx, offset); break;
        case LT_INT8:   num = PEEKNEXTLOCAL(int8_t,   ctx, offset); break;
        case LT_INT32:  num = PEEKNEXTLOCAL(int32_t,  ctx, offset); break;
        case LT_UINT16: num = PEEKNEXTLOCAL(uint16_t, ctx, offset); break;
        case LT_INT16:  num = PEEKNEXTLOCAL(int16_t,  ctx, offset); break;
        case LT_UINT64: num = PEEKNEXTLOCAL(uint64_t, ctx, offset); break;
        case LT_INT64:  num = PEEKNEXTLOCAL(int64_t,  ctx, offset); break;
        default: 
            ASSPIZ (false, "Unexpected ltype=%s(%u)", lt_name(ctx->ltype), ctx->ltype); 
    }

    if (VB_DT(VCF) && num==lt_desc[ctx->ltype].max_int && dict_id_is_vcf_format_sf (ctx->dict_id)
        && !lt_desc[ctx->ltype].is_signed)
        return 0; // returns 0 if '.'

    return num;
}

static double reconstruct_from_local_float (VBlockP vb, ContextP ctx, 
                                            STRp(format), // required unless not reconstructing (eg a binary field - CI0_TRANS_NOR in the container)
                                            char separator /* 0 if none */, bool reconstruct)
{   
    ASSERT_IN_BOUNDS;

    float num=0;

    switch (ctx->ltype) {
        case LT_FLOAT32: num = NEXTLOCAL(float,  ctx); break;
        case LT_FLOAT64: num = NEXTLOCAL(double, ctx); break;
        default: 
            ASSPIZ (false, "Unexpected ltype=%s(%u) in ctx=%s", lt_name(ctx->ltype), ctx->ltype, ctx->tag_name); 
    }

    if (reconstruct) { 
        ASSPIZ (format_len, "Failed to reconstruct a float in ctx=%s, because format was not given", ctx->tag_name);

        // format is as generated by str_get_float - %8.3f. each of the two numbers can be 1 or 2 digits.
        SAFE_NULT (format);
        sprintf (BAFTtxt, format, num);      
        vb->txt_data.len += (format[2] == '.' ? (format[1]-'0') : ((format[1]-'0') * 10 + format[2]-'0'));
        SAFE_RESTORE;

        if (separator) RECONSTRUCT1 (separator);
    }

    return num;
}

// two options: 1. the length maybe given (textually) in snip/snip_len. in that case, it is used and vb->seq_len is updated.
// if snip_len==0, then the length is taken from vb->seq_len.
// NOTE: this serves nucleotide sequences AND qual. Bad design. Should have been two separate things.
uint32_t reconstruct_from_local_sequence (VBlockP vb, ContextP ctx, STRp(snip), bool reconstruct)
{
    ASSERTNOTNULL (ctx);

    uint32_t len = snip_len ? atoi (snip) : vb->seq_len;

    if (!ctx->is_loaded) return len;

    // special case: handle SAM_QUAL missing quality (expressed as a ' ')
    if (*Bc(ctx->local, ctx->next_local) == ' ' && (ctx->did_i == SAM_QUAL && (VB_DT(BAM) || VB_DT(SAM)))) {
        len = 1;
        sam_reconstruct_missing_quality (vb, reconstruct);
    }

    else {
        ASSPIZ (ctx->next_local + len <= ctx->local.len, "unexpected end of %s data: expecting ctx->next_local=%u + seq_len=%u <= local.len=%u", 
                ctx->tag_name, ctx->next_local, len, ctx->local.len32);

        if (reconstruct) RECONSTRUCT (Bc(ctx->local, ctx->next_local), len);
    }

    ctx->last_value.i = ctx->next_local; // for seq_qual, we use last_value for storing the beginning of the sequence
    ctx->next_local += len;

    return len;
}

ContextP reconstruct_get_other_ctx_from_snip (VBlockP vb, ContextP ctx, pSTRp (snip))
{
    unsigned b64_len = base64_sizeof (DictId);
    char err[*snip_len+20];
    ASSPIZ (b64_len + 1 <= *snip_len, "ctx=%s snip=\"%s\" snip_len=%u but expecting it to be >= %u", 
            ctx->tag_name, str_print_snip(*snip, *snip_len, err), *snip_len, b64_len + 1);

    DictId dict_id;
    base64_decode ((*snip)+1, &b64_len, dict_id.id);

    ContextP other_ctx = ECTX (dict_id);
    ASSPIZ (other_ctx, "Failed to get other context: ctx=%s snip=%.*s other_dict_id=%s", 
            ctx->tag_name, STRf(*snip), dis_dict_id(dict_id).s);
  
    *snip     += b64_len + 1;
    *snip_len -= b64_len + 1;

    ctx->other_did_i = other_ctx->did_i;

    return other_ctx;
}

// get ctx from a multi-dict_id special snip. note that we're careful to only ECTX the ctx_i requested, and not all,
// so that we don't do a full search of vb->contexts[] for a channel that was not segged and hence has no context
ContextP recon_multi_dict_id_get_ctx_first_time (VBlockP vb, ContextP ctx, STRp(snip), unsigned ctx_i)
{
    if (!ctx->con_cache.len) {
        ctx->con_cache.len = str_count_char (STRa(snip), '\t') + 1;
        buf_alloc_zero (vb, &ctx->con_cache, 0, ctx->con_cache.len, ContextP, 1, "con_cache");
    }

    // note: we get past this point only once per VB, per ctx_i
    str_split (snip, snip_len, ctx->con_cache.len32, '\t', item, true);
    ASSPIZ (n_items, "Unable to decoded multi-dict-id snip for %s. snip=\"%.*s\"", ctx->tag_name, STRf(snip));

    DictId item_dict_id;
    base64_decode (items[ctx_i], &item_lens[ctx_i], item_dict_id.id);

    return (*B(ContextP, ctx->con_cache, ctx_i) = ECTX (item_dict_id)); // NULL if no data was segged to this channel    
}

static ValueType reconstruct_from_lookback (VBlockP vb, ContextP ctx, STRp(snip), bool reconstruct)
{   
    ContextP lb_ctx = SCTX(snip);
    int64_t lookback = lb_ctx->last_value.i;
    ValueType value = {};

    // a lookback by word_index
    if (!snip_len) { 
        value.i = lookback_get_index (vb, lb_ctx, ctx, lookback);
        
        STR(back_snip);
        ctx_get_snip_by_word_index (ctx, value.i, back_snip);

        if (reconstruct) RECONSTRUCT (back_snip, back_snip_len);
    }

    // a lookback by txt
    else if (snip_len == 1 && (*snip >= 'T' && *snip <= 'z')) { // maximum supported - 122(=z)-84(=T)+1 = 39
        ValueType back_value = lookback_get_value (vb, lb_ctx, ctx, lookback * (*snip - 'T' + 1));

        if (reconstruct) 
            RECONSTRUCT (Btxt (back_value.index), back_value.len);
    }
    
    // a lookback by delta vs integer
    else { 
        ValueType back_value = lookback_get_value (vb, lb_ctx, ctx, lookback);

        PosType delta;
        ASSPIZ  (str_get_int (STRa(snip), &delta), "Invalid delta snip \"%.*s\"", STRf(snip));

        value.i = back_value.i + delta;
        
        if (reconstruct) RECONSTRUCT_INT (value.i);
    }

    return value; 
}

// called from SPECIAL demultiplexor
HasNewValue reconstruct_demultiplex (VBlockP vb, ContextP ctx, STRp(snip), int channel_i, ValueType *new_value, bool reconstruct)
{
    ContextP channel_ctx = MCTX (channel_i, snip, snip_len);
    ASSPIZ (channel_ctx, "Cannot find channel context of channel_i=%d of multiplexed context %s", channel_i, ctx->tag_name);

    reconstruct_from_ctx (vb, channel_ctx->did_i, 0, reconstruct);

    if (ctx->flags.store == STORE_NONE) return NO_NEW_VALUE;

    // propagate last_value up
    new_value->i = channel_ctx->last_value.i; // note: last_value is a union, this copies the entire union
    return HAS_NEW_VALUE; 
}

static HasNewValue reconstruct_numeric (VBlockP vb, ContextP ctx, STRp(snip), ValueType *new_value, bool reconstruct)
{
    new_value->i = reconstruct_from_local_int (vb, ctx, 0, false);
    
    if (reconstruct) {
        char format[32] = "%.*s%0*";

        if (new_value->i <= 0xffffffffLL)
            format[7] = "uxX"[snip[1] - '0'];

        else // beyond 32b uint
            switch (snip[1] - '0') { // snip[1] is type
                case 0: memcpy (&format[7], PRIu64, STRLEN(PRIu64)); break;
                case 1: memcpy (&format[7], PRIx64, STRLEN(PRIx64)); break;
                case 2: memcpy (&format[7], PRIX64, STRLEN(PRIX64)); break;
            }

        vb->txt_data.len32 += sprintf (BAFTtxt, format, 
                                       snip_len-3, &snip[3], // prefix - since 14.0.18
                                       snip[2]-'0', new_value->i); // snip[2] is width
    }

    return HAS_NEW_VALUE;
}

void reconstruct_one_snip (VBlockP vb, ContextP snip_ctx, 
                           WordIndex word_index, // WORD_INDEX_NONE if not used.
                           STRp(snip), bool reconstruct) // if false, calculates last_value but doesn't output to vb->txt_data)
{
    ValueType new_value = {};
    HasNewValue has_new_value = NO_NEW_VALUE;
    int64_t prev_value = snip_ctx->last_value.i;
    ContextP base_ctx = snip_ctx; // this will change if the snip refers us to another data source
    StoreType store_type = snip_ctx->flags.store;
    bool store_delta = VER(12) && snip_ctx->flags.store_delta; // note: the flag was used for something else in v8

    // case: empty snip
    if (!snip_len) {
        if (store_type == STORE_INDEX && word_index != WORD_INDEX_NONE) {
            new_value.i = word_index;
            has_new_value = HAS_NEW_VALUE;
        }
        goto done;
    }

    switch (snip[0]) {

    // display the rest of the snip first, and then the lookup up text.
    case SNIP_LOOKUP:
    case SNIP_OTHER_LOOKUP: {
        if (snip[0] == SNIP_LOOKUP) 
            { snip++; snip_len--; }
        else 
            // we are request to reconstruct from another ctx
            base_ctx = reconstruct_get_other_ctx_from_snip (vb, snip_ctx, pSTRa(snip)); // also updates snip and snip_len

        switch (base_ctx->ltype) {
            case LT_TEXT:
                if (snip_len && reconstruct) RECONSTRUCT_snip; // reconstruct this snip before adding the looked up data
                reconstruct_from_local_text (vb, base_ctx, reconstruct); // this will call us back recursively with the snip retrieved
                break;
                
            case LT_CODEC: // snip can optionally be the length of the sequence to be reconstructed
                codec_args[base_ctx->lcodec].reconstruct (vb, base_ctx->lcodec, base_ctx, STRa(snip)); break;
                break;

            case LT_INT8 ... LT_UINT64: case LT_hex8 ... LT_HEX64:
                if (reconstruct && snip_len) RECONSTRUCT_snip; // reconstruct this snip before adding the looked up data
                new_value.i = reconstruct_from_local_int (vb, base_ctx, 0, reconstruct);
                has_new_value = HAS_NEW_VALUE;
                break;

            case LT_FLOAT32 ... LT_FLOAT64:
                new_value.f = reconstruct_from_local_float (vb, base_ctx, STRa(snip), 0, reconstruct);
                has_new_value = HAS_NEW_VALUE;
                break;
            
            // case: the snip is taken to be the length of the sequence (or if missing, the length will be taken from vb->seq_len)
            case LT_SEQUENCE: 
                reconstruct_from_local_sequence (vb, base_ctx, STRa(snip), reconstruct);
                break;
                
            case LT_BITMAP:
                ASSERT_DT_FUNC (vb, reconstruct_seq);
                DT_FUNC (vb, reconstruct_seq) (vb, base_ctx, STRa(snip), reconstruct);
                break;

            default: ABORT ("%s: while reconstructing %s: Unsupported lt_type=%s (%u) for SNIP_LOOKUP or SNIP_OTHER_LOOKUP. Please upgrade to the latest version of Genozip.", 
                            LN_NAME, base_ctx->tag_name, lt_name(base_ctx->ltype), base_ctx->ltype);
        }

        break;
    }

    case SNIP_NUMERIC: // 0-padded fixed-width non-negative decimal or hexadecimal integer (v14)
        has_new_value = reconstruct_numeric (vb, snip_ctx, STRa(snip), &new_value, reconstruct);
        break;

    case SNIP_CONTAINER: {
        STR(prefixes);
        ContainerP con_p = container_retrieve (vb, snip_ctx, word_index, snip+1, snip_len-1, pSTRa(prefixes));
        new_value = container_reconstruct (vb, snip_ctx, con_p, prefixes, prefixes_len); 
        has_new_value = HAS_NEW_VALUE;
        break;
    }

    case SNIP_SELF_DELTA:
        new_value.i = reconstruct_from_delta (vb, snip_ctx, base_ctx, snip+1, snip_len-1, reconstruct);
        has_new_value = HAS_NEW_VALUE;
        break;

    case SNIP_OTHER_DELTA: 
        base_ctx = reconstruct_get_other_ctx_from_snip (vb, snip_ctx, pSTRa(snip)); // also updates snip and snip_len
        new_value.i = reconstruct_from_delta (vb, snip_ctx, base_ctx, STRa(snip), reconstruct); 
        has_new_value = HAS_NEW_VALUE;
        break;

    case SNIP_COPY: 
        base_ctx = (snip_len==1) ? snip_ctx : reconstruct_get_other_ctx_from_snip (vb, snip_ctx, pSTRa(snip)); 
        RECONSTRUCT (last_txtx (vb, base_ctx), base_ctx->last_txt.len);
        new_value = base_ctx->last_value; 
        has_new_value = HAS_NEW_VALUE;
        break;

    case SNIP_SPECIAL:
        ASSPIZ (snip_len >= 2, "SNIP_SPECIAL expects snip_len=%u >= 2. ctx=\"%s\"", snip_len, snip_ctx->tag_name);
                
        uint8_t special = snip[1] - 32; // +32 was added by SPECIAL macro

        ASSPIZ (special < DTP (num_special), "file requires special handler %u which doesn't exist in this version of genozip - please upgrade to the latest version", special);
        ASSERT_DT_FUNC (vb, special);

        has_new_value = DT_FUNC(vb, special)[special](vb, snip_ctx, snip+2, snip_len-2, &new_value, reconstruct);  
        break;

    case SNIP_DIFF:
        reconstruct_from_diff (vb, snip_ctx, snip, snip_len, reconstruct);
        break;

    case SNIP_REDIRECTION: 
        base_ctx = reconstruct_get_other_ctx_from_snip (vb, snip_ctx, pSTRa(snip)); // also updates snip and snip_len
        reconstruct_from_ctx (vb, base_ctx->did_i, 0, reconstruct);
        break;
    
    case SNIP_DUAL: {
        str_split (&snip[1], snip_len-1, 2, SNIP_DUAL, subsnip, true);
        ASSPIZ (n_subsnips==2, "Invalid SNIP_DUAL snip in ctx=%s", snip_ctx->tag_name);

        if (vcf_vb_is_primary(vb)) // recursively call for each side 
            reconstruct_one_snip (vb, snip_ctx, word_index, STRi(subsnip,0), reconstruct);
        else
            reconstruct_one_snip (vb, snip_ctx, word_index, STRi(subsnip,1), reconstruct);
        return;
    }

    case SNIP_LOOKBACK: 
        new_value = reconstruct_from_lookback (vb, base_ctx, STRa(snip), reconstruct);
        has_new_value = HAS_NEW_VALUE;
        break;

    case NUM_SNIP_CODES ... 31:
        ABORT ("%s: File %s requires a SNIP code=%u for %s. Please upgrade to the latest version of Genozip",
               LN_NAME, z_name, snip[0], base_ctx->tag_name);

    case SNIP_DONT_STORE:
        store_type  = STORE_NONE; // override store and fall through
        store_delta = false;
        snip++; snip_len--;
        goto normal_snip;

    // note: starting v14, this is replaced by SAM_SPECIAL_COPY_BUDDY
    case v13_SNIP_COPY_BUDDY: {
        has_new_value = sam_piz_special_COPY_BUDDY (vb, base_ctx, snip+1, snip_len-1, &new_value, reconstruct);
        break;
    }
    // note: starting v14, this is replaced by FASTQ_SPECIAL_PAIR2_GPOS
    case v13_SNIP_FASTQ_PAIR2_GPOS: 
        has_new_value = fastq_special_PAIR2_GPOS (vb, snip_ctx, snip+1, snip_len-1, &new_value, false);
        break;

    // note: starting v14, this is replaced by FASTQ_SPECIAL_mate_lookup
    case v13_SNIP_MATE_LOOKUP: 
        fastq_special_mate_lookup (vb, snip_ctx, 0, 0, 0, reconstruct);
        break;

    default: normal_snip: {
        if (reconstruct) RECONSTRUCT_snip; // simple reconstruction

        switch (store_type) {
            case STORE_INT: 
                // store the value only if the snip in its entirety is a reconstructable integer (eg NOT "21A", "-0", "012" etc)
                has_new_value = str_get_int (snip, snip_len, &new_value.i);
                break;

            case STORE_FLOAT: {
                char *after;
                new_value.f = strtod (snip, &after); // allows negative values

                // if the snip in its entirety is not a valid number, don't store the value.
                // this can happen for example when seg_pos_field stores a "nonsense" snip.
                has_new_value = (after == snip + snip_len);
                break;
            }
            case STORE_INDEX:
                new_value.i = word_index;
                has_new_value = (word_index != WORD_INDEX_NONE);
                break;

            default: {} // do nothing
        }

        snip_ctx->last_delta = 0; // delta is 0 since we didn't calculate delta
    }
    }

done:
    // update last_value if needed
    if (has_new_value && store_type) // note: we store in our own context, NOT base (a context, eg FORMAT/DP, sometimes serves as a base_ctx of MIN_DP and sometimes as the snip_ctx for INFO_DP)
        ctx_set_last_value (vb, snip_ctx, new_value); // if marely encountered it is set in is set in reconstruct_from_ctx_do

    // note: if store_delta, we do a self-delta. this overrides last_delta set by the delta snip which could be against a different
    // base_ctx. note: when Seg sets last_delta, it must also set store=STORE_INT
    if (store_delta) 
        snip_ctx->last_delta = new_value.i - prev_value;
}

// returns reconstructed length or -1 if snip is missing and previous separator should be deleted
int32_t reconstruct_from_ctx_do (VBlockP vb, Did did_i, 
                                 char sep, // if non-zero, outputs after the reconstruction
                                 bool reconstruct, // if false, calculates last_value but doesn't output to vb->txt_data
                                 rom func)
{
    ASSPIZ (did_i < vb->num_contexts, "called from: %s: did_i=%u out of range: vb->num_contexts=%u", func, did_i, vb->num_contexts);

    ContextP ctx = CTX(did_i);

    // if we're peeking, freeze the context
    if (vb->peek_stack_level) 
        recon_stack_push (vb, ctx);

    ASSPIZ0 (ctx->dict_id.num || ctx->did_i != DID_NONE, "ctx not initialized (dict_id=0)");

    // update ctx, if its an alias (only for primary field aliases as they have contexts, other alias don't have ctx)
    if (!ctx->dict_id.num) 
        ctx = CTX(ctx->did_i); // ctx->did_i is different than did_i if its an alias

    uint32_t last_txt_index = (uint32_t)vb->txt_data.len32;

    // case: we have b250 data
    if (ctx->b250.len32 ||
        (!ctx->b250.len32 && !ctx->local.len32 && ctx->dict.len)) {  // all_the_same case - no b250 or local, but have dict      
        STR0(snip);
        WordIndex word_index = LOAD_SNIP(ctx->did_i); // note: if we have no b250, local but have dict, this will be word_index=0 (see ctx_get_next_snip)

        if (!snip) goto missing;

        reconstruct_one_snip (vb, ctx, word_index, STRa(snip), reconstruct);        

        // if SPECIAL function set value_is_missing (eg vcf_piz_special_PS_by_PID) - this treated as a WORD_INDEX_MISSING 
        if (ctx->value_is_missing) {
            ctx->value_is_missing = false;
            goto missing;
        }

        // for backward compatability with v8-11 that didn't yet have flags.store = STORE_INDEX for CHROM
        if (did_i == DTF(prim_chrom)) { // NOTE: CHROM cannot have aliases, because looking up the did_i by dict_id will lead to CHROM, and this code will be executed for a non-CHROM field
            if (!ctx_has_value_in_line_(vb, CTX(did_i))) 
                vb->last_index (did_i) = word_index;
            
            vb->chrom_node_index = vb->last_index (did_i); 
            if (word_index != vb->chrom_node_index) // eg if word_index is a SPECIAL and the special reconstructor returned the ultimate word index
                ctx_get_snip_by_word_index (CTX(did_i), vb->chrom_node_index, vb->chrom_name);
            else
                STRset (vb->chrom_name, snip);
        }
    }
    
    // case: all data is only in local
    else if (ctx->local.len32) {
        switch (ctx->ltype) {
        case LT_INT8 ... LT_UINT64 : case LT_hex8 ... LT_HEX64: {
            int64_t value = reconstruct_from_local_int (vb, ctx, 0, reconstruct); 

            if (ctx->flags.store == STORE_INT) 
                ctx_set_last_value (vb, ctx, value);

            break;
        }
        case LT_CODEC:
            codec_args[ctx->lcodec].reconstruct (vb, ctx->lcodec, ctx, NULL, 0); break;

        case LT_SEQUENCE: 
            reconstruct_from_local_sequence (vb, ctx, NULL, 0, reconstruct); break;
                
        case LT_BITMAP:
            ASSERT_DT_FUNC (vb, reconstruct_seq);
            DT_FUNC (vb, reconstruct_seq) (vb, ctx, NULL, 0, reconstruct);
            break;
        
        case LT_TEXT:
            reconstruct_from_local_text (vb, ctx, reconstruct); break;

        default:
            ASSPIZ (false, "Invalid ltype=%u in ctx=%s", ctx->ltype, ctx->tag_name);
        }
    }

    // in case of LT_BITMAP, it is it is ok if the bitmap is empty and all the data is in NONREF (e.g. unaligned SAM)
    else if (ctx->ltype == LT_BITMAP && (ctx+1)->local.len32) {
        ASSERT_DT_FUNC (vb, reconstruct_seq);
        DT_FUNC (vb, reconstruct_seq) (vb, ctx, NULL, 0, reconstruct);
    }

    // case: the entire VB was just \n - so seg dropped the ctx
    // note: for backward compatability with 8.0. for files compressed by 8.1+, it will be handled via the all_the_same mechanism
    else if (ctx->dict_id.num == DTF(eol).num) {
        if (reconstruct) { RECONSTRUCT1('\n'); }
    }

    else ASSPIZ (flag.missing_contexts_allowed,
                 "ctx %s/%s has no data (dict, b250 or local) in did_i=%u ctx->did=%u ctx->dict_id=%s ctx->is_loaded=%s", 
                 dtype_name_z (ctx->dict_id), ctx->tag_name, did_i, ctx->did_i, dis_dict_id (ctx->dict_id).s, TF(ctx->is_loaded));
        
    if (sep && reconstruct) RECONSTRUCT1 (sep); 

    ctx->last_txt = (TxtWord){ .index = last_txt_index,
                               .len   = vb->txt_data.len32 - last_txt_index };

    ctx_set_encountered (vb, ctx); // this is the ONLY place in PIZ where we set encountered
    ctx->last_encounter_was_reconstructed = reconstruct;

    // in "store per line" mode, we save one entry per line (possibly a line has no entries if it is an optional field)
    if (ctx->flags.store_per_line) 
        reconstruct_store_history (vb, ctx); // note: we don't store if spl_custom - a SPECIAL function should do that

    if (vb->peek_stack_level) 
        recon_stack_pop (vb, ctx, false);

    return (int32_t)ctx->last_txt.len;

missing:
    ctx->last_txt.len = 0;

    if (ctx->flags.store == STORE_INDEX) 
        ctx_set_last_value (vb, ctx, (int64_t)WORD_INDEX_MISSING);

    if (vb->peek_stack_level) 
        recon_stack_pop (vb, ctx, false);

    return reconstruct ? -1 : 0; // -1 if WORD_INDEX_MISSING - remove preceding separator
} 
