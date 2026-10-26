/* move 64 sprites for 16 frames, bouncing off the edges; an array of structs
 * (struct sprite is in buffers.h: x, y, dx, dy, frame) */
#include "race.h"

int main(void) {
    uint8_t f, i;
    for (f = 0; f < 16; f++) {
        for (i = 0; i < 64; i++) {
            struct sprite *s = &sprites[i];
            s->x += s->dx;
            if (s->x < 8 || s->x >= 240) {
                s->dx = -s->dx;
            }
            s->y += s->dy;
            if (s->y < 8 || s->y >= 184) {
                s->dy = -s->dy;
            }
            s->frame = (s->frame + 1) & 7;
        }
    }
    return 0;
}
