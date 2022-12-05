// ------------------------------------------------------------------
//   digest.h
//   Copyright (C) 2020-2022 Genozip Limited
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.

#pragma once

#include "genozip.h"

// Digest must be packed as it appears in a Section in the Genozip file format (will only be meaningful on CPUs with more than 128 bit though...)
#pragma pack(1) 

typedef union { 
    // used if the digest is MD5
    uint8_t bytes[16]; 
    uint32_t words[4];
    uint128_t w128;

    // used if the digest is Adler32
#define adler_bgen words[0] // Big Endian
} Digest;

#define DIGEST_NONE ((Digest){})

#pragma pack()

typedef struct {
    bool initialized;
    bool log;
    uint64_t bytes_digested;
    uint32_t     lo, hi;
    uint32_t     a, b, c, d;
    union {
        uint8_t  bytes[64];
        uint32_t words[16];
    } buffer;
} Md5Context;

typedef struct {
    bool initialized;
    bool log;
    uint64_t bytes_digested;
    uint32_t adler; // native endianity
} AdlerContext;

typedef union {
    struct {
        bool initialized;
        bool log;
        uint64_t bytes_digested; 
    } common;
    Md5Context md5_ctx;
    AdlerContext adler_ctx;
} DigestContext;

#define DIGEST_CONTEXT_NONE (DigestContext){}

extern Digest digest_snapshot (const DigestContext *ctx, rom msg);
extern Digest digest_txt_header (BufferP data, Digest piz_expected_digest);
extern bool digest_one_vb (VBlockP vb, bool is_compute_thread, BufferP data);

typedef struct { char s[34]; } DigestDisplay;
extern DigestDisplay digest_display (Digest digest);

typedef enum { DD_NORMAL, DD_MD5, DD_MD5_IF_MD5, DD_SHORT } DigestDisplayMode;
extern DigestDisplay digest_display_ex (const Digest digest, DigestDisplayMode mode);
extern rom digest_name (void);

#define digest_is_equal(digest1,digest2) ((digest1).w128 == (digest2).w128)
extern bool digest_recon_is_equal (const Digest recon_digest, const Digest expected_digest);
extern void digest_verify_ref_is_equal (const Reference ref, rom header_ref_filename, const Digest header_md5);

#define md5_is_zero(digest) (!(digest).w128)
#define v8_digest_is_zero(digest) (IS_PIZ && !VER(9) && md5_is_zero(digest))
#define digest_is_zero md5_is_zero

#define piz_need_digest (!v8_digest_is_zero (z_file->digest) && !flag.data_modified && !flag.reading_chain) 

// backward compatability note: in v8 files compressed without --md5 or --test, we had no digest. starting v9, we have Adler32
