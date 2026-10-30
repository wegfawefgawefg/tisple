"""A sprite blitter: 24 masked 16 x 16 sprites shifted into a 256 x 192 bitmap, with the
background saved under each and put back, for 8 frames"""

from kernels import Kernel, RESULT
from layout import Buf

ROW = 32  # bytes a row: 256 pixels, the leftmost in bit 7
ROWS = 192
SPRITES = 24
FRAMES = 8
SPRITE = [("x", "u8"), ("y", "u8"), ("dx", "i8"), ("dy", "i8"), ("shape", "u8")]
MAX_X = 256 - 24
MAX_Y = ROWS - 16

ART = [
    # ball
    ["......####......", "....########....", "...##########...", "..####..######..",
     ".####....######.", ".####....######.", "################", "################",
     "################", "################", ".##############.", ".##############.",
     "..############..", "...##########...", "....########....", "......####......"],
    # ship
    [".......##.......", ".......##.......", "......####......", "......####......",
     ".....######.....", ".....##..##.....", "....###..###....", "....########....",
     "...##########...", "..####.##.####..", ".#####.##.#####.", "################",
     "####.######.####", "###...####...###", "##.....##.....##", "#..............#"],
    # ghost
    [".....######.....", "...##########...", "..############..", ".###..####..###.",
     ".##....##....##.", ".##..#.##..#.##.", ".###..####..###.", "################",
     "################", "################", "################", "################",
     "################", "##.###.##.###.##", "#...#...#...#...", "................"],
    # diamond
    [".......##.......", "......####......", ".....######.....", "....###..###....",
     "...###....###...", "..###......###..", ".###........###.", "###....##....###",
     "###....##....###", ".###........###.", "..###......###..", "...###....###...",
     "....###..###....", ".....######.....", "......####......", ".......##......."],
]


def shape_rows(art, n):
    """per row: image high, image low, opaque high, opaque low; the image has a dark
    stripe pattern inside the opaque pixels"""
    rows = []
    for y, line in enumerate(art):
        opaque = int(line.replace("#", "1").replace(".", "0"), 2)
        image = opaque & (0xFFFF ^ (0x1111 << ((y + n) & 3)))
        rows.append([image >> 8, image & 255, opaque >> 8, opaque & 255])
    return rows


SHAPES = [shape_rows(a, n) for n, a in enumerate(ART)]


def inputs(rng):
    screen = [((x * 7) ^ (y * 13)) & 0x55 if (x + y) % 3 else rng.below(256)
              for y in range(ROWS) for x in range(ROW)]
    sprites = []
    for i in range(SPRITES):
        sprites.append({"x": rng.below(MAX_X + 1), "y": rng.below(MAX_Y + 1),
                        "dx": rng.below(7) - 3 or 2, "dy": rng.below(5) - 2 or -1,
                        "shape": i & 3})
    return {"screen": screen, "sprites": sprites, "checksum": 0x5A5A}


def reference(ins):
    screen = list(ins["screen"])
    sprites = [dict(s) for s in ins["sprites"]]
    saved = [[0] * 48 for _ in range(SPRITES)]

    def save_and_draw(i, s):
        at = s["y"] * ROW + (s["x"] >> 3)
        shift = s["x"] & 7
        for r in range(16):
            i0, i1, o0, o1 = SHAPES[s["shape"]][r]
            image = ((i0 << 16) | (i1 << 8)) >> shift
            opaque = ((o0 << 16) | (o1 << 8)) >> shift
            for k in range(3):
                p = at + r * ROW + k
                saved[i][r * 3 + k] = screen[p]
                byte_shift = 16 - 8 * k
                screen[p] = (screen[p] & ~(opaque >> byte_shift) & 255) | \
                    ((image >> byte_shift) & 255)

    def restore(i, s):
        at = s["y"] * ROW + (s["x"] >> 3)
        for r in range(16):
            for k in range(3):
                screen[at + r * ROW + k] = saved[i][r * 3 + k]

    for i, s in enumerate(sprites):
        save_and_draw(i, s)
    for _ in range(FRAMES):
        for i in range(SPRITES - 1, -1, -1):
            restore(i, sprites[i])
        for s in sprites:
            x, y = s["x"] + s["dx"], s["y"] + s["dy"]
            if x < 0 or x > MAX_X:
                s["dx"] = -s["dx"]
                x = s["x"] + s["dx"]
            if y < 0 or y > MAX_Y:
                s["dy"] = -s["dy"]
                y = s["y"] + s["dy"]
            s["x"], s["y"] = x, y
        for i, s in enumerate(sprites):
            save_and_draw(i, s)
    a = b = 0
    for v in screen:
        a = (a + v) & 0xFFFF
        b = (b + a) & 0xFFFF
    return {"screen": screen, "sprites": sprites, "checksum": a ^ b}


PROGRAM = Kernel("blit", "Sprite blitter: 24 masked 16 x 16 sprites shifted into a bitmap, "
                 "background saved and restored, 8 frames",
                 [Buf("screen", "u8", ROW * ROWS, 0x2000),
                  Buf("sprites", SPRITE, SPRITES, 0x5800, struct="sprite"),
                  Buf("checksum", "u16", None, RESULT)],
                 inputs, reference)
