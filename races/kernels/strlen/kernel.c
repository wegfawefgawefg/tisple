/* length of a zero-terminated string (3000 bytes) */
#include "race.h"

int main(void) {
    uint16_t n = 0;
    while (str[n] != 0) {
        n++;
    }
    result = n;
    return 0;
}
