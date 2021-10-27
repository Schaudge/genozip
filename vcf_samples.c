// ------------------------------------------------------------------
//   vcf_samples.c
//   Copyright (C) 2019-2021 Black Paw Ventures Limited
//   Please see terms and conditions in the file LICENSE.txt

#include "vcf_private.h"
#include "seg.h"
#include "context.h"
#include "optimize.h"
#include "file.h"
#include "strings.h"
#include "dict_id.h"
#include "codec.h"
#include "reconstruct.h"
#include "base64.h"
#include "stats.h"

static Container con_FORMAT_AD={}, con_FORMAT_ADALL={}, con_FORMAT_ADF={}, con_FORMAT_ADR={}, con_FORMAT_SAC={}, 
                 con_FORMAT_F1R2={}, con_FORMAT_F2R1={}, con_FORMAT_MB={}, con_FORMAT_SB={}, con_FORMAT_AF={};
static char sb_snips[2][32], mb_snips[2][32], f2r1_snips[MAX_ARRAY_ITEMS][32], adr_snips[MAX_ARRAY_ITEMS][32], adf_snips[MAX_ARRAY_ITEMS][32], 
            rdf_snip[32], rdr_snip[32], ad_snip[32],
            af_snip[32], sac_snips[MAX_ARRAY_ITEMS/2][32];
static unsigned sb_snip_lens[2], mb_snip_lens[2], f2r1_snip_lens[MAX_ARRAY_ITEMS], adr_snip_lens[MAX_ARRAY_ITEMS], adf_snip_lens[MAX_ARRAY_ITEMS], 
                af_snip_len, sac_snip_lens[MAX_ARRAY_ITEMS/2], rdf_snip_len, rdr_snip_len, ad_snip_len;

#define last_sample_i ctx_specific // ZIP: like last_line_i, but used for VCF/FORMAT fields (0-based). Only meaningful if last_line_i indicates the line is the same vb->line_i

#define vcf_encountered_in_sample_(vb, ctx) (ctx_encountered_in_line_((VBlockP)vb, ctx) && (ctx)->last_sample_i == VB_VCF->sample_i)
#define vcf_encountered_in_sample(vb, dict_id, p_ctx) (ctx_encountered_in_line ((VBlockP)vb, dict_id, p_ctx) && (*(p_ctx))->last_sample_i == VB_VCF->sample_i)
#define vcf_has_value_in_sample_(vb, ctx) (ctx_has_value_in_line_((VBlockP)vb, ctx) && (ctx)->last_sample_i == (vb)->sample_i)
#define vcf_has_value_in_sample(vb, dict_id, p_ctx) (ctx_has_value_in_line ((VBlockP)vb, dict_id, p_ctx) && (*(p_ctx))->last_sample_i == (vb)->sample_i)

#define vcf_set_last_sample_value_(vb, ctx, last_value) do { ctx_set_last_value ((VBlockP)(vb), ctx, last_value); (ctx)->last_sample_i = (vb)->sample_i; } while (0);
// extern'ed version of vcf_set_last_sample_value_
void vcf_set_last_sample_value (VBlockP vb, ContextP ctx, int64_t last_value)
{
    vcf_set_last_sample_value_(VB_VCF, ctx, last_value);
}

#define vcf_set_encountered_in_sample(ctx)  /* set encountered if not already vcf_set_last_sample_value_ */  \
    if (!vcf_has_value_in_sample_(vb, ctx)) { \
        (ctx)->last_line_i   = -(int32_t)vb->line_i - 1; /* encountered in line */ \
        (ctx)->last_sample_i = vb->sample_i;  \
    }

#define vcf_reset_encountered_in_sample(ctx) (ctx)->last_sample_i = LAST_LINE_I_INIT

// prepare snip of A - B
static void vcf_seg_prepare_minus_snip (DictId dict_id_a, DictId dict_id_b, char *snip, unsigned *snip_len)
{
    snip[0] = SNIP_SPECIAL;
    snip[1] = VCF_SPECIAL_MINUS;
    
    DictId two_dicts[2] = { dict_id_a, dict_id_b };
    *snip_len = 2 + base64_encode ((uint8_t *)two_dicts, sizeof (two_dicts), &snip[2]);
}

void vcf_samples_zip_initialize (void) 
{
    static bool done = false;
    if (done) return; // already initialized (in previous files)
    done = true;

    con_FORMAT_AF    = seg_initialize_container_array (_FORMAT_AF,    false, true);    
    con_FORMAT_AD    = seg_initialize_container_array (_FORMAT_AD,    false, true);    
    con_FORMAT_ADALL = seg_initialize_container_array (_FORMAT_ADALL, false, true);    
    con_FORMAT_ADF   = seg_initialize_container_array (_FORMAT_ADF,   false, true);    
    con_FORMAT_ADR   = seg_initialize_container_array (_FORMAT_ADR,   false, true);    
    con_FORMAT_F1R2  = seg_initialize_container_array (_FORMAT_F1R2,  false, true);    
    con_FORMAT_F2R1  = seg_initialize_container_array (_FORMAT_F2R1,  false, true);    
    con_FORMAT_MB    = seg_initialize_container_array (_FORMAT_MB,    false, true);    
    con_FORMAT_SB    = seg_initialize_container_array (_FORMAT_SB,    false, true);    
    con_FORMAT_SAC   = seg_initialize_container_array (_FORMAT_SAC,   false, true);    

    // prepare special snips for the odd elements of SB and MB - (AD minus even item 0) 
    for (unsigned i=0; i < 2; i++) {
        vcf_seg_prepare_minus_snip (con_FORMAT_AD.items[i].dict_id, con_FORMAT_SB.items[i*2].dict_id, sb_snips[i], &sb_snip_lens[i]);
        vcf_seg_prepare_minus_snip (con_FORMAT_AD.items[i].dict_id, con_FORMAT_MB.items[i*2].dict_id, mb_snips[i], &mb_snip_lens[i]);
    }

    for (unsigned i=0; i < MAX_ARRAY_ITEMS/2; i++) 
        vcf_seg_prepare_minus_snip (con_FORMAT_AD.items[i].dict_id, con_FORMAT_SAC.items[i*2].dict_id, sac_snips[i], &sac_snip_lens[i]);

    for (unsigned i=0; i < MAX_ARRAY_ITEMS; i++) {
        vcf_seg_prepare_minus_snip (con_FORMAT_AD.items[i].dict_id, con_FORMAT_F1R2.items[i].dict_id, f2r1_snips[i], &f2r1_snip_lens[i]);
        vcf_seg_prepare_minus_snip (con_FORMAT_AD.items[i].dict_id, con_FORMAT_ADF.items[i].dict_id,  adr_snips[i],  &adr_snip_lens[i]);
        vcf_seg_prepare_minus_snip (con_FORMAT_AD.items[i].dict_id, con_FORMAT_ADR.items[i].dict_id,  adf_snips[i],  &adf_snip_lens[i]);
    }

    vcf_seg_prepare_minus_snip ((DictId)_FORMAT_RD, (DictId)_FORMAT_RDR, rdf_snip, &rdf_snip_len);
    vcf_seg_prepare_minus_snip ((DictId)_FORMAT_RD, (DictId)_FORMAT_RDF, rdr_snip, &rdr_snip_len);
    vcf_seg_prepare_minus_snip ((DictId)_FORMAT_DP, (DictId)_FORMAT_RD,  ad_snip,  &ad_snip_len);

    seg_prepare_snip_other (SNIP_COPY, _INFO_AF, 0, 0, af_snip);
}


// used when CTX is expected to be (BaseCtx-MinusCtx) - if it indeed is, we use a special snip
static void vcf_seg_FORMAT_minus (VBlockVCFP vb, ContextP ctx, 
                                  STRp(str), int64_t value, // option 1,2
                                  ContextP base_ctx, ContextP minus_ctx, STRp(minus_snip))
{
    // we can use the formula only if AD,F1R1 were encountered in this line, and that they have the number of items as us
    if (str && !str_get_int (STRa(str), &value)) goto fallback;

    vcf_set_last_sample_value_ (vb, ctx, value);

    bool use_formula = vcf_has_value_in_sample_(vb, base_ctx) && vcf_has_value_in_sample_(vb, minus_ctx) &&
                       value == base_ctx->last_value.i - minus_ctx->last_value.i;

    // case: formula works - seg as minus
    if (use_formula)
        seg_by_ctx (VB, STRa(minus_snip), ctx, str_len);

    // case: the formula doesn't work for this item - seg a normal snip
    else
        fallback:
        seg_by_ctx (VB, STRa(str), ctx, str_len);
}

// used for DP, GQ, A0D and otheres - store in transposed matrix in local 
static inline WordIndex vcf_seg_FORMAT_transposed (VBlockVCF *vb, Context *ctx, STRp(cell), unsigned add_bytes)
{
    ctx->ltype = LT_UINT32_TR;
    ctx->flags.store = STORE_INT;

    buf_alloc (vb, &ctx->local, 1, vb->lines.len * vcf_num_samples, uint32_t, 1, "contexts->local");

    if (cell_len == 1 && cell[0] == '.') {
        NEXTENT (uint32_t, ctx->local) = 0xffffffff;
    }
    else {
        ASSSEG (str_get_int (STRa(cell), &ctx->last_value.i) && ctx->last_value.i >= 0 && ctx->last_value.i <= 0xfffffffe, 
                cell, "While compressing %s expecting an integer in the range [0, 0xfffffffe] or a '.', but found: %.*s", 
                ctx->tag_name, cell_len, cell);

        NEXTENT (uint32_t, ctx->local) = (uint32_t)ctx->last_value.i;
    }

    // add a LOOKUP to b250
    seg_by_ctx (VB, (char []){ SNIP_LOOKUP }, 1, ctx, add_bytes);

    return 0;
}

// a comma-separated array - each element goes into its own item context, single repeat
static WordIndex vcf_seg_FORMAT_A_R_G (VBlockVCF *vb, Context *ctx, Container con /* by value */, STRp(value), StoreType item_store_type,
                                       void (*seg_item_cb)(VBlockVCFP, Context *ctx, unsigned num_items, ContextP *item_ctxs, 
                                                           const char**, const uint32_t*, const int64_t*))
{   
    str_split (value, value_len, MAX_ARRAY_ITEMS, ',', item, false);
    
    if (!(con.nitems_lo = n_items)) 
        return seg_by_ctx (VB, value, value_len, ctx, value_len); // too many items - normal seg

    Context *item_ctxs[con.nitems_lo];
    int64_t values[con.nitems_lo];

    for (unsigned i=0; i < con.nitems_lo; i++) {

        item_ctxs[i] = ctx_get_ctx (vb, con.items[i].dict_id);
        item_ctxs[i]->flags.store = item_store_type;
        item_ctxs[i]->st_did_i = ctx->did_i;

        if (seg_item_cb) {
            if (str_get_int (STRi(item, i), &values[i])) 
                item_ctxs[i]->last_value.i = values[i];
            else
                seg_item_cb = NULL; // can't use callback if not all items are int
        }
    }

    // case: seg items via callback
    if (seg_item_cb)
        seg_item_cb (vb, ctx, con.nitems_lo, item_ctxs, items, item_lens, values);

    // case: seg items as normal snips
    else 
        for (unsigned i=0; i < con.nitems_lo; i++) 
            seg_by_ctx (VB, STRi(item, i), item_ctxs[i], item_lens[i]);

    ctx->last_txt_len = con.nitems_lo; // seg only: for use by vcf_seg_*_items callbacks

    return container_seg (vb, ctx, (ContainerP)&con, 0, 0, con.nitems_lo-1); // account for the commas
}

//----------
// FORMAT/AD
// ---------

// Sepcial treatment for item 0
static void vcf_seg_AD_items (VBlockVCFP vb, Context *ctx, unsigned num_items, ContextP *item_ctxs, 
                              STRps(item), const int64_t *values)
{
    // case: AD = DP-RD
    if (num_items==1 &&
        vcf_has_value_in_sample_(vb, CTX(FORMAT_DP)) &&
        vcf_has_value_in_sample_(vb, CTX(FORMAT_RD)) && 
        values[0] == CTX(FORMAT_DP)->last_value.i - CTX(FORMAT_RD)->last_value.i) 
    
        vcf_seg_FORMAT_minus (vb, item_ctxs[0], 0, item_lens[0], values[0], CTX(FORMAT_DP), CTX(FORMAT_RD), STRa(ad_snip));
        
    // case: we had ADALL preceeding in this sample, seg as delta vs. ADALL 
    else if (vcf_has_value_in_sample_(vb, CTX(FORMAT_ADALL))) 
        for (unsigned i=0; i < num_items; i++) 
            seg_delta_vs_other (VB, item_ctxs[i], ECTX (con_FORMAT_ADALL.items[i].dict_id), NULL, item_lens[i]);

    // case: we have only one sample, and INFO/ADP - we expect FORMAT/AD and INFO/ADP to be related
    else if (ctx_has_value_in_line_(vb, CTX(INFO_ADP)) && vcf_num_samples==1 && num_items==1)
        seg_delta_vs_other (VB, item_ctxs[0], CTX(INFO_ADP), 0, item_lens[0]);

    // case: no preceeding ADALL, since item 0 (depth of REF) is usually somewhat related to the overall sample depth,
    // and hence values within a sample are expected to be correlated - we store it transposed, and the other items - normally
    else {
        vcf_seg_FORMAT_transposed (vb, item_ctxs[0], STRi(item, 0), item_lens[0]);

        for (unsigned i=1; i < num_items; i++) 
            seg_by_ctx (VB, STRi(item, i), item_ctxs[i], item_lens[i]);
    }
            
    // set sum of items for AD
    int64_t sum = 0; 
    for (unsigned i=0; i < num_items; i++) 
        sum +=  values[i];

    vcf_set_last_sample_value_ (vb, ctx, sum);
    ctx->flags.store = STORE_INT; // tell container_reconstruct_do to set last_value of container to the sum of its items

    memcpy (vb->ad_values, values, num_items * sizeof (values[0]));
}

//-------------
// FORMAT/ADALL
//-------------

// Sepcial treatment for item 0
static void vcf_seg_ADALL_items (VBlockVCFP vb, Context *ctx, unsigned num_items, ContextP *item_ctxs, 
                                 STRps(item), const int64_t *values)
{
    // item 0, the depth of REF, is usually somewhat related to the overall sample depth,
    // therefore values within a sample are expected to be correlated - so we store it transposed
    vcf_seg_FORMAT_transposed (vb, item_ctxs[0], STRi(item, 0), item_lens[0]);

    for (unsigned i=1; i < num_items; i++) 
        seg_by_ctx (VB, STRi(item, i), item_ctxs[i], item_lens[i]);
}

//----------------------
// FORMAT/F2R1, ADF, ADR
//----------------------

// used when Vector is expected to be (AD-OtherVector) - if it indeed is, we use a special snip
static void vcf_seg_AD_complement_items (VBlockVCFP vb, Context *ctx, unsigned num_items, ContextP *item_ctxs, 
                                         STRps(item), const int64_t *values,
                                         DictId other_dict_id, const Container *other_con,
                                         char my_snips[][32], unsigned *my_snip_lens)
{
    // we can use the formula only if AD,F1R1 were encountered in this line, and that they have the number of items as us
    ContextP ad_ctx, other_ctx;
    bool use_formula = vcf_encountered_in_sample (vb, _FORMAT_AD, &ad_ctx) &&
                       vcf_encountered_in_sample (vb, other_dict_id.num, &other_ctx) &&
                       ad_ctx->last_txt_len    == num_items &&  // last_txt_len is # of items stored by vcf_seg_FORMAT_A_R_G 
                       other_ctx->last_txt_len == num_items;

    for (unsigned i=0; i < num_items; i++) {

        // case: as expected, F1R2 + F2R1 = AD - seg as a F2R1 as a MINUS snip
        if (use_formula && vb->ad_values[i] == values[i] + ECTX (other_con->items[i].dict_id)->last_value.i) {
            seg_by_ctx (VB, my_snips[i], my_snip_lens[i], item_ctxs[i], item_lens[i]); 
            item_ctxs[i]->no_stons = true; // enable "all_the_same"
        }

        // case: the formula doesn't work for this item - seg a normal snip
        else
            seg_by_ctx (VB, STRi(item,i), item_ctxs[i], item_lens[i]);
    }
}

// F2R1 = AD - F1R2 (applied if AD and F1R2 are encountered before F2R1)
static void vcf_seg_F2R1_items (VBlockVCFP vb, Context *ctx, unsigned num_items, ContextP *item_ctxs, 
                                STRps(item), const int64_t *values)
{
    vcf_seg_AD_complement_items (vb, ctx, num_items, item_ctxs, items, item_lens, values, (DictId)_FORMAT_F1R2, &con_FORMAT_F1R2, f2r1_snips, f2r1_snip_lens);
}

// ADF = AD - ADR (applied if AD and ADR are encountered before ADF)
static void vcf_seg_ADF_items (VBlockVCFP vb, Context *ctx, unsigned num_items, ContextP *item_ctxs, 
                               STRps(item), const int64_t *values)
{
    vcf_seg_AD_complement_items (vb, ctx, num_items, item_ctxs, items, item_lens, values, (DictId)_FORMAT_ADR, &con_FORMAT_ADR, adf_snips, adf_snip_lens);
}

// ADR = AD - ADF (applied if AD and ADF are encountered before ADR)
static void vcf_seg_ADR_items (VBlockVCFP vb, Context *ctx, unsigned num_items, ContextP *item_ctxs, 
                               STRps(item), const int64_t *values)
{
    vcf_seg_AD_complement_items (vb, ctx, num_items, item_ctxs, items, item_lens, values, (DictId)_FORMAT_ADF, &con_FORMAT_ADF, adr_snips, adr_snip_lens);
}

//----------
// FORMAT/SB
//----------

// For bi-allelic SNPs, sum every of two values is expected to equal the corresponding value in AD. Example: AD=59,28 SB=34,25,17,11. 
// seg the second of every pair as a MINUS snip
static void vcf_seg_SB_items (VBlockVCFP vb, Context *ctx, unsigned num_items, ContextP *item_ctxs, 
                              STRps(item), const int64_t *values)
{
    // verify that AD was encountered in this line, and that it has exactly half the number of items as us
    ContextP ad_ctx;
    bool use_formula = vcf_encountered_in_sample (vb, _FORMAT_AD, &ad_ctx) && ad_ctx->last_txt_len == 2 && num_items == 4; // note: last_txt_len = # of items stored by vcf_seg_FORMAT_A_R_G

    for (unsigned i=0; i < num_items; i++) {

        // seg odd-numbered element as AD - (even element), if the sum is correct
        if (use_formula && i%2 && vb->ad_values[i/2] == values[i-1] + values[i]) {
            seg_by_ctx (VB, sb_snips[i/2], sb_snip_lens[i/2], item_ctxs[i], item_lens[i]); 
            item_ctxs[i]->no_stons = true; // to enable "all_the_same"
        }
        else {
            item_ctxs[i]->flags.store = STORE_INT; // consumed by the odd items ^
            seg_by_ctx (VB, STRi(item, i), item_ctxs[i], item_lens[i]);
        }
    }
}

//-----------
// FORMAT/SAC
//-----------

// sum every of two values is expected to equal the corresponding value in AD. seg the second of every pair as a MINUS snip
static void vcf_seg_SAC_items (VBlockVCFP vb, Context *ctx, unsigned num_items, ContextP *item_ctxs, 
                               STRps(item), const int64_t *values)
{
    // verify that AD was encountered in this line, and that it has exactly half the number of items as us
    ContextP ad_ctx;
    bool use_formula = vcf_encountered_in_sample (vb, _FORMAT_AD, &ad_ctx) && 2 * ad_ctx->last_txt_len == num_items; // note: last_txt_len = # of items stored by vcf_seg_FORMAT_A_R_G

    for (unsigned i=0; i < num_items; i++) {

        // seg odd-numbered element as AD - (even element), if the sum is correct
        if (use_formula && i%2 && vb->ad_values[i/2] == values[i-1] + values[i]) {
            seg_by_ctx (VB, sac_snips[i/2], sac_snip_lens[i/2], item_ctxs[i], item_lens[i]); 
            item_ctxs[i]->no_stons = true; // to enable "all_the_same"
        }
        else {
            item_ctxs[i]->flags.store = STORE_INT; // consumed by the odd items ^
            seg_by_ctx (VB, STRi(item, i), item_ctxs[i], item_lens[i]);
        }
    }
}

//----------
// FORMAT/MB
//----------

// For bi-allelic SNPs: sum every of two items is expected to equal the corresponding value in AD. Example: AD=7,49 F2R1=3,28 MB=4,3,26,23 
// In addition, the even-numbered item is quite similar to the corresponding value in F2R1.
// Seg the even items as delta from F2R1 and odd items as a MINUS snip between AD and the preceding even item
static void vcf_seg_MB_items (VBlockVCFP vb, Context *ctx, unsigned num_items, ContextP *item_ctxs, 
                              STRps(item), const int64_t *values)
{
    bool use_formula_even = vcf_encountered_in_sample_(vb, CTX(FORMAT_F2R1)) && CTX(FORMAT_F2R1)->last_txt_len == 2 && num_items == 4;
    bool use_formula_odd  = vcf_encountered_in_sample_(vb, CTX(FORMAT_AD))   && CTX(FORMAT_AD)  ->last_txt_len == 2 && num_items == 4; // last_txt_len is # of items set by vcf_seg_FORMAT_A_R_G

    for (unsigned i=0; i < num_items; i++) {

        // if possible, seg even-numbered element delta vs the corresponding element in F2R1
        if (use_formula_even && !(i%2)) { 
            seg_delta_vs_other (VB, item_ctxs[i], ECTX (con_FORMAT_F2R1.items[i/2].dict_id), NULL, item_lens[i]);
            item_ctxs[i]->flags.store = STORE_INT; // consumed by the odd items (below)
        }

        // if possible, seg odd-numbered element as AD minus (even element), if the sum is correct
        else if (use_formula_odd && i%2 && vb->ad_values[i/2] == values[i-1] + values[i]) {
            seg_by_ctx (VB, mb_snips[i/2], mb_snip_lens[i/2], item_ctxs[i], item_lens[i]); 
            item_ctxs[i]->no_stons = true; // to enable "all_the_same"
        }
        
        else { // fallback if formulas don't work
            seg_by_ctx (VB, STRi(item, i), item_ctxs[i], item_lens[i]);
            item_ctxs[i]->flags.store = STORE_INT; // possibly consumed by the odd items (^)
        }
    }
}

// parameter is two dict_id's (in base64). reconstructs dict1.last_value - dict2.last_value
SPECIAL_RECONSTRUCTOR (vcf_piz_special_MINUS)
{
    DictId two_dicts[2];
    base64_decode (snip, &snip_len, (uint8_t *)two_dicts);

    new_value->i = ECTX (two_dicts[0])->last_value.i - 
                   ECTX (two_dicts[1])->last_value.i;

    if (reconstruct)
        RECONSTRUCT_INT (new_value->i); 

    return true; // has new_value
}

//----------
// FORMAT/AF
// ---------

static inline WordIndex vcf_seg_FORMAT_AF (VBlockVCF *vb, Context *ctx, STRp(cell))
{
    if (vcf_num_samples == 1 && // very little hope that INFO/AF is equal to FORMAT/AF if we have more than one sample
        !z_dual_coords &&       // note: we can't use SNIP_COPY in dual coordinates, because when translating, it will translate the already-translated INFO/AF
        ctx_encountered_in_line_(VB, CTX(INFO_AF)) && 
        str_issame (cell, CTX(INFO_AF)->last_snip))
        return seg_by_ctx (VB, af_snip, af_snip_len, ctx, cell_len);
    else
        return vcf_seg_FORMAT_A_R_G (vb, ctx, con_FORMAT_AF, STRa(cell), STORE_NONE, NULL);
}

//----------
// FORMAT/PS
// ---------

static inline WordIndex vcf_seg_FORMAT_PS (VBlockVCF *vb, Context *ctx, STRp(cell))
{
    ctx->flags.store = STORE_INT;
    ctx->no_stons = true;

    int64_t ps_value=0;
    if (str_get_int (STRa(cell), &ps_value) && ps_value == ctx->last_value.i) // same as previous line
        return seg_by_ctx (VB, ((char []){ SNIP_SELF_DELTA, '0' }), 2, ctx, cell_len);

    return seg_delta_vs_other_do (VB, ctx, CTX(VCF_POS), STRa(cell), 1000, cell_len);
}

//----------
// FORMAT/DP
// ---------

static inline WordIndex vcf_seg_FORMAT_DP (VBlockVCF *vb, Context *ctx, STRp(cell))
{
    // case - we have FORMAT/AD - calculate delta vs the sum of AD components
    if (vcf_has_value_in_sample_(vb, CTX(FORMAT_AD)))
        return seg_delta_vs_other (VB, ctx, CTX(FORMAT_AD), STRa(cell));

    // case - we have FORMAT/SDP - calculate delta vs the sum of AD components
    else if (vcf_has_value_in_sample_(vb, CTX(FORMAT_SDP)))
        return seg_delta_vs_other (VB, ctx, CTX(FORMAT_SDP), STRa(cell));
    
    // case: there is only one sample there is an INFO/DP too, we store a delta 
    else if (vcf_num_samples == 1 && vcf_has_value_in_sample_(vb, CTX(INFO_DP))) 
        return seg_delta_vs_other (VB, ctx, CTX(INFO_DP), STRa(cell));

    // case: no FORMAT/AD and no INFO/DP - store in transposed matrix
    else 
        return vcf_seg_FORMAT_transposed (vb, ctx, STRa(cell), cell_len); // this handles DP that is an integer or '.'
}

// ---------------------
// INFO/AF and FORMAT/AF
// ---------------------

// translate to (max_value - value).
static int32_t vcf_piz_luft_trans_complement_to_max_value (VBlockP vb, ContextP ctx, char *recon, int32_t recon_len, bool validate_only, double max_value)
{
    if (IS_TRIVAL_FORMAT_SUBFIELD) return true; // This is FORMAT field which is empty or "." - all good

    char format[20];
    double f;

    // if we're validating a FORMAT field with --chain (in vcf_seg_validate_luft_trans_one_sample, if REF<>ALT) - accept a valid scientific notation
    // as it will be converted to normal notation in vcf_seg_one_sample
    if (validate_only && chain_is_loaded && dict_id_is_vcf_format_sf (ctx->dict_id) &&
        str_scientific_to_decimal (recon, recon_len, NULL, NULL, &f) && f >= 0.0 && f <= max_value) return true; // scientific notation in the valid range

    // if item format is inconsistent with AF being a probability value - we won't translate it
    if (!str_get_float (recon, recon_len, &f, format, NULL) || f < 0.0 || f > max_value) 
        return false;
    
    if (validate_only) return true; 

    vb->txt_data.len -= recon_len;
    char f_str[50];
    sprintf (f_str, format, max_value - f);
    RECONSTRUCT (f_str, strlen (f_str)); // careful not to use bufprintf as it adds a \0 and we are required to translate in-place for all FORMAT fields
    
    return true;
}


// Lift-over translator for INFO/AF, FORMAT/AF and similar fields, IF it is bi-allelic and we have a ALT<>REF switch.
// We change the probability value to 1-AF
// returns true if successful (return value used only if validate_only)
TRANSLATOR_FUNC (vcf_piz_luft_A_1)
{
    return vcf_piz_luft_trans_complement_to_max_value (vb, ctx, recon, recon_len, validate_only, 1);
}

//----------
// FORMAT/GL
// ---------

// convert an array of probabilities to an array of integer phred scores capped at 60
static void vcf_convert_prob_to_phred (VBlockVCFP vb, const char *flag_name, STRp(snip), char *optimized_snip, unsigned *optimized_snip_len)
{
    str_split_floats (snip, snip_len, 0, ',', prob, false);
    ASSVCF (n_probs, "cannot to apply %s to value \"%.*s\"", flag_name, snip_len, snip); // not an array of floats - abort, because we already changed the FORMAT field

    unsigned phred_len = 0;
    for (unsigned i=0; i < n_probs; i++) {
        
        int64_t phred = MIN_(60, (int64_t)(((-probs[i]) * 10)+0.5)); // round to the nearest int, capped at 60

        phred_len += str_int (phred, &optimized_snip[phred_len]);
        if (i < n_probs - 1)
            optimized_snip[phred_len++] = ',';
    }

    *optimized_snip_len = phred_len;
}

// converts an array of phred scores (possibly floats) to integers capped at 60
static bool vcf_phred_optimize (const char *snip, unsigned len, char *optimized_snip, unsigned *optimized_snip_len /* in / out */)
{
    str_split_floats (snip, len, 0, ',', item, false);
    if (!n_items) return false; // not an array of floats

    unsigned out_len = 0;

    for (unsigned i=0; i < n_items; i++) {
        int64_t new_phred = MIN_(60, (int64_t)(items[i] + 0.5));
        out_len += str_int (new_phred, &optimized_snip[out_len]);
        if (i < n_items-1) optimized_snip[out_len++] = ',';
    }

    *optimized_snip_len = out_len;
    return true;
}

//--------------------
// Multiplex by dosage
// -------------------

void vcf_set_init_mux_by_dosage (VBlockVCFP vb, DidIType did_i, StoreType store_type)
{
    stats_set_consolidation (VB, did_i, 3, did_i+1, did_i+2, did_i+3);

    CTX(did_i  )->flags.store = CTX(did_i+1)->flags.store = 
    CTX(did_i+2)->flags.store = CTX(did_i+3)->flags.store = store_type; 
}

static inline int vcf_seg_get_dosage (VBlockVCFP vb)
{
     return vcf_has_value_in_sample_(vb, CTX(FORMAT_GT)) ? CTX(FORMAT_GT)->last_value.i : -1; // dosage store here by vcf_seg_FORMAT_GT
}

// the 3 contexts after the caller context must be reseved for 0,1,2 dosages
static inline void vcf_seg_FORMAT_mux_by_dosage (VBlockVCF *vb, Context *ctx, STRp(cell))
{
    int dosage = vcf_seg_get_dosage (vb);

    if (dosage >= 0 && dosage <= 2
        && !z_dual_coords) { // don't use this in a DVCF, as it will incorrectly change if REF<>ALT switch as GT changes

        seg_by_ctx (VB, STRa(cell), ctx + dosage + 1, cell_len);
        seg_by_ctx (VB, (char[]){ SNIP_SPECIAL, VCF_SPECIAL_MUX_BY_DOSAGE }, 2, ctx, 0);
    }
    else
        seg_by_ctx (VB, STRa(cell), ctx, cell_len);
}

static inline int vcf_piz_get_dosage (VBlockP vb)
{
    const char *gt = last_txt (vb, FORMAT_GT);
    unsigned gt_len = CTX(FORMAT_GT)->last_txt_len ;

    if (gt_len != 3 || (gt[0]!='0' && gt[0]!='1') || (gt[2]!='0' && gt[2]!='1')) return -1; 

    return (int)gt[0] + (int)gt[2] - 2*'0';
}

SPECIAL_RECONSTRUCTOR (vcf_piz_special_MUX_BY_DOSAGE)
{
    unsigned dosage = vcf_piz_get_dosage (vb);

    DidIType mux_did_i = ctx->did_i + dosage + 1;
    reconstruct_from_ctx (vb, mux_did_i, 0, reconstruct);

    if (ctx->flags.store == STORE_NONE) return false;

    // propagate last_value up
    new_value->i = CTX(mux_did_i)->last_value.i; // note: last_value is a union, this copies the entire union
    return true; 
}

// used for decompressing files compressed with version up to 12.0.42
SPECIAL_RECONSTRUCTOR (vcf_piz_special_FORMAT_DS_old)
{
    if (!reconstruct) goto done;

    char float_format[10];
    int32_t val;
    sscanf (snip, "%s %d", float_format, &val); // snip looks like eg: "%5.3f 50000"

    unsigned dosage = vcf_piz_get_dosage (vb);
    bufprintf (vb, &vb->txt_data, float_format, (double)val / 1000000 + dosage);

done:
    return false; // no new value
}

//----------
// FORMAT/AB
// ---------

// Expecting: '.' if dosage is 0 or 2, another value otherwise
static inline void vcf_seg_FORMAT_AB (VBlockVCF *vb, Context *ctx, STRp(cell))
{
    int dosage = vcf_seg_get_dosage (vb);
    bool dos02 = (dosage==0 || dosage==2);

    bool meets_expectation = (dos02 && cell_len==1 && *cell=='.') || !dos02;

    if (meets_expectation) {
        if (!dos02) seg_by_ctx (VB, STRa(cell), ctx+1, 0);
        seg_by_ctx (VB, (char[]){ SNIP_SPECIAL, VCF_SPECIAL_AB }, 2, ctx, cell_len);
    }
    else 
        seg_by_ctx (VB, STRa(cell), ctx, cell_len);
}

SPECIAL_RECONSTRUCTOR (vcf_piz_special_FORMAT_AB)
{
    unsigned dosage = vcf_piz_get_dosage (vb);
    bool dos02 = (dosage==0 || dosage==2);

    if (dos02 && reconstruct)
        RECONSTRUCT1 ('.');

    else if (!dos02) 
        reconstruct_from_ctx (vb, ctx->did_i + 1, 0, reconstruct);

    return false; // no new value
}

//----------
// FORMAT/DS
// ---------

// Lift-over translator for FORMAT/DS, IF it is bi-allelic and we have a ALT<>REF switch.
// We change the value to (ploidy-value)
// returns true if successful (return value used only if validate_only)
TRANSLATOR_FUNC (vcf_piz_luft_PLOIDY)
{
    if (!vcf_encountered_in_sample_(vb, CTX(FORMAT_GT))) return false; // we can't translate unless this variant as GT

    // use gt_prev_ploidy: in Seg, set by vcf_seg_FORMAT_GT, in validate and piz set by vcf_piz_luft_GT 
    return vcf_piz_luft_trans_complement_to_max_value (vb, ctx, recon, recon_len, validate_only, VB_VCF->gt_prev_ploidy);
}

//----------
// FORMAT/GT
// ---------

// complete haplotypes of lines that don't have GT, if any line in the vblock does have GT.
// In this case, the haplotype matrix must include the lines without GT too
void vcf_seg_FORMAT_GT_complete_missing_lines (VBlockVCF *vb)
{
    buf_alloc (vb, &CTX(FORMAT_GT_HT)->local, 0, vb->lines.len * vb->ht_per_line, char, CTX_GROWTH, "contexts->local");

    for (vb->line_i=0; vb->line_i < (uint32_t)vb->lines.len; vb->line_i++) {

        if (CTX(FORMAT_GT_HT) && !DATA_LINE (vb->line_i)->has_haplotype_data) {
            char *ht_data = ENT (char, CTX(FORMAT_GT_HT)->local, vb->line_i * vb->ht_per_line);
            memset (ht_data, '*', vb->ht_per_line);

            // NOTE: we DONT set dl->has_haplotype_data to true bc downstream we still
            // count this row as having no GT field when analyzing gt data
        }
    }

    CTX(FORMAT_GT_HT)->local.len = vb->lines.len * vb->ht_per_line;
}

// increase ploidy of the previous lines, if higher ploidy was encountered
static void vcf_seg_FORMAT_GT_increase_ploidy (VBlockVCF *vb, unsigned new_ploidy, uint32_t max_new_size)
{
    // protect against highly unlikely case that we don't have enough consumed txt data to store increased-ploidy ht data 
    ASSVCF (new_ploidy * vb->line_i * vcf_num_samples <= max_new_size, 
            "haplotype data overflow due to increased ploidy on line %"PRIu64, vb->line_i);

    uint32_t num_samples = vb->line_i * vcf_num_samples + vb->sample_i; // all samples in previous lines + previous samples in current line
    char *ht_data = FIRSTENT (char, CTX(FORMAT_GT_HT)->local);

    // copy the haplotypes backwards (to avoid overlap), padding with '*' (which are NOT counted in .repeats of the GT container)
    for (int sam_i = num_samples-1; sam_i >= 0; sam_i--) {

        int ht_i=new_ploidy-1 ; for (; ht_i >= vb->ploidy; ht_i--) 
            ht_data[sam_i * new_ploidy + ht_i] = '*'; 

        for (; ht_i >= 0; ht_i--)
            ht_data[sam_i * new_ploidy + ht_i] = ht_data[sam_i * vb->ploidy + ht_i];
    }

    vb->ploidy = new_ploidy;
    vb->ht_per_line = vb->ploidy * vcf_num_samples;
}

static inline WordIndex vcf_seg_FORMAT_GT (VBlockVCF *vb, Context *ctx, ZipDataLineVCF *dl, STRp(cell))
{
    // the GT field is represented as a Container, with a single item repeating as required by poidy, and the separator 
    // determined by the phase
    MiniContainer gt = { .repeats = 1, 
                         .nitems_lo = 1, 
                         .drop_final_repeat_sep = true, 
                         .callback = (vb->use_special_sf == USE_SF_YES),
                         .items = { { .dict_id = (DictId)_FORMAT_GT_HT } },
                       };

    unsigned save_cell_len = cell_len;

    // update repeats according to ploidy, and separator according to phase
    for (unsigned i=1; i<cell_len-1; i++)
        if (cell[i] == '|' || cell[i] == '/') {
            gt.repeats++;
            gt.repsep[0] = cell[i];
        }

    ASSVCF (gt.repeats <= VCF_MAX_PLOIDY, "ploidy=%u exceeds the maximum of %u", gt.repeats, VCF_MAX_PLOIDY);
    
    // if the ploidy of this line is bigger than the ploidy of the data in this VB so far, then
    // we have to increase ploidy of all the haplotypes read in in this VB so far. This can happen for example in 
    // the X chromosome if initial samples are male with ploidy=1 and then a female sample with ploidy=2
    if (vb->ploidy && gt.repeats > vb->ploidy) 
        vcf_seg_FORMAT_GT_increase_ploidy (vb, gt.repeats, ENTNUM (vb->txt_data, cell));

    if (!vb->ploidy) {
        vb->ploidy = gt.repeats; // very first sample in the vb
        vb->ht_per_line = vb->ploidy * vcf_num_samples;
    }

    buf_alloc (vb, &CTX(FORMAT_GT_HT)->local, vb->ploidy, vb->ht_per_line * vb->lines.len, char, CTX_GROWTH, "contexts->local");

    // note - ploidy of this sample might be smaller than vb->ploidy (eg a male sample in an X chromosesome that was preceded by a female sample, or "." sample)
    Allele *ht_data = ENT (Allele, CTX(FORMAT_GT_HT)->local, vb->line_i * vb->ht_per_line + vb->ploidy * vb->sample_i);

    int64_t dosage=0; // sum of allele values
    for (unsigned ht_i=0; ht_i < gt.repeats; ht_i++) {

        Allele ht = *(cell++); 
        cell_len--;

        ASSVCF (IS_DIGIT(ht) || ht == '.', 
                "invalid VCF file - expecting an allele in a sample to be a number 0-99 or . , but seeing %c (ht_i=%u)", ht, ht_i);

        // single-digit allele numbers
        ht_data[ht_i] = ht;

        // calculate dosage contribution of this ht (to be used in vcf_seg_FORMAT_mux_by_dosage)
        if (dosage >= 0 && (ht == '0' || ht == '1'))
            dosage += ht - '0'; // dosage only works if alleles are 0 or 1
        else
            dosage = -1; // no dosage

        if (!cell_len) break;

        // handle 2-digit allele numbers
        if (ht != '.' && IS_DIGIT (*cell)) {
            unsigned allele = 10 * (ht-'0') + (*(cell++) - '0');
            cell_len--;

            // make sure there isn't a 3rd digit
            ASSVCF (!cell_len || !IS_DIGIT (*cell), "VCF file sample %u - genozip currently supports only alleles up to 99", vb->sample_i+1);

            ht_data[ht_i] = '0' + allele; // use ascii 48->147

            dosage = -1; // no dosage (since allele is not 0 or 1)
        }

        // read and verify phase
        if (gt.repeats > 1 && ht_i < gt.repeats-1) {
            
            char phase = *(cell++);
            cell_len--;

            ASSVCF (phase != ' ', "invalid VCF file - expecting a tab or newline after sample %u but seeing a space", vb->sample_i+1);
            ASSVCF (phase == gt.repsep[0], "invalid VCF file -  unable to parse sample %u: expecting a %c but seeing %c", vb->sample_i+1, gt.repsep[0], phase);
        }
    } // for characters in a sample

    // if the ploidy of the sample is lower than vb->ploidy, set missing ht as '-' (which will cause deletion of themselves and their separator)
    // and set the ploidy to vb->ploidy - to avoid increase in entroy of GT.b250
    if (gt.repeats != vb->ploidy) {
        
        for (unsigned ht_i=gt.repeats; ht_i < vb->ploidy; ht_i++) 
            ht_data[ht_i] = '-'; // unlike '*', we DO count '-' in .repeats (so that we can have the same number of repeats = lower entroy in GT.b250)

        gt.repeats = vb->ploidy;
        if (!gt.repsep[0]) gt.repsep[0] = vb->gt_prev_phase; // this happens in case if a 1-ploid sample
    }

    // if this sample is a "./." - replace it with "%|%" or "%/%" according to the previous sample's phase -  
    // so that the gt container is likely identical and we reduce GT.b250 entropy. Reason: many tools
    // (including bcftools merge) produce "./." for missing samples even if all other samples are phased
    if (ht_data[0]=='.' && gt.repeats==2 && ht_data[1]=='.' && gt.repsep[0]=='/') {
        gt.repsep[0] = vb->gt_prev_phase ? vb->gt_prev_phase : '|'; // '|' is arbitrary
        ht_data[0] = ht_data[1] = '%';
    }

    // in case we have INFO/SF, we verify that it is indeed the list of samples for which the first ht is not '.'
    if (vb->use_special_sf == USE_SF_YES && ht_data[0] != '.') 
        vcf_seg_INFO_SF_one_sample (vb);

    vcf_set_last_sample_value_ (vb, ctx, dosage); // to be used in vcf_seg_FORMAT_mux_by_dosage

    ASSVCF (!cell_len, "Invalid GT data in sample_i=%u", vb->sample_i+1);

    // shortcut if we have the same ploidy and phase as previous GT (saves re-genetrating base64 in container_seg)
    if (gt.repeats == vb->gt_prev_ploidy && gt.repsep[0] == vb->gt_prev_phase) 
        return seg_duplicate_last (VB, ctx, save_cell_len);

    else {
        vb->gt_prev_ploidy = gt.repeats;
        vb->gt_prev_phase  = gt.repsep[0];
        return container_seg (vb, ctx, (ContainerP)&gt, 0, 0, save_cell_len); 
    }
}

// Lift-over translator assigned to a FORMAT/GT item, IF it is bi-allelic and we have a ALT<>REF switch. No limitations on ploidy.
// We switch 0<>1. If its unphased (only /) - we list the 0s first, then the 1s
TRANSLATOR_FUNC (vcf_piz_luft_GT)
{
    // validate. make sure this is a bi-allelic genotype (no ploidy limitation)
    for (uint32_t i=0; i < recon_len; i += 2)  
        if (recon[i] != '0' && recon[i] != '1' && recon[i] != '.') return false;

    for (uint32_t i=1; i < recon_len; i += 2)  
        if (recon[i] != '/' && recon[i] != '|') return false;

    VB_VCF->gt_prev_ploidy = (recon_len+1) / 2; // consumed by vcf_piz_luft_PLOIDY

    if (validate_only) return true;

    // exchange 0 <> 1
    for (uint32_t i=0; i < recon_len; i += 2)
        if      (recon[i] == '0') recon[i] = '1';
        else if (recon[i] == '1') recon[i] = '0';

    return true;    
}

//------------------------------------------------
// FORMAT and INFO - subfields with G, R, R2 types
//------------------------------------------------

// Lift-over ALT<>REF switch translator for bi-allelic multi-value fields: 
// three cases: (1) R1,R2->R2,R1 (2) Ra1,Rb1,Ra2,Rb2->Ra2,Rb2,Ra1,Rb1 (3) G11,G12,G22->G22,G12,G11
// returns true if successful (return value used only if validate_only)
static bool vcf_piz_luft_switch_first_last (VBlockP vb, ContextP ctx, char *recon, int32_t recon_len, 
                                           unsigned num_items, char field_type, bool validate_only)
{
    if (IS_TRIVAL_FORMAT_SUBFIELD) return true; // This is FORMAT field which is empty or "." - all good

    char copy[recon_len];
    memcpy (copy, recon, recon_len);
    
    str_split (copy, recon_len, num_items, ',', item, true);
    if (!n_items) return false; // if item format is inconsistent with VCF header - we won't translate it

    if (validate_only) return true;
    
    vb->txt_data.len -= recon_len;
    
    if (num_items==2 || num_items == 3) {
        RECONSTRUCT_SEP (items[num_items-1], item_lens[num_items-1], ',');

        if (num_items==3)
            RECONSTRUCT_SEP (items[1], item_lens[1], ',');

        RECONSTRUCT (items[0], item_lens[0]);
    }
    else if (num_items == 4) { // Ra1,Rb1,Ra2,Rb2 -> Ra2,Rb2,Ra1,Rb1
        RECONSTRUCT_SEP (items[2], item_lens[2], ',');
        RECONSTRUCT_SEP (items[3], item_lens[3], ',');
        RECONSTRUCT_SEP (items[0], item_lens[0], ',');
        RECONSTRUCT     (items[1], item_lens[1]);
    }
    
    return true;
}

// Lift-over translator assigned to a Number=R item, IF it is bi-allelic and we have a ALT<>REF switch.
// 'R' : We switch between the two comma-separated values.
// 'R2': We switch between the two PAIRS of comma-separated values.
// 'G' : We have 3 values which represent the genotypes REF/REF,REF/ALT,ALT/ALT We switch between the 1st and 3rd value.
// returns true if successful 
TRANSLATOR_FUNC (vcf_piz_luft_R)  { return vcf_piz_luft_switch_first_last (vb, ctx, recon, recon_len, 2, 'R', validate_only); } // 2 bc we only handle bi-allelic
TRANSLATOR_FUNC (vcf_piz_luft_R2) { return vcf_piz_luft_switch_first_last (vb, ctx, recon, recon_len, 4, '.', validate_only); } // 4 bc we only handle bi-allelic

TRANSLATOR_FUNC (vcf_piz_luft_G)  
{ 
    if (IS_TRIVAL_FORMAT_SUBFIELD) return true; // This is FORMAT field which is empty or "." - all good

    unsigned num_values = str_count_char (recon, recon_len, ',')+1;
    if (num_values != 3 && num_values != 2) return false; // Genozip currently only support haploid (2 bi-allelic genotypes) and diploid (3 bi-allelic genotypes) 

    return vcf_piz_luft_switch_first_last (vb, ctx, recon, recon_len, num_values, 'G', validate_only); 
}

//------------------------------------------------------------------------
// Validate that ALL subfields in ALL samples can luft-translate as needed
//------------------------------------------------------------------------

static const char *error_format_field (unsigned n_items, ContextP *ctxs)
{
    static char format[256];
    unsigned len=0;
    for (unsigned i=0; i < n_items; i++) 
        len += strlen (ctxs[i]->tag_name) + 1;

    if (len > sizeof format-1) return "<FORMAT too long to display>";

    len=0;
    for (unsigned i=0; i < n_items; i++) {
        unsigned one_len = strlen (ctxs[i]->tag_name);
        memcpy (&format[len], ctxs[i]->tag_name, one_len); 
        len += one_len;
        format[len++] = ':';
    } 

    format[len-1] = 0;
    return format;
}

// if any context fails luft-translation, returns that context, or if all is good, returns NULL
static inline Context *vcf_seg_validate_luft_trans_one_sample (VBlockVCF *vb, ContextP *ctxs, uint32_t num_items, char *sample, unsigned sample_len)
{
    str_split (sample, sample_len, num_items, ':', item, false);
    ASSVCF (n_items, "Sample %u has too many subfields - FORMAT field \"%s\" specifies only %u: \"%.*s\"", 
            vb->sample_i+1, error_format_field (num_items, ctxs), num_items, sample_len, sample);

    ContextP failed_ctx = NULL; // optimistic initialization - nothing failed

    uint32_t save_ploidy = vb->gt_prev_ploidy; // ruined by vcf_piz_luft_GT 
    
    for (unsigned i=0; i < n_items; i++) {
        if (needs_translation (ctxs[i]) && item_lens[i]) {
            if ((vb->line_coords == DC_LUFT && !vcf_lo_seg_cross_render_to_primary (vb, ctxs[i], STRi(item,i), NULL, NULL)) ||
                (vb->line_coords == DC_PRIMARY && !(DT_FUNC(vb, translator)[ctxs[i]->luft_trans](VB, ctxs[i], (char *)STRi(item,i), 0, true)))) {
                failed_ctx = ctxs[i];  // failed translation
                break;
            }
        }
        vcf_set_encountered_in_sample (ctxs[i]); // might be needed for validation 
    }

    // reset modified values, in preparation for real Seg
    vb->gt_prev_ploidy = save_ploidy;
    for (unsigned i=0; i < n_items; i++) 
        vcf_reset_encountered_in_sample (ctxs[i]); 

    return failed_ctx; 
}

// If ALL subfields in ALL samples can luft-translate as required: 1.sets ctx->line_is_luft_trans for all contexts 2.lifted-back if this is a LUFT lne
// if NOT: ctx->line_is_luft_trans=false for all contexts, line is rejects (LO_FORMAT), and keeps samples in their original LUFT or PRIMARY coordinates.
static inline void vcf_seg_validate_luft_trans_all_samples (VBlockVCF *vb, uint32_t num_items, ContextP *ctxs, 
                                                            int32_t len, char *samples_start,
                                                            const char *backup_luft_samples, uint32_t backup_luft_samples_len)
{
    const char *field_start, *next_field = samples_start;
    unsigned field_len=0;
    bool has_13;

    // initialize optimistically. we will roll back and set to false if ANY subfield in ANY sample fails to translate, and re-seg all samples
    for (unsigned sf_i=0; sf_i < num_items; sf_i++)
        ctxs[sf_i]->line_is_luft_trans = needs_translation (ctxs[sf_i]); 

    // 0 or more samples
    vb->sample_i=0;
    for (char separator=0 ; separator != '\n'; vb->sample_i++) {

        field_start = next_field;
        next_field = seg_get_next_item (vb, field_start, &len, GN_SEP, GN_SEP, GN_IGNORE, &field_len, &separator, &has_13, "sample-subfield");
        ASSVCF (field_len, "unexpected tab character after sample # %u", vb->sample_i);

        Context *failed_ctx = vcf_seg_validate_luft_trans_one_sample (vb, ctxs, num_items, (char *)field_start, field_len);
        if (failed_ctx) { // some context doesn't luft-translate as required
            REJECT_SUBFIELD (LO_FORMAT, failed_ctx, ".\tCannot cross-render sample due to field %s: \"%.*s\"", failed_ctx->tag_name, field_len, field_start);

            // make all contexts untranslateable in this line
            for (unsigned i=0; i < num_items; i++)  // iterate on the order as in the line
                ctxs[i]->line_is_luft_trans = false;

            // if this is an untranslatable LUFT-only line, recover the original LUFT-coordinates samples
            if (vb->line_coords == DC_LUFT) 
                memcpy (samples_start, backup_luft_samples, backup_luft_samples_len);
        }
    }
}

// ----------
// One sample
// ----------

// returns the number of colons in the sample
static inline unsigned vcf_seg_one_sample (VBlockVCF *vb, ZipDataLineVCF *dl, ContextP *ctxs, ContainerP samples, const char *sample, unsigned sample_len)
{
    str_split (sample, sample_len, con_nitems (*samples), ':', sf, false);

    ASSVCF (n_sfs, "Sample %u has too many subfields - FORMAT field \"%s\" specifies only %u: \"%.*s\"", 
            vb->sample_i+1, error_format_field (con_nitems (*samples), ctxs), con_nitems (*samples), sample_len, sample);

    for (unsigned i=0; i < n_sfs; i++) { 

        DictId dict_id = samples->items[i].dict_id;
        Context *ctx = ctxs[i], *other_ctx;

        unsigned modified_len = sf_lens[i]*2 + 10;
        char modified[modified_len]; // theoritcal risk of stack overflow if subfield value is very large

#       define SEG_OPTIMIZED_MUX_BY_DOSAGE do {                         \
            vcf_seg_FORMAT_mux_by_dosage (vb, ctx, STRa(modified));     \
            int32_t shrinkage = (int)sf_lens[i] - (int)modified_len;    \
            vb->recon_size      -= shrinkage;                           \
            vb->recon_size_luft -= shrinkage;                           \
        } while (0)

        // --chain: if this is RendAlg=A_1 and RendAlg=PLOIDY subfield, convert a eg 4.31e-03 to e.g. 0.00431. This is to
        // ensure primary->luft->primary is lossless (4.31e-03 cannot be converted losslessly as we can't preserve format info)
        if (chain_is_loaded && (ctx->luft_trans == VCF2VCF_A_1 || ctx->luft_trans == VCF2VCF_PLOIDY) && 
            str_scientific_to_decimal (STRi(sf, i), modified, &modified_len, NULL)) {
            
            int32_t shrinkage = (int32_t)sf_lens[i] - (int32_t)modified_len; // possibly negative = growth
            vb->recon_size      -= shrinkage; 
            vb->recon_size_luft -= shrinkage; 
            sfs[i] = modified; 
            sf_lens[i] = modified_len; 
        }

        if (!sf_lens[i])
            seg_by_ctx (VB, "", 0, ctx, 0); // generates WORD_INDEX_EMPTY

        // note: cannot use switch bc dict_id_* are variables, not constants

        // ##FORMAT=<ID=GT,Number=1,Type=String,Description="Genotype">
        else if (dict_id.num == _FORMAT_GT)
            vcf_seg_FORMAT_GT (vb, ctx, dl, STRi(sf, i));

        // --GL-to-PL:  GL: 0.00,-0.60,-8.40 -> PL: 0,6,60
        // note: we changed the FORMAT field GL->PL in vcf_seg_format_field. data is still stored in the GL context.
        // ##FORMAT=<ID=PL,Number=G,Type=Integer,Description="Phred-scaled genotype likelihoods rounded to the closest integer">
        else if (flag.GL_to_PL && dict_id.num == _FORMAT_GL) {
            vcf_convert_prob_to_phred (vb, "--GL-to-PL", STRi(sf, i), modified, &modified_len);
            SEG_OPTIMIZED_MUX_BY_DOSAGE;
        }

        // convert GP (probabilities) to PP (phred values). applicable for v4.3 and over
        // ##FORMAT=<ID=PP,Number=G,Type=Integer,Description="Phred-scaled genotype posterior probabilities rounded to the closest integer">
        else if (flag.GP_to_PP && dict_id.num == _FORMAT_GP && vb->vcf_version >= VCF_v4_3) {
            vcf_convert_prob_to_phred (vb, "--GP-to-PP", STRi(sf, i), modified, &modified_len);
            SEG_OPTIMIZED_MUX_BY_DOSAGE;
        }

        // note: GP and PL - for non-optimized, I tested segging as A_R_G and seg_array - they are worse or not better than the default. likely because the values are correlated.

        // ##FORMAT=<ID=PL,Number=G,Type=Integer,Description="Normalized, Phred-scaled likelihoods for genotypes as defined in the VCF specification">       
        else if (flag.optimize_phred && 
                 (  dict_id.num == _FORMAT_PL  || 
                    dict_id.num == _FORMAT_PP  || 
                    dict_id.num == _FORMAT_PRI || 
                    (dict_id.num == _FORMAT_GP && vb->vcf_version <= VCF_v4_2)) && // up to v4.2 GP contained phred values (since 4.3 it contains probabilities) 
                 vcf_phred_optimize (STRi(sf, i), modified, &modified_len)) 
            SEG_OPTIMIZED_MUX_BY_DOSAGE;

        // This is good for Number=G fields, and other fields correlated to the dosage. To use:
        // 1. three addition contexts, directly following the main context, must be defined in vcf.h #pragma GENDICT
        // 2. stats_set_consolidation must be called in vcf_seg_initialize
        else if (dict_id.num == _FORMAT_GL  || dict_id.num == _FORMAT_DS   || dict_id.num == _FORMAT_PL || 
                 dict_id.num == _FORMAT_PP  || dict_id.num == _FORMAT_GP   || dict_id.num == _FORMAT_RD ||
                 dict_id.num == _FORMAT_PRI || dict_id.num == _FORMAT_PVAL || dict_id.num == _FORMAT_FREQ)
            vcf_seg_FORMAT_mux_by_dosage (vb, ctx, STRi (sf, i));

        // case: PS ("Phase Set") - might be the same as POS (for example, if set by Whatshap: https://whatshap.readthedocs.io/en/latest/guide.html#features-and-limitations)
        // or might be the same as the previous line
        else if (dict_id.num == _FORMAT_PS) 
            vcf_seg_FORMAT_PS (vb, ctx, STRi(sf, i));

        // standard: ##FORMAT=<ID=GQ,Number=1,Type=Integer,Description="Genotype Quality">
        // GIAB: ##FORMAT=<ID=GQ,Number=1,Type=Integer,Description="Net Genotype quality across all datasets, calculated from GQ scores of callsets supporting the consensus GT, using only one callset from each dataset">   
        else if (dict_id.num == _FORMAT_GQ) 
            vcf_seg_FORMAT_transposed (vb, ctx, STRi(sf, i), sf_lens[i]);
            
        // ##FORMAT=<ID=DP,Number=1,Type=Integer,Description="Approximate read depth (reads with MQ=255 or with bad mates are filtered)">
        else if (dict_id.num == _FORMAT_DP) 
            vcf_seg_FORMAT_DP (vb, ctx, STRi(sf, i));
            
        // ##FORMAT=<ID=MIN_DP,Number=1,Type=Integer,Description="Minimum DP observed within the GVCF block">
        // case: MIN_DP - it is slightly smaller and usually equal to DP - we store MIN_DP as the delta DP-MIN_DP
        // note: the delta is vs. the DP field that preceeds MIN_DP - we take the DP as 0 there is no DP that preceeds
        else if (dict_id.num == _FORMAT_MIN_DP && vcf_has_value_in_sample (vb, _FORMAT_DP, &other_ctx)) 
            seg_delta_vs_other (VB, ctx, other_ctx, STRi(sf, i));

        else if (dict_id.num == _FORMAT_SDP && ctx_has_value_in_line_(VB, CTX(INFO_ADP))) 
            seg_delta_vs_other (VB, ctx, CTX(INFO_ADP), STRi(sf, i));

        else if (dict_id.num == _FORMAT_AF) 
            vcf_seg_FORMAT_AF (vb, ctx, STRi(sf, i));

        // standard: ##FORMAT=<ID=AD,Number=.,Type=Integer,Description="Allelic depths for the ref and alt alleles in the order listed">  
        // GIAB: ##FORMAT=<ID=AD,Number=R,Type=Integer,Description="Net allele depths across all unfiltered datasets with called genotype">
        else if (dict_id.num == _FORMAT_AD) 
            vcf_seg_FORMAT_A_R_G (vb, ctx, con_FORMAT_AD, STRi(sf, i), STORE_INT, vcf_seg_AD_items);

        // GIAB: ##FORMAT=<ID=ADALL,Number=R,Type=Integer,Description="Net allele depths across all datasets">
        else if (dict_id.num == _FORMAT_ADALL) 
            vcf_seg_FORMAT_A_R_G (vb, ctx, con_FORMAT_ADALL, STRi(sf, i), STORE_INT, vcf_seg_ADALL_items);

        else if (dict_id.num == _FORMAT_ADF) 
            vcf_seg_FORMAT_A_R_G (vb, ctx, con_FORMAT_ADF, STRi(sf, i), STORE_INT, vcf_seg_ADF_items);

        else if (dict_id.num == _FORMAT_ADR) 
            vcf_seg_FORMAT_A_R_G (vb, ctx, con_FORMAT_ADR, STRi(sf, i), STORE_INT, vcf_seg_ADR_items);

        // ##FORMAT=<ID=F1R2,Number=R,Type=Integer,Description="Count of reads in F1R2 pair orientation supporting each allele">
        else if (dict_id.num == _FORMAT_F1R2) 
            vcf_seg_FORMAT_A_R_G (vb, ctx, con_FORMAT_F1R2, STRi(sf, i), STORE_INT, NULL);

        // ##FORMAT=<ID=F2R1,Number=R,Type=Integer,Description="Count of reads in F2R1 pair orientation supporting each allele">
        else if (dict_id.num == _FORMAT_F2R1) 
            vcf_seg_FORMAT_A_R_G (vb, ctx, con_FORMAT_F2R1, STRi(sf, i), STORE_INT, vcf_seg_F2R1_items);

        // ##FORMAT=<ID=SB,Number=4,Type=Integer,Description="Per-sample component statistics which comprise the Fisher's Exact Test to detect strand bias">
        else if (dict_id.num == _FORMAT_SB) 
            vcf_seg_FORMAT_A_R_G (vb, ctx, con_FORMAT_SB, STRi(sf, i), STORE_NONE, vcf_seg_SB_items);

        // ##FORMAT=<ID=MB,Number=4,Type=Integer,Description="Per-sample component statistics to detect mate bias">
        else if (dict_id.num == _FORMAT_MB) 
            vcf_seg_FORMAT_A_R_G (vb, ctx, con_FORMAT_MB, STRi(sf, i), STORE_NONE, vcf_seg_MB_items);

        // ##FORMAT=<ID=SAC,Number=.,Type=Integer,Description="Number of reads on the forward and reverse strand supporting each allele (including reference)">
        else if (dict_id.num == _FORMAT_SAC) 
            vcf_seg_FORMAT_A_R_G (vb, ctx, con_FORMAT_SAC, STRi(sf, i), STORE_NONE, vcf_seg_SAC_items);

        else if (dict_id.num == _FORMAT_RDF) 
            vcf_seg_FORMAT_minus (vb, ctx, STRi(sf, i), 0, CTX(FORMAT_RD), CTX(FORMAT_RDR), STRa(rdf_snip));

        else if (dict_id.num == _FORMAT_RDR) 
            vcf_seg_FORMAT_minus (vb, ctx, STRi(sf, i), 0, CTX(FORMAT_RD), CTX(FORMAT_RDF), STRa(rdr_snip));

        else if (dict_id.num == _FORMAT_AB) 
            vcf_seg_FORMAT_AB (vb, ctx, STRi(sf, i));

        else  // default
            seg_by_ctx (VB, STRi(sf, i), ctx, sf_lens[i]);

        if (ctx->flags.store == STORE_INT && !vcf_has_value_in_sample_(vb, ctx)) { // not already set
            int64_t value;
            if (str_get_int (STRi(sf, i), &value))
                vcf_set_last_sample_value_(vb, ctx, value);
        }
        
        vcf_set_encountered_in_sample (ctx);
    }

    // missing subfields - defined in FORMAT but missing (not merely empty) in sample
    for (unsigned i=n_sfs; i < con_nitems (*samples); i++)  
        seg_by_ctx (VB, NULL, 0, ctxs[i], 0); // generates WORD_INDEX_MISSING

    return n_sfs - 1; // number of colons
}

//------------
// All samples
//------------

const char *vcf_seg_samples (VBlockVCF *vb, ZipDataLineVCF *dl, int32_t *len, char *next_field, bool *has_13,
                             const char *backup_luft_samples, uint32_t backup_luft_samples_len)
{
    // Container for samples - we have:
    // - repeats as the number of samples in the line (<= vcf_num_samples)
    // - num_items as the number of FORMAT subfields (inc. GT)

    Container samples = *ENT (Container, vb->format_mapper_buf, dl->format_node_i); // make a copy of the template
    ContextP *ctxs = ENT (ContextP, vb->format_contexts, dl->format_node_i * MAX_FIELDS);
    uint32_t num_items = con_nitems (samples);

    // check that all subfields in all samples can be luft-translated as required, or make this a LUFT-only / PRIMARY-only line.
    // Also, if the data is in LUFT coordinates and is indeed translatable, then this lifts-back the samples to PRIMARY coordinates
    if (z_dual_coords && LO_IS_OK (last_ostatus))
        vcf_seg_validate_luft_trans_all_samples (vb, num_items, ctxs, *len, next_field, backup_luft_samples, backup_luft_samples_len);

    const char *field_start;
    unsigned field_len=0, num_colons=0;

    // 0 or more samples
    for (char separator=0 ; separator != '\n'; samples.repeats++) {

        field_start = next_field;
        next_field = (char *)seg_get_next_item (vb, field_start, len, GN_SEP, GN_SEP, GN_IGNORE, &field_len, &separator, has_13, "sample-subfield");

        ASSVCF (field_len, "Error: invalid VCF file - expecting sample data for sample # %u, but found a tab character", 
                samples.repeats+1);

        vb->sample_i = samples.repeats;
        num_colons += vcf_seg_one_sample (vb, dl, ctxs, &samples, (char *)field_start, field_len);

        ASSVCF (samples.repeats < vcf_num_samples || separator == '\n',
                "invalid VCF file - expecting a newline after the last sample (sample #%u)", vcf_num_samples);
    }

    ASSVCF (samples.repeats <= vcf_num_samples, "according the VCF header, there should be %u sample%s per line, but this line has %u samples - that's too many",
            vcf_num_samples, vcf_num_samples==1 ? "" : "s", samples.repeats);

    // in some real-world files I encountered have too-short lines due to human errors. we pad them
    if (samples.repeats < vcf_num_samples) {
        WARN_ONCE ("FYI: the number of samples in variant CHROM=%.*s POS=%"PRId64" is %u, different than the VCF column header line which has %u samples",
                   vb->chrom_name_len, vb->chrom_name, vb->last_int (VCF_POS), samples.repeats, vcf_num_samples);

        if (dl->has_haplotype_data) {
            char *ht_data = ENT (char, CTX(FORMAT_GT_HT)->local, vb->line_i * vb->ploidy * vcf_num_samples + vb->ploidy * samples.repeats);
            unsigned num_missing = vb->ploidy * (vcf_num_samples - samples.repeats); 
            memset (ht_data, '*', num_missing);
        }
    }
    
    // assign all translators. note: we either have translators for all translatable items, or none at all.
    if (z_dual_coords)
        for (uint32_t i=0; i < num_items; i++)
            if (ctxs[i]->line_is_luft_trans)
                samples.items[i].translator = ctxs[i]->luft_trans;

    container_seg (vb, CTX(VCF_SAMPLES), &samples, 0, 0, samples.repeats + num_colons); // account for : and \t \r \n separators

    CTX(FORMAT_GT_HT)->local.len = (vb->line_i+1) * vb->ht_per_line;
 
    return next_field;
}

