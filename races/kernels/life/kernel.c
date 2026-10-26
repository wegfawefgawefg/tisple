/* one step of Life on 40 x 48 cells with a dead border, as programs/life: rows of 42
 * cells, cur to next */
#include "race.h"

#define STRIDE 42
#define LIVE 1
#define DEAD 0

static uint8_t *row_of(uint8_t *world, uint8_t y) {
    return world + (uint16_t)y * STRIDE;
}

int main(void) {
    uint8_t y, x;
    for (y = 1; y < 49; y++) {
        const uint8_t *above = row_of(cur, y - 1);
        const uint8_t *here = row_of(cur, y);
        const uint8_t *below = row_of(cur, y + 1);
        uint8_t *out = row_of(next, y);
        for (x = 1; x < 41; x++) {
            uint8_t n = above[x - 1] + above[x] + above[x + 1] + here[x - 1] + here[x + 1] +
                        below[x - 1] + below[x] + below[x + 1];
            uint8_t alive = here[x];
            out[x] = (n == 3 || (n == 2 && alive == LIVE)) ? LIVE : DEAD;
        }
    }
    return 0;
}
