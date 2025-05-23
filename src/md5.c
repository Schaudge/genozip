// ------------------------------------------------------------------
//   aes.c
//   Copyright (C) 2019-2025 Genozip Limited. Patent Pending.
//   Please see terms and conditions in the file LICENSE.txt
//
//   WARNING: Genozip is proprietary, not open source software. Modifying the source code is strictly prohibited
//   and subject to penalties specified in the license.
//   Copyright claimed on additions and modifications vs public domain.
//
// This is an implementation of MD5 based on: https://github.com/WaterJuice/WjCryptLib/blob/master/lib/WjCryptLib_Md5.c
// which has been modified extensively. The unmodified terms of the license are as follows:
// >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> 
// This is free and unencumbered software released into the public domain.
//
// Anyone is free to copy, modify, publish, use, compile, sell, or
// distribute this software, either in source code form or as a compiled
// binary, for any purpose, commercial or non-commercial, and by any
// means.
//    
// In jurisdictions that recognize copyright laws, the author or authors
// of this software dedicate any and all copyright interest in the
// software to the public domain. We make this dedication for the benefit
// of the public at large and to the detriment of our heirs and
// successors. We intend this dedication to be an overt act of
// relinquishment in perpetuity of all present and future rights to this
// software under copyright law.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.
// 
// For more information, please refer to <http://unlicense.org/>
// <<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<<

#include <memory.h>
#include "md5.h"
#include "vblock.h"

#define F( x, y, z )            ( (z) ^ ((x) & ((y) ^ (z))) )
#define G( x, y, z )            ( (y) ^ ((z) & ((x) ^ (y))) )
#define H( x, y, z )            ( (x) ^ (y) ^ (z) )
#define I( x, y, z )            ( (y) ^ ((x) | ~(z)) )

#define STEP( f, a, b, c, d, x, t, s )                          \
    (a) += f((b), (c), (d)) + (x) + (t);                        \
    (a) = (((a) << (s)) | (((a) & 0xffffffff) >> (32 - (s))));  \
    (a) += (b);

void md5_display_state (const Md5State *x) // for debugging
{
    static unsigned iteration=1;

    fprintf (stderr, "\n%2u: %08x %08x %08x %08x %08x %08x ", iteration, x->hi, x->lo, x->a, x->b, x->c, x->d);
    for (unsigned i=0; i<64; i++) fprintf (stderr, "%2.2x", x->buffer.bytes[i]);
    fprintf (stderr, "\n");

    iteration++;
}

static const void *md5_transform (Md5StateP state, const void *data, uintmax_t size)
{
    const uint32_t *ptr = (uint32_t *)data;

    uint32_t a = state->a;
    uint32_t b = state->b;
    uint32_t c = state->c;
    uint32_t d = state->d;

    do {
        uint32_t saved_a = a;
        uint32_t saved_b = b;
        uint32_t saved_c = c;
        uint32_t saved_d = d;

#ifdef __LITTLE_ENDIAN__
        // in little endian machines, we can access the data directly - we don't need to copy memory
        #define block ptr
#else
        // in big endian machines - we need to flip the data to little endian - so we do it in a copy
        uint32_t block[16]; 
        for (unsigned i=0; i < 16; i++) block[i] = LTEN32(ptr[i]);
#endif
        // Round 1
        STEP( F, a, b, c, d, block[0],  0xd76aa478, 7  )
        STEP( F, d, a, b, c, block[1],  0xe8c7b756, 12 )
        STEP( F, c, d, a, b, block[2],  0x242070db, 17 )
        STEP( F, b, c, d, a, block[3],  0xc1bdceee, 22 )
        STEP( F, a, b, c, d, block[4],  0xf57c0faf, 7  )
        STEP( F, d, a, b, c, block[5],  0x4787c62a, 12 )
        STEP( F, c, d, a, b, block[6],  0xa8304613, 17 )
        STEP( F, b, c, d, a, block[7],  0xfd469501, 22 )
        STEP( F, a, b, c, d, block[8],  0x698098d8, 7  )
        STEP( F, d, a, b, c, block[9],  0x8b44f7af, 12 )
        STEP( F, c, d, a, b, block[10], 0xffff5bb1, 17 )
        STEP( F, b, c, d, a, block[11], 0x895cd7be, 22 )
        STEP( F, a, b, c, d, block[12], 0x6b901122, 7  )
        STEP( F, d, a, b, c, block[13], 0xfd987193, 12 )
        STEP( F, c, d, a, b, block[14], 0xa679438e, 17 )
        STEP( F, b, c, d, a, block[15], 0x49b40821, 22 )

        // Round 2
        STEP( G, a, b, c, d, block[1],  0xf61e2562, 5  )
        STEP( G, d, a, b, c, block[6],  0xc040b340, 9  )
        STEP( G, c, d, a, b, block[11], 0x265e5a51, 14 )
        STEP( G, b, c, d, a, block[0],  0xe9b6c7aa, 20 )
        STEP( G, a, b, c, d, block[5],  0xd62f105d, 5  )
        STEP( G, d, a, b, c, block[10], 0x02441453, 9  )
        STEP( G, c, d, a, b, block[15], 0xd8a1e681, 14 )
        STEP( G, b, c, d, a, block[4],  0xe7d3fbc8, 20 )
        STEP( G, a, b, c, d, block[9],  0x21e1cde6, 5  )
        STEP( G, d, a, b, c, block[14], 0xc33707d6, 9  )
        STEP( G, c, d, a, b, block[3],  0xf4d50d87, 14 )
        STEP( G, b, c, d, a, block[8],  0x455a14ed, 20 )
        STEP( G, a, b, c, d, block[13], 0xa9e3e905, 5  )
        STEP( G, d, a, b, c, block[2],  0xfcefa3f8, 9  )
        STEP( G, c, d, a, b, block[7],  0x676f02d9, 14 )
        STEP( G, b, c, d, a, block[12], 0x8d2a4c8a, 20 )

        // Round 3
        STEP( H, a, b, c, d, block[5],  0xfffa3942, 4  )
        STEP( H, d, a, b, c, block[8],  0x8771f681, 11 )
        STEP( H, c, d, a, b, block[11], 0x6d9d6122, 16 )
        STEP( H, b, c, d, a, block[14], 0xfde5380c, 23 )
        STEP( H, a, b, c, d, block[1],  0xa4beea44, 4  )
        STEP( H, d, a, b, c, block[4],  0x4bdecfa9, 11 )
        STEP( H, c, d, a, b, block[7],  0xf6bb4b60, 16 )
        STEP( H, b, c, d, a, block[10], 0xbebfbc70, 23 )
        STEP( H, a, b, c, d, block[13], 0x289b7ec6, 4  )
        STEP( H, d, a, b, c, block[0],  0xeaa127fa, 11 )
        STEP( H, c, d, a, b, block[3],  0xd4ef3085, 16 )
        STEP( H, b, c, d, a, block[6],  0x04881d05, 23 )
        STEP( H, a, b, c, d, block[9],  0xd9d4d039, 4  )
        STEP( H, d, a, b, c, block[12], 0xe6db99e5, 11 )
        STEP( H, c, d, a, b, block[15], 0x1fa27cf8, 16 )
        STEP( H, b, c, d, a, block[2],  0xc4ac5665, 23 )

        // Round 4
        STEP( I, a, b, c, d, block[0],  0xf4292244, 6  )
        STEP( I, d, a, b, c, block[7],  0x432aff97, 10 )
        STEP( I, c, d, a, b, block[14], 0xab9423a7, 15 )
        STEP( I, b, c, d, a, block[5],  0xfc93a039, 21 )
        STEP( I, a, b, c, d, block[12], 0x655b59c3, 6  )
        STEP( I, d, a, b, c, block[3],  0x8f0ccc92, 10 )
        STEP( I, c, d, a, b, block[10], 0xffeff47d, 15 )
        STEP( I, b, c, d, a, block[1],  0x85845dd1, 21 )
        STEP( I, a, b, c, d, block[8],  0x6fa87e4f, 6  )
        STEP( I, d, a, b, c, block[15], 0xfe2ce6e0, 10 )
        STEP( I, c, d, a, b, block[6],  0xa3014314, 15 )
        STEP( I, b, c, d, a, block[13], 0x4e0811a1, 21 )
        STEP( I, a, b, c, d, block[4],  0xf7537e82, 6  )
        STEP( I, d, a, b, c, block[11], 0xbd3af235, 10 )
        STEP( I, c, d, a, b, block[2],  0x2ad7d2bb, 15 )
        STEP( I, b, c, d, a, block[9],  0xeb86d391, 21 )

        a += saved_a;   
        b += saved_b;
        c += saved_c;
        d += saved_d;

        ptr += 16;
    } while (size -= 64);

    state->a = a;
    state->b = b;
    state->c = c;
    state->d = d;

    return ptr;
}

void md5_initialize (Md5StateP state)
{
#ifdef DEBUG // note: ASSERT not supported when compiled from script 
    ASSERT0 (is_zero_struct (*state), "md5_initialize expects state to be zeros, but its not");
#endif

    state->a = 0x67452301;
    state->b = 0xefcdab89;
    state->c = 0x98badcfe;
    state->d = 0x10325476;

    state->lo = 0;
    state->hi = 0;

    state->initialized = true;
}

// data must be aligned on 32-bit boundary
void md5_update (Md5StateP state, const void *data, uint32_t len)
{
    if (!len) return; // nothing to do

    uint32_t    saved_lo;
    uint32_t    used;
    uint32_t    free;

    saved_lo = state->lo;
    if ((state->lo = (saved_lo + len) & 0x1fffffff) < saved_lo) 
        state->hi++;
    
    state->hi += (uint32_t)(len >> 29);

    used = saved_lo & 0x3f;

    if (used) {
        free = 64 - used;

        if (len < free) {
            memcpy (&state->buffer.bytes[used], data, len);
            goto finish;
        }

        memcpy (&state->buffer.bytes[used], data, free);
        data += free;
        len -= free;
        md5_transform (state, state->buffer.bytes, 64);
    }

    if (len >= 64) {
        data = md5_transform (state, data, len & ~(unsigned long)0x3f);
        len &= 0x3f;
    }

    memcpy (state->buffer.bytes, data, len);

finish:
    //fprintf (stderr, "%s md5_update snapshot: %s\n", primary_command == ZIP ? "ZIP" : "PIZ", digest_display (digest_snapshot (state)));
    //md5_display_state (state);
    return;
}

Digest md5_finalize (Md5StateP state)
{
    uint32_t    used;
    uint32_t    free;

    used = state->lo & 0x3f;

    state->buffer.bytes[used++] = 0x80;

    free = 64 - used;

    if (free < 8) {
        memset (&state->buffer.bytes[used], 0, free);
        md5_transform (state, state->buffer.bytes, 64);
        used = 0;
        free = 64;
    }

    memset (&state->buffer.bytes[used], 0, free - 8);

    state->lo <<= 3;
    state->buffer.words[14] = LTEN32 (state->lo);
    state->buffer.words[15] = LTEN32 (state->hi);

    md5_transform (state, state->buffer.bytes, 64);
    Digest digest = { .words = { LTEN32 (state->a), LTEN32 (state->b), LTEN32 (state->c), LTEN32 (state->d) } };

    memset (state, 0, sizeof (Md5State)); // return to its pre-initialized state, should it be used again

    return digest;
}

// note: data must be aligned to the 32bit boundary (its accessed as uint32_t*)
Digest md5_do (const void *data, uint32_t len)
{
    Md5State state;
    memset (&state, 0, sizeof(Md5State));

    md5_initialize (&state);
    
    md5_update (&state, data, len);

    return md5_finalize (&state);
}

Digest md5_read (const char str[32])
{
    Digest out;

    for (int i=0; i < 16; i++)
        out.bytes[i] = (HEXDIGIT2NUM(str[i*2]) << 4) | HEXDIGIT2NUM(str[i*2+1]);

    return out;
}
