/* 16-bit multiply: the sum of x[i] * y[i] over 128 word pairs, wrapping at 16 bits */
#include "race.h"

int main(void) {
    uint16_t sum = 0;
    uint8_t i;
    for (i = 0; i < 128; i++) {
        sum += x[i] * y[i];
    }
    result = sum;
    return 0;
}
