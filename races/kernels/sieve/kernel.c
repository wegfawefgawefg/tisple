/* sieve of Eratosthenes over 8192 flags; result is the number of primes below 8192 */
#include "race.h"

int main(void) {
    uint16_t i, k;
    uint16_t count = 0;
    for (i = 0; i < 8192; i++) {
        flags[i] = 1;
    }
    for (i = 2; i < 8192; i++) {
        if (flags[i]) {
            count++;
            for (k = i + i; k < 8192; k += i) {
                flags[k] = 0;
            }
        }
    }
    result = count;
    return 0;
}
