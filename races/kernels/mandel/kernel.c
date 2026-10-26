/* Mandelbrot, 32 x 24 pixels, up to 15 iterations, 16-bit fixed point with 6 fraction
 * bits. A point escapes when |x| or |y| reaches 2 or x^2 + y^2 reaches 4, tested before
 * the squares can overflow 16 bits. */
#include "race.h"

int main(void) {
    int16_t cy = y0;
    uint16_t k = 0;
    uint8_t px, py, n;
    for (py = 0; py < 24; py++) {
        int16_t cx = x0;
        for (px = 0; px < 32; px++) {
            int16_t zx = 0;
            int16_t zy = 0;
            for (n = 0; n < 15; n++) {
                int16_t zx2, zy2;
                if (zx >= 128 || zx <= -128 || zy >= 128 || zy <= -128) {
                    break;
                }
                zx2 = (zx * zx) >> 6;
                zy2 = (zy * zy) >> 6;
                if (zx2 + zy2 >= 256) {
                    break;
                }
                zy = ((zx * zy) >> 5) + cy;
                zx = zx2 - zy2 + cx;
            }
            iters[k] = n;
            k++;
            cx += step;
        }
        cy += step;
    }
    return 0;
}
