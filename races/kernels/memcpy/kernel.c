/* copy 4 KB */
#include "race.h"

int main(void) {
    uint16_t i;
    for (i = 0; i < 4096; i++) {
        dst[i] = src[i];
    }
    return 0;
}
