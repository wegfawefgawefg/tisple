/* fill 8 KB with a byte */
#include "race.h"

int main(void) {
    uint8_t v = value;
    uint16_t i;
    for (i = 0; i < 8192; i++) {
        buf[i] = v;
    }
    return 0;
}
