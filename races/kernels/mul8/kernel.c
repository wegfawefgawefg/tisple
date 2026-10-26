/* 8-bit multiply: the sum of a[i] * b[i] over 256 byte pairs, in 16 bits */
#include "race.h"

int main(void) {
    uint16_t sum = 0;
    uint8_t i = 0;
    do {
        sum += (uint16_t)(a[i]) * b[i];
    } while (++i != 0);
    result = sum;
    return 0;
}
