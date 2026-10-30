"""A tile map scroller: a 128 x 24 map of 8 x 8 tiles drawn into a 256 x 192 bitmap at a
fine (pixel) scroll, animated water, a player falling, climbing and taking coins"""

from kernels import Kernel, RESULT
from layout import Buf

MAP_W, MAP_H = 128, 24
ROW = 32
FRAMES = 8
SPEED = 3
SOLID, ANIMATED = 1, 2
COIN = 13


def make_tiles():
    tiles = [[0] * 8]
    for t in range(1, 16):
        rows = []
        for line in range(8):
            v = ((t * 37 + line * 11) ^ (t << line)) & 255
            if t <= 4:
                v = 0xFF if line in (0, 4) else (0x81 if (line < 4) == (t & 1) else 0x18)
            rows.append(v)
        tiles.append(rows)
    tiles[COIN] = [0x00, 0x3C, 0x7E, 0x66, 0x66, 0x7E, 0x3C, 0x00]
    return tiles


TILES = make_tiles()
ATTR = [0] + [SOLID] * 8 + [0, 0, 0, 0, 0, ANIMATED, ANIMATED]


def make_map(rng):
    m = [0] * (MAP_W * MAP_H)
    for x in range(MAP_W):
        ground = 20 if (x // 9) % 5 != 3 else 24  # a pit every so often
        for y in range(ground, MAP_H):
            m[y * MAP_W + x] = 1 + (x + y) % 4
        if ground == 24:
            m[23 * MAP_W + x] = 14 + (x & 1)  # water in the pit
        if rng.below(6) == 0:
            y = 6 + rng.below(12)
            for k in range(1 + rng.below(4)):
                if x + k < MAP_W:
                    m[y * MAP_W + x + k] = 5 + rng.below(4)
        if rng.below(5) == 0:
            m[(4 + rng.below(15)) * MAP_W + x] = COIN
        if rng.below(9) == 0:
            m[(2 + rng.below(4)) * MAP_W + x] = 9 + rng.below(4)  # clouds, not solid
    return m


def inputs(rng):
    return {"screen": rng.bytes(ROW * 192), "map": make_map(rng), "checksum": 0x5A5A}


def tile_line(t, line, frame):
    if ATTR[t] & ANIMATED:
        line = (line + frame) & 7
    return TILES[t][line]


def reference(ins):
    screen = list(ins["screen"])
    m = list(ins["map"])
    row, score = 2, 0
    for frame in range(FRAMES):
        sx = frame * SPEED
        fine, coarse = sx & 7, sx >> 3
        for r in range(24):
            for c in range(32):
                left = m[r * MAP_W + coarse + c]
                right = m[r * MAP_W + coarse + c + 1]
                for line in range(8):
                    v = (tile_line(left, line, frame) << fine) & 255
                    v |= tile_line(right, line, frame) >> (8 - fine)
                    screen[(r * 8 + line) * ROW + c] = v
        col = (sx + 100) >> 3
        below = m[(row + 1) * MAP_W + col]
        if not ATTR[below] & SOLID and row < 22:
            row += 1
        elif ATTR[m[row * MAP_W + col + 1]] & SOLID and row > 1:
            row -= 1
        if m[row * MAP_W + col] == COIN:
            m[row * MAP_W + col] = 0
            score += 1
    a = b = 0
    for v in screen + [row, score]:
        a = (a + v) & 0xFFFF
        b = (b + a) & 0xFFFF
    return {"screen": screen, "map": m, "checksum": a ^ b}


PROGRAM = Kernel("scroll", "Tile map scroller: 32 x 24 tiles of a 128-wide map at pixel "
                 "scroll, animated tiles, a player, 8 frames",
                 [Buf("screen", "u8", ROW * 192, 0x2000), Buf("map", "u8", MAP_W * MAP_H, 0x4000),
                  Buf("checksum", "u16", None, RESULT)],
                 inputs, reference)
