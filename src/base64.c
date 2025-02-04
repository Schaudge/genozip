// ------------------------------------------------------------------
//   base64.c
//
// derived from: https://github.com/launchdarkly/c-client-sdk/blob/master/base64.c
//
// Original unmodified license statement:
//  * Base64 encoding/decoding (RFC1341)
//  * Copyright (c) 2005-2011, Jouni Malinen <j@w1.fi>
//  *
//  * This software may be distributed under the terms of the BSD license.
//

#include "base64.h"

static bytes encode_lookup =
    (bytes)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const uint8_t decode_lookup[256] = {
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, // ASCII 0-15
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, // ASCII 16-31
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 62  , 0x80, 0x80, 0x80, 63  , // ASCII 32-47
	52  , 53  , 54  , 55  , 56  , 57  , 58  , 59  , 60  , 61  , 0x80, 0x80, 0x80, 0   , 0x80, 0x80, // ASCII 48-63 (outside of encode_lookup '=' = 0) 
    0x80, 0   , 1   , 2   , 3   , 4   , 5   , 6   , 7   , 8   , 9   , 10  , 11  , 12  , 13  , 14  , // ASCII 64-79
	15  , 16  , 17  , 18  , 19  , 20  , 21  , 22  , 23  , 24  , 25  , 0x80, 0x80, 0x80, 0x80, 0x80, // ASCII 80-95
	0x80, 26  , 27  , 28  , 29  , 30  , 31  , 32  , 33  , 34  , 35  , 36  , 37  , 38  , 39  , 40  , // ASCII 96-111
	41  , 42  , 43  , 44  , 45  , 46  , 47  , 48  , 49  , 50  , 51  , 0x80, 0x80, 0x80, 0x80, 0x80, // ASCII 112-127
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, // ASCII 128-255
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 
	0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80 };

// returns length of encoded (which is at most base64_sizeof)
// data must be allocated base64_sizeof bytes
unsigned base64_encode (bytes data, unsigned data_len, char *b64_str)
{
    ASSERTNOTNULL (data);

	bytes end = data + data_len;
	char *next = b64_str;
	while (end - data >= 3) {
		*next++ = encode_lookup[data[0] >> 2];
		*next++ = encode_lookup[((data[0] & 0x03) << 4) | (data[1] >> 4)];
		*next++ = encode_lookup[((data[1] & 0x0f) << 2) | (data[2] >> 6)];
		*next++ = encode_lookup[  data[2] & 0x3f];
		data += 3;
	}

	if (end - data) {
		*next++ = encode_lookup[data[0] >> 2];
		if (end - data == 1) {
			*next++ = encode_lookup[(data[0] & 0x03) << 4];
			*next++ = '=';
		} else {
			*next++ = encode_lookup[((data[0] & 0x03) << 4) | (data[1] >> 4)];
			*next++ = encode_lookup[ (data[1] & 0x0f) << 2];
		}
		*next++ = '=';
	}

    return next - b64_str;
}

// returns length of decoded data. b64_str_len is updated to the length of b64 string that was consumed.
uint32_t base64_decode (rom b64_str, unsigned *b64_str_len /* in / out */, uint8_t *data,
                        uint32_t data_len) // -1 if asking to read to end of snip (a non-b64 char like \0 or \t will terminate the b64 string) 
{
	uint8_t block[4];
	uint32_t len=0;

	unsigned pad=0, i=0; for (; i < *b64_str_len && len < data_len; i++) {
		if (b64_str[i] == '=') pad++;
		block[i&3] = decode_lookup[(unsigned)b64_str[i]];
		
		if (block[i&3] == 0x80) break; // end of b64 string

		if ((i&3) == 3) {
  			                 { *data++ = (block[0] << 2) | (block[1] >> 4); len++; }
			if (pad <= 1)    { *data++ = (block[1] << 4) | (block[2] >> 2); len++; }
			if (!pad)        { *data++ = (block[2] << 6) |  block[3];       len++; }
		}
	}	

	*b64_str_len = i;

	return len;
}