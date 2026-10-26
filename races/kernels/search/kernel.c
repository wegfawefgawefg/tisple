/* naive search for the 6-byte needle in 4 KB of text; result is the first index */
#include "race.h"

#define TEXT_LEN 4096
#define NEEDLE_LEN 6

int main(void) {
    uint16_t i;
    for (i = 0; i < TEXT_LEN - NEEDLE_LEN + 1; i++) {
        const uint8_t *p = text + i;
        uint8_t j = 0;
        while (j < NEEDLE_LEN && p[j] == needle[j]) {
            j++;
        }
        if (j == NEEDLE_LEN) {
            result = i;
            return 0;
        }
    }
    result = 0xFFFF;
    return 0;
}
