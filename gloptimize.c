// ------------------------------------------------------------------
//   gl-optimize.c
//   Copyright (C) 2019 Divon Lan <vczip@blackpawventures.com>
//   Please see terms and conditions in the files LICENSE.non-commercial.txt and LICENSE.commercial.txt

// Optimization - for GL (Genotype Likelihood) subfield - this is a standard subfield defined
// in the VCF spec - it consists of floating point comma-seperated numbers - representing
// the log(probability) of allele combinations (for example, 00, 01, 11 in case of diploid with one alt allele)
// it looks something like this: -0.16,-0.52,-3.22  . note that 10^(-0.16) + 10^(-0.52) + 10^(-3.22) = 1.
//
// In this optimization, we do simply replace the highest number (i.e. -0.16 in the example above) with
// a string of '0' the same length. The number can be recovered from the formula above. After replacing, 
// we test that the number indeed recovers to the original number, which it sometimes doesn't due to
// rounding errors in the data. We handle the most common error of an error of 1 on the least significant
// digit, which we encode as 010000 (of the correct length). With this, we capture 98% of the cases. 
// We skip optimizing a GL field in which not all numbers are of the format -[0-9].[0-9]+ (negative, one 
// integer digit, up to 9 digits after the decimal point)

#include <math.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include "vczip.h"

#define MAX_GL_LEN 12 /* we support numbers as long as -0.1234567890 but not longer */

// look at a GL field that has been optimized, and return the value missing, as an
// unsigned int with the correct number of digits. For example, -4.352 will return as 4352
// return -1 if this is not an optimized gl subfield
static inline int gl_optimize_get_missing_gl_int(char *data,
                                                 char **gl_start, unsigned *gl_len) // out
{
    static const unsigned POW10[MAX_GL_LEN-2] = {1, 10, 100, 1000, 10000, 100000, 1000000, 
                                                 10000000, 100000000, 1000000000};
    *gl_len = 0;
    double probability_sum = 0;
    int rounding_error_correction = 0;
    
    do {    
        char *start = data;

        // this can be an optimized value, or part of a completely not optimized GL. 
        // In an optimized GL, other fields are guaranteed to start with a minus sign.
        if (*data == '0') { 

            if (data[1] == '1') {
                rounding_error_correction = 1;
                data += 2;
                (*gl_len) += 2;
            }

            while (*(data++) == '0') (*gl_len)++;

            if (*gl_len < 4) 
                return -1; // possibly a 0.000 (non-negative 0) - this is an unoptimized gl
            
            ASSERT (*gl_len <= MAX_GL_LEN, "Optimized gl subfield is %u characters exceeding allowed maximum of %u", *gl_len, MAX_GL_LEN);

            if (data[-1] != ',' && data[-1] != ':' && data[-1] != '\t') return -1; // not an optimized GL - series of 0s should have been termianted by a , or : or \t
            *gl_start = start;

            continue;
        }

        // get the floating point number - it is expected to be -*.**** (a minus sign, one integer, decimal) if this GL has optimization
        if (data[0] != '-' || data[2] != '.') return -1;

        double gl = 0 - (data[1] - '0'); // nagative integer part
        data += 3;

        double divisor = 10.0;
        while (*data != ':' && *data != ',' && *data != '\t') {
            gl -= (*(data++) - '0') / divisor; // -= because we are growing a negative number
            divisor *= 10;
        }

        probability_sum += pow (10, gl);

        data++; // skip separator

    } while (data[-1] != ':' && data[-1] != '\t');

    if (! *gl_len) return -1; // there is no optimed value here

    // in extreme cases, and due to rounding errors in the data, probability_sum can be slightly more than 1
    // this will cause the log10 to be NaN. We therefore limit it to 0.999999999 which will
    // result in missing_gl=9 - the highest possible value, since we only allow single digit integers
    double missing_gl = -log10 (1 - MIN (probability_sum, 0.999999999)); // turn it into a positive value
    if (missing_gl > 9) missing_gl = 9; // at most 9 - i.e. a probabiliy of 0.000000001 - highly unlikely value for the item that's suppose to be the highest probability

    // round to the number of digits available
    unsigned missing_gl_int = (unsigned)round(missing_gl * POW10[(*gl_len)-3]) + rounding_error_correction;

    return missing_gl_int;
}

// moves data pointer to the beginning of the GL subfield with the a line data column.
// returns false if the GL field is missing from this sample
static inline bool gl_optimize_seek_gl_subfield(char **data, unsigned gl_subfield_no_gt)
{
    for (unsigned colon_i=1; colon_i < gl_subfield_no_gt; (*data)++) {
        if (**data == '\t') return false; // this subfield is missing (and all following subfields too) - this is allowed by VCF spec
        if (**data == ':') colon_i++;
    }

    if (**data == ':' || **data == '\t') return false; // :: or :\t - this subfield is missing, and next subfield is about to begin - this is allowed by VCF spec

    return true;
}

// returns length of gt after optimization, or -1 if optimization failed
void gl_optimize_do(VariantBlock *vb, char *data, unsigned data_len, unsigned gl_subfield_no_gt) 
{
    START_TIMER; // note: the timer itself add a lot of time to this function

    if (!gl_optimize_seek_gl_subfield (&data, gl_subfield_no_gt)) goto cleanup; // no GL field

    char *start_gl_subfield = data;

#   define MAX_NUM_GL_VALUES 32 /* for normal one-alt diploids only 3 are used */
    double best_gl=10000000 /* initialize high value */;
    char *best_gl_start=NULL;
    unsigned best_gl_len=0;

    unsigned gl_i; for (gl_i=0; gl_i < MAX_NUM_GL_VALUES; gl_i++) {

        char *gl_start = data;

        // we expect all numbers to be -*.**** : mandatory -, one integer, mandatory . and a variable number of decimals
        if (data[0] != '-' || data[2] != '.') goto cleanup;

        double gl = data[1] - '0'; // integer part
        data += 3;
   
        double divisor = 10.0;
        while (*data != ':' && *data != ',' && *data != '\t') {
            gl += (*(data++) - '0') / divisor;
            divisor *= 10;
        }

        if (gl < best_gl) { // current gl is the new best
            best_gl = gl;
            best_gl_start = gl_start;
            best_gl_len = data - gl_start;
        }

        if (*data == ':' || *data == '\t') break;

        data++; // skip the seperator
    }

    if (data[-1] == ',') goto cleanup; // more than MAX_NUM_GL_VALUES - sorry, can't optimize this

    if (*(best_gl_start+1) == '.') goto cleanup; // we don't currently support the numeric format "-.001" (no integer) - we can in the future, if there's a need

    if (best_gl_len > MAX_GL_LEN) goto cleanup; // we can't optimize a number with so many digits

    // save, in case we need to reverse if it fails QA
    char saved_best_gl[MAX_GL_LEN];
    memcpy (saved_best_gl, best_gl_start, best_gl_len);

    // get integer representation
    int best_gl_int = best_gl_start[1] - '0'; // integer part
    for (unsigned i=3; i < best_gl_len; i++)
        best_gl_int = (best_gl_int*10) + best_gl_start[i] - '0';

    // now to the action - zero this gl
    memset (best_gl_start, '0', best_gl_len);

    // test to see if we can recover the original value correctly - sometimes there are fp rounding errors
    // get the missing data and its location and length
    char *gl_start;
    unsigned gl_len;
    int recovered_gl_int = gl_optimize_get_missing_gl_int(start_gl_subfield, &gl_start, &gl_len);

    // about 35% fail due to a rounding error, of these 99% are when the recovered int is too low by 1
    if (best_gl_int != recovered_gl_int) {
        if (best_gl_int - recovered_gl_int == 1) 
            best_gl_start[1] = '1';  // set it to 010000000 indicating a rounding error of 1 should be corrected

        else // don't optimize - return to saved values
            memcpy (best_gl_start, saved_best_gl, best_gl_len);
    }

cleanup:
    COPY_TIMER(vb->profile.gl_optimize_do);
}

void gl_optimize_undo (VariantBlock *vb, char *data, unsigned len, unsigned gl_subfield_no_gt)
{
    START_TIMER; // note: the timer itself add a lot of time to this function

    // find the correct subfield
    if (!gl_optimize_seek_gl_subfield (&data, gl_subfield_no_gt)) goto cleanup; // no GL field

    // get the missing data and its location and length
    char *gl_start;
    unsigned gl_len;
    int missing_gl_int = gl_optimize_get_missing_gl_int(data, &gl_start, &gl_len);
    if (missing_gl_int < 0) goto cleanup; // not an optimized GL subfield

    // output "manually" - don't call sprintf - much faster - output number from right to left
    for (unsigned i=gl_len-1; i >= 3; i--) {
        gl_start[i] = missing_gl_int % 10 + '0';
        missing_gl_int /= 10;
    }

    gl_start[2] = '.';
    gl_start[1] = missing_gl_int + '0';
    gl_start[0] = '-';

cleanup:
    COPY_TIMER (vb->profile.gl_optimize_undo);
}

// look at FORMAT column string, to figure out the index of the gl_subfield in this line
// index starts from 1. index=0 indicates the subfield is missing from FORMAT
int gl_optimize_get_gl_subfield_index(const char *data)
{
    const char *start_str = data;
    do {
        if (data[0]=='G' && data[1]=='L')
            return 1 + (data - start_str) / 3;

        data += 3;
    } 
    while (data[-1] != '\t' && data[-1] != '\n');

    return 0; // no GL
}

 