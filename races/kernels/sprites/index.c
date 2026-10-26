/* move 64 sprites for 16 frames, bouncing off the edges; an array of structs indexed
 * directly, the form Oscar64's __striped arrays need (no pointer to an element) */
#include "race.h"

int main(void) {
    uint8_t f, i;
    for (f = 0; f < 16; f++) {
        for (i = 0; i < 64; i++) {
            sprites[i].x += sprites[i].dx;
            if (sprites[i].x < 8 || sprites[i].x >= 240) {
                sprites[i].dx = -sprites[i].dx;
            }
            sprites[i].y += sprites[i].dy;
            if (sprites[i].y < 8 || sprites[i].y >= 184) {
                sprites[i].dy = -sprites[i].dy;
            }
            sprites[i].frame = (sprites[i].frame + 1) & 7;
        }
    }
    return 0;
}
