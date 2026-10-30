/* A side-scroller's renderer: a 128 x 24 map of 8 x 8 tiles drawn into a 256 x 192
 * bitmap (32 bytes a row) at a pixel scroll, every frame in full: each screen byte is the
 * left tile's line shifted left by the fine scroll and the right tile's shifted right.
 * Animated tiles (water) roll their lines with the frame. A player walks at a fixed
 * place on the screen, falling, climbing and taking coins from the map. 8 frames,
 * 3 pixels a frame; the checksum folds the screen, the player's row and the score. */
#include "race.h"

#define MAP_W 128
#define ROW 32
#define FRAMES 8
#define SPEED 3
#define SOLID 1
#define ANIMATED 2
#define COIN 13

static const uint8_t TILES[16 * 8] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0x81, 0x81, 0x81, 0xFF, 0x18, 0x18, 0x18,
    0xFF, 0x18, 0x18, 0x18, 0xFF, 0x81, 0x81, 0x81,
    0xFF, 0x81, 0x81, 0x81, 0xFF, 0x18, 0x18, 0x18,
    0xFF, 0x18, 0x18, 0x18, 0xFF, 0x81, 0x81, 0x81,
    0xBC, 0xCE, 0xDB, 0xF2, 0xB5, 0x50, 0xBB, 0x86,
    0xD8, 0xE5, 0xEC, 0xCF, 0x6A, 0xD5, 0xA0, 0x2B,
    0x04, 0x00, 0x05, 0x1C, 0x5F, 0xDA, 0x85, 0xD0,
    0x20, 0x23, 0x1E, 0x09, 0xD4, 0x5F, 0x6A, 0x75,
    0x44, 0x4A, 0x47, 0x26, 0xE9, 0xA4, 0xCF, 0x1A,
    0x78, 0x69, 0xA0, 0xC3, 0x3E, 0xE9, 0x34, 0xBF,
    0x9C, 0xB4, 0x81, 0xE0, 0x73, 0xAE, 0x19, 0x64,
    0xB0, 0xDF, 0xE2, 0xBD, 0x28, 0x73, 0xFE, 0x09,
    0x00, 0x3C, 0x7E, 0x66, 0x66, 0x7E, 0x3C, 0x00,
    0x08, 0x0D, 0x24, 0x57, 0xD2, 0xFD, 0xC8, 0x53,
    0x24, 0x28, 0x7D, 0x34, 0xA7, 0x82, 0xAD, 0xF8,
};
static const uint8_t ATTR[16] = {0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 2, 2};

static uint8_t player_row, score;

/* line of tile t, rolled by the frame when it is animated */
static uint8_t tile_line(uint8_t t, uint8_t line, uint8_t frame) {
    if (ATTR[t] & ANIMATED) line = (line + frame) & 7;
    return TILES[t * 8 + line];
}

static void draw(uint16_t sx, uint8_t frame) {
    uint8_t fine = sx & 7;
    uint8_t back = 8 - fine;
    uint8_t r, c, line, left, right;
    const uint8_t *tiles;
    uint8_t *p;
    for (r = 0; r < 24; r++) {
        tiles = map + r * MAP_W + (sx >> 3);
        for (c = 0; c < 32; c++) {
            left = tiles[c];
            right = tiles[c + 1];
            p = screen + r * (8 * ROW) + c;
            for (line = 0; line < 8; line++) {
                *p = (uint8_t)(tile_line(left, line, frame) << fine) |
                     (tile_line(right, line, frame) >> back);
                p += ROW;
            }
        }
    }
}

static void move_player(uint16_t sx) {
    uint8_t col = (sx + 100) >> 3;
    uint8_t *at = map + player_row * MAP_W + col;
    if (!(ATTR[at[MAP_W]] & SOLID) && player_row < 22) player_row++;
    else if ((ATTR[at[1]] & SOLID) && player_row > 1) player_row--;
    at = map + player_row * MAP_W + col;
    if (*at == COIN) {
        *at = 0;
        score++;
    }
}

int main(void) {
    uint8_t frame;
    uint16_t sx, n, sum_a = 0, sum_b = 0;
    player_row = 2;
    score = 0;
    for (frame = 0; frame < FRAMES; frame++) {
        sx = frame * SPEED;
        draw(sx, frame);
        move_player(sx);
    }
    for (n = 0; n < ROW * 192; n++) {
        sum_a += screen[n];
        sum_b += sum_a;
    }
    sum_a += player_row;
    sum_b += sum_a;
    sum_a += score;
    sum_b += sum_a;
    checksum = sum_a ^ sum_b;
    return 0;
}
