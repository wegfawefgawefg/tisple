/* 24 lines on hi-res page 1: lib/hgr.tsp's Bresenham and table plot in C. hgr_tables.h
 * (written by race.py) has the same tables lib/hgr makes: row addresses as low and high
 * byte tables, and the byte and bit of every column. */
#include "race.h"
#include "hgr_tables.h"

static void plot(uint16_t x, uint8_t y) {
    uint8_t *p = (uint8_t *)(ROW_LO[y] | ((uint16_t)ROW_HI[y] << 8)) + COL_BYTE[x];
    *p |= COL_BIT[x];
}

/* every pixel from x0, y0 to x1, y1 on, both ends included */
static void line(uint16_t x0, uint8_t y0, uint16_t x1, uint8_t y1) {
    int16_t x = (int16_t)x0;
    int16_t y = (int16_t)y0;
    int16_t x_end = (int16_t)x1;
    int16_t y_end = (int16_t)y1;
    int16_t dx = x_end - x;
    int16_t step_x = 1;
    int16_t dy, step_y, err;
    if (dx < 0) {
        dx = -dx;
        step_x = -1;
    }
    dy = y - y_end;
    step_y = 1;
    if (dy > 0) {
        dy = -dy;
        step_y = -1;
    }
    /* dy is minus the height; err tracks how far the drawn line is off the real one */
    err = dx + dy;
    for (;;) {
        int16_t twice;
        plot((uint16_t)x, (uint8_t)y);
        if (x == x_end && y == y_end) {
            break;
        }
        twice = err << 1;
        if (twice >= dy) {
            err += dy;
            x += step_x;
        }
        if (twice <= dx) {
            err += dx;
            y += step_y;
        }
    }
}

int main(void) {
    uint8_t i;
    for (i = 0; i < 24; i++) {
        line(x0s[i], y0s[i], x1s[i], y1s[i]);
    }
    return 0;
}
