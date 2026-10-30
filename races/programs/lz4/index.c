/* program.c with u16 indexes into the buffers instead of pointers */
#include "race.h"

static uint16_t src;

static uint8_t read_byte(void) {
    return packed[src++];
}

static uint16_t extend_length(uint16_t len) {
    uint8_t b;
    if (len != 15) return len;
    do {
        b = read_byte();
        len += b;
    } while (b == 255);
    return len;
}

static uint16_t lz4_decode(uint16_t src_len) {
    uint16_t o = 0, len, offset, m;
    uint8_t token;
    src = 0;
    for (;;) {
        token = read_byte();
        len = extend_length(token >> 4);
        while (len != 0) {
            out[o++] = packed[src++];
            len--;
        }
        if (src >= src_len) break;
        offset = read_byte();
        offset |= (uint16_t)read_byte() << 8;
        m = o - offset;
        len = extend_length(token & 15) + 4;
        while (len != 0) {
            out[o++] = out[m++];
            len--;
        }
    }
    return o;
}

int main(void) {
    uint16_t n, i, sum_a = 0, sum_b = 0;
    n = lz4_decode(packed_len);
    for (i = 0; i < n; i++) {
        sum_a += out[i];
        sum_b += sum_a;
    }
    checksum = sum_a ^ sum_b ^ n;
    return 0;
}
