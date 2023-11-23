// ------------------------------------------------------------------
//   dict_io.h
//   Copyright (C) 2019-2023 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited,
//   under penalties specified in the license.

#pragma once

extern void dict_io_read_all_dictionaries (void);
extern void dict_io_compress_dictionaries (void);

extern void dict_io_print (FILE *fp, STRp(data), bool with_word_index, bool add_newline, bool remove_equal_asterisk);
extern void dict_io_show_singletons (VBlockP vb, ContextP ctx);
