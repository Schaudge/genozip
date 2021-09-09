// ------------------------------------------------------------------
//   seg.c
//   Copyright (C) 2019-2021 Black Paw Ventures Limited
//   Please see terms and conditions in the file LICENSE.txt

#include "genozip.h"
#include "profiler.h"
#include "seg.h"
#include "vblock.h"
#include "context.h"
#include "endianness.h"
#include "file.h"
#include "strings.h"
#include "optimize.h"
#include "random_access.h"
#include "dict_id.h"
#include "base64.h"
#include "piz.h"
#include "zfile.h"
#include "data_types.h"
#include "container.h"
#include "codec.h"
#include "reference.h"
#include "zip.h"
#include "coords.h"

WordIndex seg_by_ctx_do (VBlock *vb, const char *snip, unsigned snip_len, Context *ctx, uint32_t add_bytes,
                         bool *is_new) // optional out
{
    ASSERTNOTNULL (ctx);

    buf_alloc (vb, &ctx->b250, 1, vb->lines.len, uint32_t, CTX_GROWTH, "contexts->b250");
    
    WordIndex node_index = ctx_evaluate_snip_seg ((VBlockP)vb, ctx, snip, snip_len, is_new);

    ASSERT (node_index < ctx->nodes.len + ctx->ol_nodes.len || node_index == WORD_INDEX_EMPTY || node_index == WORD_INDEX_MISSING, 
            "out of range: dict=%s node_index=%d nodes.len=%u ol_nodes.len=%u",  
            ctx->tag_name, node_index, (uint32_t)ctx->nodes.len, (uint32_t)ctx->ol_nodes.len);
    
    NEXTENT (uint32_t, ctx->b250) = node_index;
    ctx->txt_len += add_bytes;
    
    // a snip that is stored in its entirety in local, with just a LOOKUP in the dictionary, is counted as a singleton
    if (snip_len==1 && (snip[0] == SNIP_LOOKUP))
        ctx->num_singletons++;

    return node_index;
} 

// segs the same node as previous seg
WordIndex seg_known_node_index (VBlockP vb, ContextP ctx, WordIndex node_index, unsigned add_bytes) 
{ 
    buf_alloc (vb, &ctx->b250, 1, vb->lines.len, uint32_t, CTX_GROWTH, NULL);

    NEXTENT (uint32_t, ctx->b250) = node_index; 
    ctx->txt_len += add_bytes;

    return node_index;
}

// rolls back a context to the rollback point registered in seg_create_rollback_point
void seg_rollback (VBlockP vb, Context *ctx)
{
    // if we evaluated this context since the rollback count - undo
    while (ctx->b250.len > ctx->rback_b250_len) {
        WordIndex node_index = *LASTENT (WordIndex, ctx->b250);
        ctx_decrement_count (vb, ctx, node_index);
        ctx->b250.len--;
    }

    ctx->local.len      = ctx->rback_local_len;
    ctx->txt_len        = ctx->rback_txt_len;
    ctx->num_singletons = ctx->rback_num_singletons;
    ctx->last_value     = ctx->rback_last_value;
    ctx->last_delta     = ctx->rback_last_delta;
    ctx->last_txt_index = ctx->rback_last_txt_index;
    ctx->last_txt_len   = ctx->rback_last_txt_len;
    ctx->last_line_i    = LAST_LINE_I_INIT; // undo "encountered in line"
}


void seg_simple_lookup (VBlockP vb, ContextP ctx, unsigned add_bytes)
{
    static const char lookup[1] = { SNIP_LOOKUP };
    seg_by_ctx (vb, lookup, 1, ctx, add_bytes);
}

const char *seg_get_next_item (void *vb_, const char *str, int *str_len, 
                               GetNextAllow newline, GetNextAllow tab, GetNextAllow space,
                               unsigned *len, char *separator, 
                               bool *has_13, // out - only modified if '\r' detected ; only needed if newline=GN_SEP
                               const char *item_name)
{
    VBlockP vb = (VBlockP)vb_;

    unsigned i=0; for (; i < *str_len; i++) {
        char c = str[i];
        if ((tab     == GN_SEP && c == '\t') ||
            (newline == GN_SEP && c == '\n') ||
            (space   == GN_SEP && c == ' ')) {
                *len = i;
                *separator = c;
                *str_len -= i+1;

                // check for Windows-style '\r\n' end of line 
                if (i && c == '\n' && str[i-1] == '\r') {
                    (*len)--;
                    ASSERT0 (has_13, "has_13=NULL but expecting it because newline=GN_SEP");
                    *has_13 = true;
                }

                return str + i+1; // beyond the separator
        }
        else if ((tab     == GN_FORBIDEN && c == '\t') ||
                 (newline == GN_FORBIDEN && c == '\n') ||
                 (space   == GN_FORBIDEN && c == ' ' ) ) break;
    }

    ASSSEG (*str_len, str, "missing %s field", item_name);

    ABOSEG (str, "while segmenting %s: expecting a %s%s%safter \"%.*s\"", 
            item_name,
            newline==GN_SEP ? "NEWLINE " : "", tab==GN_SEP ? "TAB " : "", space==GN_SEP ? "\" \" " : "", 
            MIN_(i, 1000), str);

    return 0; // avoid compiler warning - never reaches here
}

// returns first character after current line
const char *seg_get_next_line (void *vb_, const char *str, 
                               int *remaining_len, // in/out
                               unsigned *len,      // out - length of line exluding \r and \n
                               bool must_have_newline, bool *has_13 /* out */, const char *item_name)
{
    VBlockP vb = (VBlockP)vb_;

    const char *after = str + *remaining_len;
    for (const char *s=str; s < after; s++)
        if (*s == '\n') {
                *len = s - str;
                *remaining_len -= *len + 1;

                // check for Windows-style '\r\n' end of line 
                if (s > str && s[-1] == '\r') {
                    (*len)--;
                    *has_13 = true;
                }

                return str + *len + *has_13 + 1; // beyond the separator
        }
    
    ASSSEG (*remaining_len, str, "missing %s field", item_name);

    // if we reached here - line doesn't end with a newline
    ASSSEG (!must_have_newline, str, "while segmenting %s: expecting a NEWLINE after (showing at most 1000 characters): \"%.*s\"", 
            item_name, MIN_(*remaining_len, 1000), str);

    // we have no newline, but check if last character is a \r
    *has_13 = (after > str) && (after[-1] == '\r');
    *len = *remaining_len - *has_13;
    *remaining_len = 0;

    return after;
}

// returns true is value is of type store_type and stored in last_value
bool seg_set_last_txt (VBlockP vb, ContextP ctx, const char *value, unsigned value_len, StoreType store_type)
{
    bool is_value_in_txt_data = value >= FIRSTENT (char, vb->txt_data) &&
                                value <= LASTENT  (char, vb->txt_data);

    ctx->last_txt_index = is_value_in_txt_data ? ENTNUM (vb->txt_data, value) : INVALID_LAST_TXT_INDEX;
    ctx->last_txt_len = value_len;

    bool stored = false;

    if (store_type == STORE_INT) 
        stored = str_get_int (value, value_len, &ctx->last_value.i);
    
    else if (store_type == STORE_FLOAT) {
        char *after; 
        double f = strtod (value, &after); 
        if ((stored = (after == value + value_len))) 
            ctx->last_value.f = f;
    }

    if (stored) {
        ctx->flags.store = store_type;
        ctx->last_line_i = vb->line_i;    
    }
    else 
        ctx_set_encountered_in_line (ctx);

    return stored;
}

WordIndex seg_integer_do (VBlockP vb, DidIType did_i, int64_t n, unsigned add_bytes)
{
    char snip[40];
    unsigned snip_len = str_int (n, snip);
    return seg_by_did_i (vb, snip, snip_len, did_i, add_bytes);
}

// prepare snips that contain code + dict_id + optional parameter (SNIP_LOOKUP_OTHER, SNIP_OTHER_DELTA, SNIP_REDIRECTION...)
void seg_prepare_snip_other_do (uint8_t snip_code, DictId other_dict_id, bool has_parameter, int32_t parameter, 
                                char *snip, unsigned *snip_len) //  in / out
{
    // make sure we have enough memory
    unsigned required_len = 1 + base64_size (DICT_ID_LEN) + 11 /* max length of a int32 -1000000000 */ + 1 /* \0 */ ;
    ASSERT (*snip_len >= required_len, "*snip_len=%u, but it needs to be at least %u", *snip_len, required_len);

    snip[0] = snip_code;
    *snip_len = 1 + base64_encode (other_dict_id.id, DICT_ID_LEN, &snip[1]);

    if (has_parameter)
        *snip_len += str_int (parameter, &snip[*snip_len]);
}

WordIndex seg_chrom_field (VBlock *vb, const char *chrom_str, unsigned chrom_str_len)
{
    ASSERT0 (chrom_str_len, "chrom_str_len=0");

    bool is_new;
    WordIndex chrom_node_index = seg_by_did_i_ex (vb, chrom_str, chrom_str_len, CHROM, chrom_str_len+1, &is_new);

    // don't allow adding chroms if const_chroms (except "*")
    ASSSEG (!is_new || !flag.const_chroms || (chrom_str_len == 1 && chrom_str[0] == '*'), 
            chrom_str, "contig '%.*s' appears in file, but is missing in the %s header", chrom_str_len, chrom_str, dt_name (txt_file->data_type));
            
    random_access_update_chrom (vb, DC_PRIMARY, chrom_node_index, chrom_str, chrom_str_len);

    return chrom_node_index;
}

// scans a pos field - in case of non-digit or not in the range [0,MAX_POS], either returns -1
// (if allow_nonsense) or errors
static PosType seg_scan_pos_snip (VBlock *vb, const char *snip, unsigned snip_len, bool zero_is_bad, 
                                  SegError *err) // out - if NULL, exception if error
{
    PosType value=0;
    bool is_int = str_get_int (snip, snip_len, &value); // value unchanged if not integer

    if (is_int && value >= !!zero_is_bad && value <= MAX_POS) {
        *err = ERR_SEG_NO_ERROR;
        return value; // all good
    }

    ASSSEG (err, snip, "position field must be an integer number between %u and %"PRId64", seeing: %.*s", 
            !!zero_is_bad, MAX_POS, snip_len, snip);

    *err = is_int ? ERR_SEG_OUT_OF_RANGE : ERR_SEG_NOT_INTEGER;
    return value; // in- or out-of- range integer, or 0 if value is not integer
}

// returns POS value if a valid pos, or 0 if not
PosType seg_pos_field (VBlock *vb, 
                       DidIType snip_did_i,    // mandatory: the ctx the snip belongs to
                       DidIType base_did_i,    // mandatory: base for delta
                       unsigned opt,           // a combination of SPF_* options
                       char missing,           // a character allowed (meaning "missing value"), segged as SNIP_DONT_STORE
                       const char *pos_str, unsigned pos_len, // option 1
                       PosType this_pos,       // option 2
                       unsigned add_bytes)     
{
    Context *snip_ctx = CTX(snip_did_i);
    Context *base_ctx = CTX(base_did_i);

    snip_ctx->no_stons = true;
    base_ctx->flags.store = STORE_INT;

    base_ctx->no_stons = true;

    SegError err = ERR_SEG_NO_ERROR;
    if (pos_str) { // option 1
        if (pos_len == 1 && *pos_str == missing) 
            err = ERR_SEG_NOT_INTEGER; // not an error, just so that we seg this as SNIP_DONT_STORE
        
        else {
            this_pos = seg_scan_pos_snip (vb, pos_str, pos_len, IS_FLAG (opt, SPF_ZERO_IS_BAD), &err);
            ASSERT (IS_FLAG (opt, SPF_BAD_SNIPS_TOO) || !err, "invalid value %.*s in %s vb=%u line_i=%"PRIu64, 
                    pos_len, pos_str, CTX(snip_did_i)->tag_name, vb->vblock_i, vb->line_i);
        }

        // we accept out-of-range integer values for non-self-delta
        if (snip_did_i != base_did_i && err == ERR_SEG_OUT_OF_RANGE) err = ERR_SEG_NO_ERROR;

        if (err) {
            SAFE_ASSIGN (pos_str-1, SNIP_DONT_STORE);
            seg_by_ctx (vb, pos_str-1, pos_len+1, snip_ctx, add_bytes); 
            SAFE_RESTORE;
            snip_ctx->last_delta = 0;  // on last_delta as we're PIZ won't have access to it - since we're not storing it in b250 
            return 0; // invalid pos
        }
    }

    else { // option 2 
        // check out-of-range for self-delta
        if (snip_did_i == base_did_i && (this_pos < 0 || this_pos > MAX_POS)) {
            err = ERR_SEG_OUT_OF_RANGE;
            char snip[15] = { SNIP_DONT_STORE };
            unsigned snip_len = 1 + str_int (this_pos, &snip[1]);
            seg_by_ctx (vb, snip, snip_len, snip_ctx, add_bytes); 
            snip_ctx->last_delta = 0;  // on last_delta as we're PIZ won't have access to it - since we're not storing it in b250 
            return 0; // invalid pos
        }
    }

    PosType pos_delta = this_pos - base_ctx->last_value.i;
    
    ctx_set_last_value (vb, snip_ctx, this_pos);

    // if the delta is too big, add this_pos (not delta) to local and put SNIP_LOOKUP in the b250
    // EXCEPT if it is the first vb (ie last_pos==0) because we want to avoid creating a whole RANDOM_POS
    // section in every VB just for a single entry in case of a nicely sorted file
    if (!IS_FLAG (opt, SPF_UNLIMIED_DELTA) && (pos_delta > MAX_POS_DELTA || pos_delta < -MAX_POS_DELTA) && base_ctx->last_value.i) {

        // store the value in in 32b local
        buf_alloc (vb, &snip_ctx->local, 1, vb->lines.len, uint32_t, CTX_GROWTH, "contexts->local");
        NEXTENT (uint32_t, snip_ctx->local) = BGEN32 (this_pos);
        snip_ctx->txt_len += add_bytes;

        snip_ctx->ltype  = LT_UINT32;

        // add a LOOKUP to b250
        static const char lookup[1] = { SNIP_LOOKUP };
        seg_by_ctx (vb, lookup, 1, snip_ctx, 0);

        snip_ctx->last_delta = 0;  // on last_delta as we're PIZ won't have access to it - since we're not storing it in b250 
        return this_pos;
    }

    // store the delta in last_delta only if we're also putting in the b250
    snip_ctx->last_delta = pos_delta;
    
    // if the delta is the negative of the previous delta (as happens in unsorted BAM files with the second line in
    // each pair of lines) - we just store an empty snippet
    bool is_negated_last = (snip_ctx->last_delta && snip_ctx->last_delta == -pos_delta);

    // case: add a delta
    if (!is_negated_last) {
        
        char pos_delta_str[100]; // more than enough for the base64
        unsigned total_len = sizeof (pos_delta_str);

        if (base_ctx == snip_ctx) {
            pos_delta_str[0] = SNIP_SELF_DELTA;
            total_len = 1;
        }
        else 
            seg_prepare_snip_other (SNIP_OTHER_DELTA, base_ctx->dict_id, false, 0, pos_delta_str, &total_len);

        unsigned delta_len = str_int (pos_delta, &pos_delta_str[total_len]);
        total_len += delta_len;

        seg_by_ctx (vb, pos_delta_str, total_len, snip_ctx, add_bytes);
    }
    // case: the delta is the negative of the previous delta - add a SNIP_SELF_DELTA with no payload - meaning negated delta
    else {
        char negated_delta = SNIP_SELF_DELTA; // no delta means negate the previous delta
        seg_by_did_i (vb, &negated_delta, 1, snip_did_i, add_bytes);
        snip_ctx->last_delta = 0; // no negated delta next time
    }

    return this_pos;
}
void seg_pos_field_cb (VBlockP vb, ContextP ctx, const char *pos_str, unsigned pos_len)
{
    seg_pos_field (vb, ctx->did_i, ctx->did_i, 0, 0, pos_str, pos_len, 0, pos_len);
}

// Commonly (but not always), IDs are SNPid identifiers like "rs17030902". We store the ID divided to 2:
// - We store the final digits, if any exist, and up to 9 digits, as an integer in SEC_NUMERIC_ID_DATA, which is
//   compressed with LZMA
// - In the dictionary we store the prefix up to this number, and \1 if there is a number and a \2 
//   if the caller (as in seg_me23) wants us to store an extra bit.
// example: rs17030902 : in the dictionary we store "rs\1" or "rs\1\2" and in SEC_NUMERIC_ID_DATA we store 17030902.
//          1423       : in the dictionary we store "\1" and 1423 SEC_NUMERIC_ID_DATA
//          abcd       : in the dictionary we store "abcd" and nothing is stored SEC_NUMERIC_ID_DATA
void seg_id_field_do (VBlock *vb, ContextP ctx, const char *id_snip, unsigned id_snip_len)
{
    int i=id_snip_len-1; for (; i >= 0; i--) 
        if (!IS_DIGIT (id_snip[i])) break;
    
    unsigned num_digits = MIN_(id_snip_len - (i+1), 9);

    // leading zeros will be part of the dictionary data, not the number
    for (unsigned i = id_snip_len - num_digits; i < id_snip_len; i++) 
        if (id_snip[i] == '0') 
            num_digits--;
        else 
            break;

    // added to local if we have a trailing number
    if (num_digits) {
        uint32_t id_num = atoi (&id_snip[id_snip_len - num_digits]);
        seg_add_to_local_uint32 (vb, ctx, id_num, 0);
    }

    ctx->no_stons = true;
    ctx->ltype  = LT_UINT32;

    // prefix the textual part with SNIP_LOOKUP_UINT32 if needed (we temporarily overwrite the previous separator or the buffer underflow area)
    unsigned new_len = id_snip_len - num_digits;
    SAFE_ASSIGN (&id_snip[-1], SNIP_LOOKUP); // we assign it anyway bc of the macro convenience, but we included it only if num_digits>0
    seg_by_ctx (vb, id_snip-(num_digits > 0), new_len + (num_digits > 0), ctx, id_snip_len); // account for the entire length, and sometimes with \t
    SAFE_RESTORE;
}

// returns true if it was an integer
bool seg_integer_or_not (VBlockP vb, ContextP ctx, const char *this_value, unsigned this_value_len, unsigned add_bytes)
{
    // case: its an integer
    if (!ctx->no_stons && // we interpret no_stons as means also no moving ints to local (one of the reasons is that an int might actually be a float)
        str_get_int (this_value, this_value_len, &ctx->last_value.i) &&
        ctx->last_value.i >= 0 && ctx->last_value.i <= 0xffffffffULL) {

        if (!ctx->local.len) { // first number 
            ctx->dynamic_size_local = true;
            ctx->ltype = LT_UINT32; // set only upon storing the first number - if there are no numbers, leave it as LT_TEXT so it can be used for singletons
            
            if (!ctx->b250.len) // no non-numbers yet
                ctx->numeric_only = true; // until proven otherwise
        }

        // add to local
        buf_alloc (vb, &ctx->local, 1, vb->lines.len, uint32_t, CTX_GROWTH, "contexts->local");
        NEXTENT (uint32_t, ctx->local) = (uint32_t)ctx->last_value.i;
        
        // add to b250
        seg_simple_lookup ((VBlockP)vb, ctx, add_bytes);

        return true;
    }

    // case: non-numeric snip
    else { 
        seg_by_ctx (vb, this_value, this_value_len, ctx, add_bytes);
        ctx->numeric_only = false;
        return false;
    }
}

void seg_integer_or_not_cb (VBlockP vb, ContextP ctx, const char *int_str, unsigned int_str_len)
{
    seg_integer_or_not (vb, ctx, int_str, int_str_len, int_str_len);
}

// if its a float, stores the float in local, and a LOOKUP in b250, and returns true. if not - normal seg, and returns false.
bool seg_float_or_not (VBlockP vb, ContextP ctx, const char *this_value, unsigned this_value_len, unsigned add_bytes)
{
    // TO DO: implement reconstruction in reconstruct_one_snip-SNIP_LOOKUP
    char snip[1 + FLOAT_FORMAT_LEN];
    unsigned format_len;

    // case: its an float
    if (!ctx->no_stons && // we interpret no_stons as means also no moving ints to local (one of the reasons is that an int might actually be a float)
        str_get_float (this_value, this_value_len, &ctx->last_value.f, &snip[2], &format_len)) {

        ctx->ltype = LT_FLOAT32; // set only upon storing the first number - if there are no numbers, leave it as LT_TEXT so it can be used for singletons

        // add to local
        buf_alloc (vb, &ctx->local, 1, vb->lines.len, float, CTX_GROWTH, "contexts->local");
        NEXTENT (float, ctx->local) = (float)ctx->last_value.f; // 32 bit
        
        // add to b250
        snip[0] = SNIP_LOOKUP;
        seg_by_ctx (vb, snip, 1 + format_len, ctx, add_bytes);

        return true;
    }

    // case: non-float snip
    else { 
        seg_by_ctx (vb, this_value, this_value_len, ctx, add_bytes);
        return false;
    }
}

WordIndex seg_delta_vs_other (VBlock *vb, Context *ctx, Context *other_ctx, 
                              const char *value, unsigned value_len, // if value==NULL, we use last_value.i (note: value_len must always be given)
                              int64_t max_delta /* max abs value of delta - beyond that, seg as is, ignored if < 0 */)
{
    if (!other_ctx) goto fallback;

    if (value && !str_get_int (value, value_len, &ctx->last_value.i)) goto fallback;

    int64_t delta = ctx->last_value.i - other_ctx->last_value.i; 
    if (max_delta >= 0 && (delta > max_delta || delta < -max_delta)) goto fallback;

    SNIP(100);
    seg_prepare_snip_other (SNIP_OTHER_DELTA, other_ctx->dict_id, true, (int32_t)delta, snip, &snip_len);

    other_ctx->no_stons = true;
    other_ctx->flags.store = STORE_INT;

    ctx->numeric_only = false;
    return seg_by_ctx (vb, snip, snip_len, ctx, value_len);

fallback:
    return value ? seg_by_ctx (vb, value, value_len, ctx, value_len) : seg_integer_do (vb, ctx->did_i, ctx->last_value.i, value_len);
}

#define MAX_COMPOUND_COMPONENTS 36
Container seg_initialize_container_array_do (DictId dict_id, bool type_1_items, bool comma_sep)
{
    Container con = (Container){ .repeats = 1,
                                 .drop_final_item_sep = comma_sep };

    for (unsigned i=0; i < MAX_COMPOUND_COMPONENTS; i++) {
        const uint8_t *id = dict_id.id;
        
        char dict_id_str[8] = { id[0], base36(i), id[1], id[2], id[3], id[4], id[5], id[6] };
        
        con.items[i].dict_id = dict_id_make (dict_id_str, 8, type_1_items ? DTYPE_1 : DTYPE_2);
        if (comma_sep) con.items[i].seperator[0] = ',';
    }

    return con;
}

typedef struct { 
    const char *item;       // pointer into field
    unsigned item_len;
    char sep;               // separator after item - 0 if none
    char sep2;              // double space seperator - common in FASTA headers
    bool is_int;
    int64_t value;          // in case of an int
    unsigned leading_zeros; // in case of an int - these will go into the container prefix
} CompoundItem;

static void seg_compound_field_split (const char *field, unsigned field_len, const char *is_sep,
                                      CompoundItem *items, uint8_t *n_items) // out: array of MAX_COMPOUND_COMPONENTS
{
    bool in_digits = field_len ? IS_DIGIT (field[0]) : false;
    bool is_final_item = false;
    const char *snip = field;
    unsigned snip_len = 0;
    *n_items = 0;

    for (unsigned i=0; i < field_len; i++) { // one more than field_len - to finalize the last subfield
    
        int next_c = (i==field_len-1) ? 0 : field[i+1];
        bool next_is_digit = IS_DIGIT (next_c);

        bool is_last_char_of_snip = (i==field_len-1)           ? true   // last character of data - definitely also last of this snip
                                  : is_final_item              ? false  // final item - we don't stop until the end of the data
                                  : in_digits != next_is_digit ? true   // digits / non-digits boundary is always terminating a snip
                                  : is_sep[next_c]             ? true   // a valid separator terminates a snip
                                  :                              false; // no reason to terminate this snip now;
        snip_len++;

        if (!is_last_char_of_snip) continue;

        CompoundItem *ci = &items[(*n_items)++];
        ci->item        = snip;
        ci->item_len    = snip_len; 
        ci->sep         = is_sep[next_c] ? (char)next_c : 0;

        // next, find out if item is an integer
        if (in_digits) {
            unsigned leading_zeros=0;
            for (; leading_zeros < snip_len-1 && snip[leading_zeros]=='0'; leading_zeros++) {} // last digit is never a leading 0, if it is 0, it is the integer

            // now set is_int, value and leading_zero. note that if is_int is false, the other two are meaningless
            ci->is_int = str_get_int (&snip[leading_zeros], snip_len - leading_zeros, &ci->value);
            ci->leading_zeros = leading_zeros;
        }
        else
            ci->is_int = false;

        if (*n_items == MAX_COMPOUND_COMPONENTS-1) is_final_item = true;

        if (ci->sep) i++; // skip separator

        // check for double space separator
        if (ci->sep == ' ' && i < field_len-1 && field[i+1] == ' ') {
            ci->sep2 = ' ';
            i++;
        }
        else
            ci->sep2 = 0;

        if (i < field_len-1) in_digits = IS_DIGIT (field[i+1]);

        snip = &field[i+1];
        snip_len = 0;
    }
}                                      

// We break down the field (eg QNAME in SAM/BAM/Kraken or Description in FASTA/FASTQ) into subfields separated by / and/or : - and/or whitespace 
// these are vendor-defined strings.
// Up to MAX_COMPOUND_COMPONENTS subfields are permitted - if there are more, then all the trailing part is just
// consider part of the last component.
// each subfield is stored in its own dictionary- the second character of the dict_id  the subfield number starting
// from 0 (0->9,a->z)
// The separators are made into a string we call "template" that is stored in the main field dictionary - we
// anticipate that usually all lines have the same format, but we allow lines to have different formats.
const char sep_with_space[256]    = { [';']=true, ['/']=true, ['|']=true, ['.']=true, ['_']=true, [' ']=true, ['\t']=true, [1]=true };
const char sep_without_space[256] = { [';']=true, ['/']=true, ['|']=true, ['.']=true, ['_']=true };
const char sep_pipe_only[256]     = { ['|']=true };
void seg_compound_field (VBlock *vb, 
                         Context *field_ctx, const char *field, unsigned field_len, 
                         const char *is_sep,   
                         unsigned nonoptimized_len, // if non-zero, we don't account for the string given, instead, only for this amount (+add_for_eol)
                         unsigned add_for_eol)      // account for characters beyond the component seperators
{
    // we use nodes.param in D?ESC contexts to track whether all snips in in this VB are the same
    // defaults to 0 (the same) and we set it to 1 if we encounter a different one
    #define not_all_the_same nodes.param 

    Container con = seg_initialize_container_array (field_ctx->dict_id, true, false); 
    CompoundItem items[MAX_COMPOUND_COMPONENTS];

    seg_compound_field_split (field, field_len, is_sep, items, &con.nitems_lo);
    
    char prefixes[field_len + con.nitems_lo + 2];
    prefixes[0] = prefixes[1] = CON_PREFIX_SEP;
    unsigned prefixes_len = 2;
    unsigned num_seps = 0;

    for (unsigned i=0; i < con.nitems_lo; i++) {
        CompoundItem  *ci = &items[i];
        ContainerItem *CI = &con.items[i];

        // process the subfield that just ended
        Context *item_ctx = ctx_get_ctx (vb, CI->dict_id);
        ASSERT (item_ctx, "item_ctx for %s is NULL", dis_dict_id (CI->dict_id).s);

        item_ctx->st_did_i = field_ctx->did_i;

        // allocate memory if needed
        buf_alloc (vb, &item_ctx->b250, 1, vb->lines.len, uint32_t, CTX_GROWTH, "contexts->b250");

        // if snip is an integer, we store a delta
        char delta_snip[30];
        unsigned original_item_len = ci->item_len;

        if (ci->is_int) {
            delta_snip[0] = SNIP_SELF_DELTA;

            PosType delta = ci->value - item_ctx->last_value.i;

            // note: if all the snips so far in this VB are the same - store just the snip, so that if the 
            // entire b250 is the same, it can be removed
            if (delta || item_ctx->not_all_the_same) {
                ci->item     = delta_snip;
                ci->item_len = 1 + str_int (delta, &delta_snip[1]);

                item_ctx->flags.store = STORE_INT;
                item_ctx->not_all_the_same = true;
            }

            ctx_set_last_value (vb, item_ctx, ci->value);
        }
        
        if (flag.pair == PAIR_READ_1)
            item_ctx->no_stons = true; // prevent singletons, so pair_2 can compare to us
        
        // we are evaluating but might throw away this snip and use SNIP_PAIR_LOOKUP instead - however, we throw away if its in the pair file,
        // i.e. its already in the dictionary and hash table - so no resources wasted
        WordIndex node_index = ctx_evaluate_snip_seg (vb, item_ctx, ci->item, ci->item_len, NULL);

        // case we are compressing fastq pairs - read 1 is the basis and thus must have a b250 node index,
        // and read 2 might have SNIP_PAIR_LOOKUP
        if (flag.pair == PAIR_READ_2) {

            // if the number of components in the compound is not exactly the same for every line of
            // pair 1 and pair 2 for this vb, the readings from the b250 will be incorrect, causing missed opportunities 
            // for SNIP_PAIR_LOOKUP and hence worse compression. This conditions makes sure this situation
            // doesn't result in an error (TO DO: overcome this, bug 159)
            // note: this can also happen if the there is no item_ctx->pair do it being fully singletons and moved to local 
            if (item_ctx->pair_b250_iter.next_b250 < AFTERENT (uint8_t, item_ctx->pair) &&
                // for pairing to word with SNIP_DELTA, if we have SNIP_PAIR_LOOKUP then all previous lines
                // this VB must have been SNIP_PAIR_LOOKUP as well. Therefore, the first time we encounter an
                // inequality - we stop the pairing going forward till the end of this VB
                !item_ctx->stop_pairing) {
                
                WordIndex pair_word_index = base250_decode (&item_ctx->pair_b250_iter.next_b250, !item_ctx->pair_flags.all_the_same, item_ctx->tag_name);  
                
                if (pair_word_index == WORD_INDEX_ONE_UP) 
                    pair_word_index = item_ctx->pair_b250_iter.prev_word_index + 1;
                
                item_ctx->pair_b250_iter.prev_word_index = pair_word_index;
                
                // note: if the pair word is a singleton in pair_1 file, then pair_word_index will be the index of {SNIP_LOOKUP}
                // rather than the snip (as replaced in ctx_evaluate_snip_merge), therefore this condition will fail. This is quite
                // rare, so not worth handling this case
                if (node_index == pair_word_index) {

                    // discard node_index - decrement its count
                    ctx_decrement_count (vb, item_ctx, node_index);

                    // get new node_index instead
                    item_ctx->pair_b250 = true;
                    static const char lookup_pair_snip[1] = { SNIP_PAIR_LOOKUP };
                    node_index = ctx_evaluate_snip_seg (vb, item_ctx, lookup_pair_snip, 1, NULL);
                } 
                else
                    // To improve: currently, pairing stops at the first non-match
                    item_ctx->stop_pairing = true;
            }
            else
                item_ctx->stop_pairing = true;
        }

        NEXTENT (uint32_t, item_ctx->b250) = node_index;

        item_ctx->txt_len += nonoptimized_len ? 0 : original_item_len;

        // set separators
        CI->seperator[0] = ci->sep;
        CI->seperator[1] = ci->sep2;
        num_seps += (ci->sep != 0) + (ci->sep2 != 0);

        // add the leading zeros to the container prefixes
        if (ci->is_int && ci->leading_zeros) {
            memset (&prefixes[prefixes_len], '0', ci->leading_zeros);
            prefixes_len += ci->leading_zeros + 1;
            prefixes[prefixes_len-1] = CON_PREFIX_SEP;
        }
        else
            prefixes[prefixes_len++] = CON_PREFIX_SEP;
    }

    container_seg (vb, field_ctx, &con, prefixes, prefixes_len, (nonoptimized_len ? nonoptimized_len : num_seps) + add_for_eol);
}

// an array or array of arrays
// note: if container_ctx->flags.store=STORE_INT, container_ctx->last_value.i will be set to sum of integer
// elements including recursively from sub-arrays (non-integer elements will be ignored)
WordIndex seg_array (VBlock *vb, Context *container_ctx, DidIType stats_conslidation_did_i, 
                     const char *value, int32_t value_len, // must be signed
                     char sep, 
                     char subarray_sep,         // if non-zero, will attempt to find internal arrays
                     bool use_integer_delta,    // first item stored as is, subsequent items stored as deltas
                     bool store_int_in_local,   // if its an integer, store in local instead of a snip
                     bool items_are_id)         // each item is segged as an ID (i.e. string + number)
{
    MiniContainer *con;
    DictId arr_dict_id;
    Context *arr_ctx;

    // first use in this VB - prepare context where array elements will go in
    if (!container_ctx->con_cache.len) {
        const uint8_t *id = container_ctx->dict_id.id;
        arr_dict_id = (DictId){ .id = { id[0], 
                                        ((id[1]+1) % 256) | 128, // different IDs for top array, subarray and items
                                        id[2], id[3], id[4], id[5], id[6], id[7] } };
        
        buf_alloc (vb, &container_ctx->con_cache, 0, 1, MiniContainer, 1, "contexts->con_cache");

        con = FIRSTENT (MiniContainer, container_ctx->con_cache);
        *con = (MiniContainer){ .nitems_lo = 1, 
                                .drop_final_repeat_sep = true,
                                .repsep    = { sep },
                                .items     = { { .dict_id = arr_dict_id } } }; // only one item

        arr_ctx = ctx_get_ctx (vb, arr_dict_id);
        arr_ctx->st_did_i = stats_conslidation_did_i;
    }
    else { 
        con         = FIRSTENT (MiniContainer, container_ctx->con_cache);
        arr_dict_id = con->items[0].dict_id;
        arr_ctx     = ctx_get_ctx (vb, arr_dict_id);
    }

    if (use_integer_delta || container_ctx->flags.store == STORE_INT) 
        arr_ctx->flags.store = STORE_INT;

    // count repeats (1 + number of seperators)
    con->repeats=1;
    for (int32_t i=0; i < value_len; i++) 
        if (value[i] == sep) con->repeats++;

    if (container_ctx->flags.store == STORE_INT) 
        ctx_set_last_value (vb, container_ctx, (int64_t)0);

    for (uint32_t i=0; i < con->repeats; i++) { // value_len will be -1 after last number

        const char *this_item = value;
        int64_t this_item_value=0;

        bool is_subarray = false;
        for (; value_len && *value != sep; value++, value_len--) 
            if (*value == subarray_sep) 
                is_subarray = true;

        unsigned this_item_len  = (unsigned)(value - this_item);

        // case: it has a sub-array
        if (is_subarray) {
            seg_array (vb, arr_ctx, stats_conslidation_did_i, this_item, this_item_len, subarray_sep, 0, use_integer_delta, store_int_in_local, items_are_id);
            arr_ctx->numeric_only = false;
            this_item_value = arr_ctx->last_value.i;
        }

        // item is ID eg "mRNA00001"
        else if (items_are_id) 
            seg_id_field (vb, arr_ctx, this_item, this_item_len, false);

        // case: its an scalar (we don't delta arrays that have sub-arrays and we don't delta the first item)
        else if (!use_integer_delta || subarray_sep || i==0) {
            
            if (store_int_in_local) {
                bool is_int = seg_integer_or_not (vb, arr_ctx, this_item, this_item_len, this_item_len);
                if (is_int) this_item_value = arr_ctx->last_value.i;
            }
            else {
                seg_by_ctx (vb, this_item, this_item_len, arr_ctx, this_item_len);
                str_get_int (this_item, this_item_len, &arr_ctx->last_value.i); // sets last_value only if it is indeed an integer
            }
        }

        // case: delta of 2nd+ item
        else if (str_get_int (this_item, this_item_len, &this_item_value)) {

            char delta_snip[30] = { SNIP_SELF_DELTA };
            unsigned delta_snip_len = 1 + str_int (this_item_value - arr_ctx->last_value.i, &delta_snip[1]);

            seg_by_ctx (vb, delta_snip, delta_snip_len, arr_ctx, this_item_len);

            ctx_set_last_value (vb, arr_ctx, this_item_value);
            arr_ctx->numeric_only = false;
        }

        // non-integer that cannot be delta'd - store as-is
        else {
            seg_by_ctx (vb, this_item, this_item_len, arr_ctx, 0);
            arr_ctx->numeric_only = false;
        }

        if (container_ctx->flags.store == STORE_INT)
            container_ctx->last_value.i += this_item_value;

        value_len--; // skip seperator
        value++;
    }

    return container_seg (vb, container_ctx, (ContainerP)con, 0, 0, con->repeats-1); // acount for separators
}

// a field that looks like: "non_coding_transcript_variant 0 ncRNA ENST00000431238,intron_variant 0 primary_transcript ENST00000431238"
// we have an array (2 entires in this example) of items (4 in this examples) - the entries are separated by comma and the items by space
// observed in Ensembel generated GVF: Variant_effect, sift_prediction, polyphen_prediction, variant_peptide
// The last item is treated as an ENST_ID (format: ENST00000399012) while the other items are regular dictionaries
// the names of the dictionaries are the same as the ctx, with the 2nd character replaced by 1,2,3...
// the field itself will contain the number of entries
void seg_array_of_struct (VBlockP vb, ContextP ctx, MediumContainer con, 
                          const char *snip, unsigned snip_len, 
                          SegCallback *callbacks) // optional - either NULL, or contains a seg callback for each item (any callback may be NULL)
{
    ContextP ctxs[con.nitems_lo]; 

    for (unsigned i=0; i < con.nitems_lo; i++) {
        ctxs[i] = ctx_get_ctx (vb, con.items[i].dict_id); 
        ctxs[i]->st_did_i = ctx->did_i;
        seg_create_rollback_point (ctxs[i]);
    }
    
    // get repeats
    str_split (snip, snip_len, 0, con.repsep[0], repeat, false);

    // if we don't have con.drop_final_repeat_sep, the last "repeat" should be zero length and removed
    if (!con.drop_final_repeat_sep) {
        if (repeat_lens[n_repeats-1]) goto badly_formatted; 
        n_repeats--;
    }
    con.repeats = n_repeats;

    ASSSEG (n_repeats <= CONTAINER_MAX_REPEATS, snip, "exceeded maximum repeats allowed (%u) while parsing %s",
            CONTAINER_MAX_REPEATS, ctx->tag_name);

    for (unsigned r=0; r < n_repeats; r++) {

        // get items in each repeat
        str_split (repeats[r], repeat_lens[r], con.nitems_lo, con.items[0].seperator[0], item, true);
        if (n_items != con.nitems_lo) goto badly_formatted;

        for (unsigned i=0; i < n_items; i++)
            if (callbacks && callbacks[i])
                callbacks[i] (vb, ctxs[i], items[i], item_lens[i]);
            else
                seg_by_ctx (vb, items[i], item_lens[i], ctxs[i], item_lens[i]);
    }

    // finally, the Container snip itself - we attempt to use the known node_index if it is cached in con_index

    // in our specific case, element i of con_index contains the node_index of the snip of the container with i repeats, or WORD_INDEX_NONE.
    if (ctx->con_index.len < n_repeats+1) {
        buf_alloc_255 (vb, &ctx->con_index, 0, n_repeats+1, WordIndex, 0, "con_index");
        ctx->con_index.len = ctx->con_index.len / sizeof (WordIndex);
    }

    WordIndex node_index = *ENT (WordIndex, ctx->con_index, n_repeats);
    unsigned account_for = con.repeats * (con.nitems_lo-1) /* item seperators */ + con.repeats - con.drop_final_repeat_sep /* repeat separators */;

    // case: first container with the many repeats - seg and add to cache
    if (node_index == WORD_INDEX_NONE) 
        *ENT (WordIndex, ctx->con_index, n_repeats) = container_seg (vb, ctx, (ContainerP)&con, NULL, 0, account_for);
    
    // case: we already know the node index of the container with this many repeats
    else 
        seg_known_node_index (vb, ctx, node_index, account_for);

    return;

badly_formatted:
    // roll back all the changed data
    for (unsigned i=0; i < con.nitems_lo ; i++) 
        seg_rollback (vb, ctxs[i]);

    // now just seg the entire snip
    seg_by_ctx (vb, snip, snip_len, ctx, snip_len); 
}                           

void seg_add_to_local_text (VBlock *vb, Context *ctx, 
                            const char *snip, unsigned snip_len, 
                            unsigned add_bytes)  // bytes in the original text file accounted for by this snip
{
    buf_alloc (vb, &ctx->local, snip_len + 1, 0, char, CTX_GROWTH, "contexts->local"); // no multiplying by line.len, as snip can idiosyncratically be very large
    if (snip_len) buf_add (&ctx->local, snip, snip_len); 
    NEXTENT (char, ctx->local) = 0;

    if (add_bytes) ctx->txt_len += add_bytes;
    ctx->local.num_ctx_words++;
}

void seg_add_to_local_text_cb (VBlockP vb, ContextP ctx, const char *snip, unsigned snip_len)
{
        seg_add_to_local_text (vb, ctx, snip, snip_len, snip_len);
}

void seg_add_to_local_fixed (VBlock *vb, Context *ctx, const void *data, unsigned data_len)  // bytes in the original text file accounted for by this snip
{
    if (data_len) {
        buf_alloc (vb, &ctx->local, data_len, vb->lines.len * data_len, char, CTX_GROWTH, "contexts->local");
        buf_add (&ctx->local, data, data_len); 
    }
}

void seg_add_to_local_uint8 (VBlockP vb, ContextP ctx, uint8_t value, unsigned add_bytes)
{
    buf_alloc (vb, &ctx->local, 1, vb->lines.len, uint8_t, CTX_GROWTH, "contexts->local");

    if (lt_desc[ctx->ltype].is_signed) value = INTERLACE (int8_t, value);
    NEXTENT (uint8_t, ctx->local) = value;

    if (add_bytes) ctx->txt_len += add_bytes;
}

void seg_add_to_local_uint16 (VBlockP vb, ContextP ctx, uint16_t value, unsigned add_bytes)
{
    buf_alloc (vb, &ctx->local, 1, vb->lines.len, uint16_t, CTX_GROWTH, "contexts->local");

    if (lt_desc[ctx->ltype].is_signed) value = INTERLACE (int16_t, value);
    NEXTENT (uint16_t, ctx->local) = BGEN16 (value);

    if (add_bytes) ctx->txt_len += add_bytes;
}

void seg_add_to_local_uint32 (VBlockP vb, ContextP ctx, uint32_t value, unsigned add_bytes)
{
    buf_alloc (vb, &ctx->local, 1, vb->lines.len, uint32_t, CTX_GROWTH, "contexts->local");

    if (lt_desc[ctx->ltype].is_signed) value = INTERLACE (int32_t, value);
    NEXTENT (uint32_t, ctx->local) = BGEN32 (value);

    if (add_bytes) ctx->txt_len += add_bytes;
}

void seg_add_to_local_uint64 (VBlockP vb, ContextP ctx, uint64_t value, unsigned add_bytes)
{
    buf_alloc (vb, &ctx->local, 1, vb->lines.len, uint64_t, CTX_GROWTH, "contexts->local");

    if (lt_desc[ctx->ltype].is_signed) value = INTERLACE (int64_t, value);
    NEXTENT (uint64_t, ctx->local) = BGEN64 (value);

    if (add_bytes) ctx->txt_len += add_bytes;
}

static void seg_set_hash_hints (VBlock *vb, int third_num)
{
    if (third_num == 1) 
        vb->num_lines_at_1_3 = vb->line_i + 1;
    else 
        vb->num_lines_at_2_3 = vb->line_i + 1;

    for (DidIType did_i=0; did_i < vb->num_contexts; did_i++) {

        Context *ctx = CTX(did_i);
        if (ctx->global_hash_prime) continue; // our service is not needed - global_cache for this dict already exists

        if (third_num == 1) 
            ctx->nodes_len_at_1_3 = ctx->nodes.len;

        else 
            ctx->nodes_len_at_2_3 = ctx->nodes.len;
    }
}

static uint32_t seg_estimate_num_lines (VBlock *vb)
{
#   define NUM_LINES_IN_TEST 10 // take an average of this number of lines
    uint32_t len=0; 

    if (!DTP (line_height)) return 0; // this data type doesn't use textual lines

    ARRAY (char, txt, vb->txt_data);
    int line_count=0; for (; line_count < DTP (line_height) * NUM_LINES_IN_TEST && len < txt_len; line_count++, len++)
        for (; len < txt_len && txt[len] != '\n'; len++) {};

    len /= MAX_(1, (line_count / DTP (line_height))); // average length of a line

    ASSERT (txt_len, "vb=%u: txt_data is empty", vb->vblock_i); 

    ASSSEG (line_count==DTP (line_height) || len < txt_len, txt, 
            "a line in the file is longer than %s characters (a maximum defined by vblock). If this is intentional, use --vblock to increase the vblock size", 
            str_uint_commas (flag.vblock_memory).s);

    return MAX_(100, (uint32_t)(((double)txt_len / (double)len) * 1.2));
}

// double the number of lines if we've run out of lines
static void seg_more_lines (VBlock *vb, unsigned sizeof_line)
{
    uint64_t num_old_lines = vb->lines.len;
    
    // note: sadly, we cannot use the normal Buffer macros here because each data_type has its own line type
    buf_alloc_zero (vb, &vb->lines, 0, (vb->lines.len + 1) * sizeof_line, char, 2, "lines");    
    vb->lines.len = vb->lines.size / sizeof_line;

    // allocate more to the b250 buffer of the fields, which each have num_lines entries
    for (DidIType f=0; f < DTF(num_fields); f++) 
        if (buf_is_alloc (&CTX(f)->b250))
            buf_alloc_zero (vb, &CTX(f)->b250, vb->lines.len - num_old_lines, 0, uint32_t, 1, "contexts->b250");
}

static void seg_verify_file_size (VBlock *vb)
{
    uint32_t recon_size = 0; // reconstructed size, as viewed in reconstruction

    // sanity checks
    ASSERT (vb->recon_size >= 0, "recon_size=%d is negative for vb_i=%u, coord=%s", vb->recon_size, vb->vblock_i, coords_name(vb->vb_coords));
    ASSERT (vb->recon_size_luft >= 0, "recon_size_luft=%d is negative for vb_i=%u, coord=%s", vb->recon_size_luft, vb->vblock_i, coords_name(vb->vb_coords));

    for (DidIType sf_i=0; sf_i < vb->num_contexts; sf_i++) 
        recon_size += CTX(sf_i)->txt_len;
    
    uint32_t vb_recon_size = vb->vb_coords == DC_LUFT ? vb->recon_size_luft : vb->recon_size; // in primary reconstruction, ##luft_only VB is reconstructed in luft coords
    if (vb_recon_size != recon_size || flag.debug_recon_size) { 

        fprintf (stderr, "context.txt_len for vb=%u:\n", vb->vblock_i);
        for (DidIType sf_i=0; sf_i < vb->num_contexts; sf_i++) {
            Context *ctx = CTX(sf_i);
            if (ctx->nodes.len || ctx->local.len || ctx->txt_len)
                fprintf (stderr, "%s: %u\n", ctx_tag_name_ex (ctx).s, (uint32_t)ctx->txt_len);
        }

        fprintf (stderr, "vb=%u reconstructed_vb_size=%s (calculated by adding up ctx.txt_len after segging) but vb->recon_size%s=%s (initialized when reading the file and adjusted for modifications) (diff=%d) (vblock_memory=%s)\n",
                 vb->vblock_i, str_uint_commas (recon_size).s, vb->vb_coords == DC_LUFT ? "_luft" : "", str_uint_commas (vb_recon_size).s, 
                 (int32_t)recon_size - (int32_t)vb_recon_size, str_size (flag.vblock_memory).s);

        ASSERT (vb_recon_size == recon_size, "Error while verifying reconstructed size - to get vblock:\n"
                "%s %s | head -c %"PRIu64" | tail -c %u > vb.%u%s",
                /* head/tail params:  */ codec_args[txt_file->codec].viewer, txt_name, vb->vb_position_txt_file + vb->txt_data.len, (uint32_t)vb->txt_data.len,
                /* output filename:   */ vb->vblock_i, file_plain_ext_by_dt (vb->data_type));
    }
}

// split each lines in this variant block to its components
void seg_all_data_lines (VBlock *vb)
{
    START_TIMER;

    ctx_initialize_predefined_ctxs (vb->contexts, vb->data_type, vb->dict_id_to_did_i_map, &vb->num_contexts); // Create ctx for the fields in the correct order 
 
    // allocate the b250 for the fields which each have num_lines entries
    for (DidIType f=0; f < DTF(num_fields); f++) 
        buf_alloc (vb, &CTX(f)->b250, 0, vb->lines.len, WordIndex, 1, "contexts->b250");
    
    DT_FUNC (vb, seg_initialize)(vb);  // data-type specific initialization

    // get estimated number of lines, if we haven't already (eg in bam_seg_initialize)
    if (!vb->lines.len)
        vb->lines.len = seg_estimate_num_lines(vb);

    // allocate lines
    uint32_t sizeof_line = DT_FUNC_OPTIONAL (vb, sizeof_zip_dataline, 1)(); // 1 - we waste a little bit of memory to avoid making exceptions throughout the code logic
    buf_alloc_zero (vb, &vb->lines, 0, vb->lines.len * sizeof_line, char, 1, "lines");

    const char *field_start = vb->txt_data.data;
    bool hash_hints_set_1_3 = false, hash_hints_set_2_3 = false;
    for (vb->line_i=0; vb->line_i < vb->lines.len; vb->line_i++) {

        uint32_t remaining_txt_len = AFTERENT (char, vb->txt_data) - field_start;
        
        if (!remaining_txt_len) { // we're done
            vb->lines.len = vb->line_i; // update to actual number of lines
            break;
        }

        //fprintf (stderr, "vb->line_i=%u\n", vb->line_i);
        bool has_13 = false;
        vb->line_start = ENTNUM (vb->txt_data, field_start);
        const char *next_field = DT_FUNC (vb, seg_txt_line) (vb, field_start, remaining_txt_len, &has_13);

        vb->longest_line_len = MAX_(vb->longest_line_len, (next_field - field_start));
        field_start = next_field;

        // if our estimate number of lines was too small, increase it
        if (vb->line_i == vb->lines.len-1 && field_start - vb->txt_data.data != vb->txt_data.len)         
            seg_more_lines (vb, sizeof_line);
        
        // collect stats at the approximate 1/3 or 2/3s marks of the file, to help hash_alloc_global create a hash
        // table. note: we do this for every vb, not just 1, because hash_alloc_global runs in the first
        // vb a new field/subfield is introduced
        if (!hash_hints_set_1_3 && (field_start - vb->txt_data.data) > vb->txt_data.len / 3) {
            seg_set_hash_hints (vb, 1);
            hash_hints_set_1_3 = true;
        }
        else if (!hash_hints_set_2_3 && (field_start - vb->txt_data.data) > 2 * vb->txt_data.len / 3) {
            seg_set_hash_hints (vb, 2);
            hash_hints_set_2_3 = true;
        }
    }

    DT_FUNC (vb, seg_finalize)(vb); // data-type specific finalization

    if (!flag.make_reference && strcmp (vb->compute_task, DYN_SET_MEM_TASK)) 
        seg_verify_file_size (vb);

    COPY_TIMER (seg_all_data_lines);
}