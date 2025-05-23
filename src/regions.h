// ------------------------------------------------------------------
//   regions.h
//   Copyright (C) 2020-2025 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#pragma once

#include "genozip.h"

extern void regions_add (rom reg_str);
extern void regions_add_by_file (rom regions_filename);
extern void regions_make_chregs (ContextP chrom_ctx);
extern void regions_transform_negative_to_positive_complement(void);
extern bool regions_get_ra_intersection (WordIndex chrom_node_index, PosType64 min_pos, PosType64 max_pos);

extern unsigned regions_get_num_range_intersections (WordIndex chrom_word_index);
extern bool regions_get_range_intersection (WordIndex chrom_word_index, PosType64 min_pos, PosType64 max_pos, unsigned intersect_i, PosType64 *intersect_min_pos, PosType64 *intersect_max_pos, bool *revcomp);

extern unsigned regions_max_num_chregs(void);
extern void regions_display(rom title);
extern bool regions_is_site_included (VBlockP vb);
extern bool regions_is_range_included (WordIndex chrom, PosType64 start_pos, PosType64 end_pos, bool completely_included);
#define regions_is_ra_included(ra) regions_is_range_included(ra->chrom_index, ra->min_pos, ra->max_pos, false)

extern void regions_destroy (void);