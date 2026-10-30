/* LZ4 block decompression (the format of LZ4's block API: a token of literal and match
 * lengths, 255-byte length extensions, little-endian offsets) of packed into out, 8 KB of
 * text and a tile map, as a game unpacks a level; then a checksum of what came out.
 * Pointers walk both buffers, as C decoders are written. */
#include "race.h"

static uint16_t lz4_decode(const uint8_t *src, uint16_t src_len, uint8_t *dst) {
    const uint8_t *end = src + src_len;
    const uint8_t *match;
    uint8_t *op = dst;
    uint8_t token, b;
    uint16_t len, offset;
    for (;;) {
        token = *src++;
        len = token >> 4;
        if (len == 15) {
            do {
                b = *src++;
                len += b;
            } while (b == 255);
        }
        while (len != 0) {
            *op++ = *src++;
            len--;
        }
        if (src >= end) break;
        offset = src[0] | ((uint16_t)src[1] << 8);
        src += 2;
        match = op - offset;
        len = token & 15;
        if (len == 15) {
            do {
                b = *src++;
                len += b;
            } while (b == 255);
        }
        len += 4;
        while (len != 0) {
            *op++ = *match++;
            len--;
        }
    }
    return (uint16_t)(op - dst);
}

int main(void) {
    uint16_t n, i, sum_a = 0, sum_b = 0;
    n = lz4_decode(packed, packed_len, out);
    for (i = 0; i < n; i++) {
        sum_a += out[i];
        sum_b += sum_a;
    }
    checksum = sum_a ^ sum_b ^ n;
    return 0;
}
