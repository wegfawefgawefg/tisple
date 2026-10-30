/* BYTE's sieve (Jim Gilbreath, BYTE, September 1981) as published: int variables, 10
 * passes over 8191 flags, counting 1899 primes. flags is the race's buffer (buffers.h)
 * instead of a global char array, and the count goes to checksum instead of printf. */
#include "race.h"

#define TRUE 1
#define FALSE 0
#define SIZE 8190

int main(void) {
    int i, prime, k, count, iter;
    for (iter = 1; iter <= 10; iter++) {
        count = 0;
        for (i = 0; i <= SIZE; i++)
            flags[i] = TRUE;
        for (i = 0; i <= SIZE; i++) {
            if (flags[i]) {
                prime = i + i + 3;
                k = i + prime;
                while (k <= SIZE) {
                    flags[k] = FALSE;
                    k += prime;
                }
                count = count + 1;
            }
        }
    }
    checksum = count;
    return 0;
}
