/* BYTE's sieve as a 6502 C programmer tunes it: unsigned 16-bit variables, an 8-bit
 * pass counter, register hints; the same algorithm and passes as program.c. */
#include "race.h"

#define SIZE 8190

int main(void) {
    register uint16_t i, k;
    uint16_t prime, count;
    uint8_t iter;
    for (iter = 0; iter < 10; iter++) {
        count = 0;
        for (i = 0; i <= SIZE; i++)
            flags[i] = 1;
        for (i = 0; i <= SIZE; i++) {
            if (flags[i]) {
                prime = i + i + 3;
                for (k = i + prime; k <= SIZE; k += prime)
                    flags[k] = 0;
                count++;
            }
        }
    }
    checksum = count;
    return 0;
}
