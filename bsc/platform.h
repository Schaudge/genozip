// ------------------------------------------------------------------
//   platform.h
//   Copyright (C) 2019-2020 Divon Lan <divon@genozip.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

#include "../genozip.h"

#define ALPHABET_SIZE 256

void *(* bsc_malloc)(void *vb, uint64_t size);
void  (* bsc_free)(void *vb, void *address);
void *bsc_zero_malloc (void *vb, uint64_t size);
