"""The race's kernels: buffers, inputs and a Python reference for each.

Each kernel lives in races/kernels/<name>/: kernel.c (all three C compilers), optional
lib.c (the same job through the C library, e.g. memset), and kernel.tsp. The kernel is
`main` in every language, so no compiler has to be talked out of inlining it. Every
buffer is written when main is entered (inputs, and a known background for outputs) and
the outputs are compared with the reference after main returns.
"""

from layout import Buf, Rng

RESULT = 0x5F00  # scalar results and parameters live here


class Kernel:
    def __init__(self, name, title, buffers, inputs, reference, c_headers=None,
                 folder="kernels"):
        self.name = name
        self.title = title
        self.buffers = buffers
        self.inputs = inputs  # rng -> {buffer name: value}, every buffer
        self.reference = reference  # inputs -> {buffer name: expected value}
        self.c_headers = c_headers or {}  # extra generated C headers: name -> () -> text
        self.folder = folder  # races/<folder>/<name>/ holds the sources

    def buffer(self, name):
        for b in self.buffers:
            if b.name == name:
                return b
        raise KeyError(name)


def u16(v):
    return v & 0xFFFF


# --- multiply ---------------------------------------------------------------------------


def mul8_inputs(rng):
    return {"a": rng.bytes(256), "b": rng.bytes(256), "result": 0x5A5A}


def mul8_reference(ins):
    return {"result": u16(sum(x * y for x, y in zip(ins["a"], ins["b"])))}


def mul16_inputs(rng):
    xs = [rng.next() & 0xFFFF for _ in range(128)]
    ys = [rng.next() & 0xFFFF for _ in range(128)]
    return {"x": xs, "y": ys, "result": 0x5A5A}


def mul16_reference(ins):
    total = 0
    for x, y in zip(ins["x"], ins["y"]):
        total = u16(total + u16(x * y))
    return {"result": total}


# --- memory -----------------------------------------------------------------------------


def fill_inputs(rng):
    return {"buf": rng.bytes(8192), "value": 0xA7}


def fill_reference(ins):
    return {"buf": [ins["value"]] * 8192}


def copy_inputs(rng):
    return {"src": rng.bytes(4096), "dst": rng.bytes(4096)}


def copy_reference(ins):
    return {"dst": list(ins["src"])}


def sieve_inputs(rng):
    return {"flags": rng.bytes(8192), "result": 0x5A5A}


def sieve_reference(ins):
    flags = [1] * 8192
    count = 0
    for i in range(2, 8192):
        if flags[i]:
            count += 1
            for k in range(i + i, 8192, i):
                flags[k] = 0
    flags[0] = flags[1] = 1
    return {"flags": flags, "result": count}


# --- strings ----------------------------------------------------------------------------

STRLEN = 3000


def strlen_inputs(rng):
    text = rng.bytes(4096, 1, 256)
    text[STRLEN] = 0
    return {"str": text, "result": 0x5A5A}


def strlen_reference(ins):
    return {"result": ins["str"].index(0)}


NEEDLE = 6
NEEDLE_AT = 3500


def search_inputs(rng):
    text = [ord("a") + rng.below(4) for _ in range(4096)]
    needle = [ord("a") + rng.below(4) for _ in range(NEEDLE)]
    text[NEEDLE_AT:NEEDLE_AT + NEEDLE] = needle
    # break any earlier match, so the answer is NEEDLE_AT
    for i in range(NEEDLE_AT):
        if text[i:i + NEEDLE] == needle:
            text[i] = ord("e")
    return {"text": text, "needle": needle, "result": 0x5A5A}


def search_reference(ins):
    text, needle = ins["text"], ins["needle"]
    for i in range(4096 - NEEDLE + 1):
        if text[i:i + NEEDLE] == needle:
            return {"result": i}
    return {"result": 0xFFFF}


# --- sort and crc -----------------------------------------------------------------------


def sort_inputs(rng):
    return {"arr": rng.bytes(256)}


def sort_reference(ins):
    return {"arr": sorted(ins["arr"])}


def crc_inputs(rng):
    return {"data": rng.bytes(1024), "result": 0x5A5A}


def crc_reference(ins):
    crc = 0xFFFF
    for b in ins["data"]:
        crc ^= b << 8
        for _ in range(8):
            if crc & 0x8000:
                crc = ((crc << 1) ^ 0x1021) & 0xFFFF
            else:
                crc = (crc << 1) & 0xFFFF
    return {"result": crc}


# --- mandelbrot -------------------------------------------------------------------------

MANDEL_W = 32
MANDEL_H = 24
MANDEL_ITER = 15


def mandel_inputs(rng):
    # 6 fractional bits: x from -2.0, y from -1.125, 0.09375 a pixel
    return {"iters": rng.bytes(MANDEL_W * MANDEL_H), "x0": -128, "y0": -72, "step": 6}


def mandel_reference(ins):
    out = []
    cy = ins["y0"]
    for _py in range(MANDEL_H):
        cx = ins["x0"]
        for _px in range(MANDEL_W):
            zx = zy = 0
            n = 0
            while n < MANDEL_ITER:
                if zx >= 128 or zx <= -128 or zy >= 128 or zy <= -128:
                    break
                zx2 = (zx * zx) >> 6
                zy2 = (zy * zy) >> 6
                if zx2 + zy2 >= 256:
                    break
                zy = ((zx * zy) >> 5) + cy
                zx = zx2 - zy2 + cx
                assert -32768 <= zx < 32768 and -32768 <= zy < 32768
                n += 1
            out.append(n)
            cx += ins["step"]
        cy += ins["step"]
    return {"iters": out}


# --- life -------------------------------------------------------------------------------

STRIDE = 42


def life_inputs(rng):
    cur = [0] * 2100
    for y in range(1, 49):
        for x in range(1, 41):
            cur[y * STRIDE + x] = 1 if rng.below(256) < 80 else 0
    return {"cur": cur, "next": [0xEE] * 2100}


def life_reference(ins):
    cur = ins["cur"]
    out = list(ins["next"])
    for y in range(1, 49):
        for x in range(1, 41):
            n = 0
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    if dx or dy:
                        n += cur[(y + dy) * STRIDE + x + dx]
            alive = cur[y * STRIDE + x]
            out[y * STRIDE + x] = 1 if n == 3 or (n == 2 and alive == 1) else 0
    return {"next": out}


# --- hi-res lines -----------------------------------------------------------------------

LINES = 24


def hgr_row_start(y):
    return 0x2000 + ((y & 7) << 10) + (((y >> 3) & 7) << 7) + (y >> 6) * 40


def line_inputs(rng):
    return {
        "screen": [0] * 8192,
        "x0s": [rng.below(280) for _ in range(LINES)],
        "y0s": [rng.below(192) for _ in range(LINES)],
        "x1s": [rng.below(280) for _ in range(LINES)],
        "y1s": [rng.below(192) for _ in range(LINES)],
    }


def _c_table(name, values):
    rows = []
    for i in range(0, len(values), 16):
        rows.append("    " + ", ".join(str(v) for v in values[i:i + 16]) + ",")
    return "static const uint8_t %s[%d] = {\n%s\n};\n" % (name, len(values), "\n".join(rows))


def hgr_tables_header():
    rows = [hgr_row_start(y) for y in range(192)]
    return ("/* written by races/race.py: lib/hgr's tables */\n" +
            _c_table("ROW_LO", [r & 0xFF for r in rows]) +
            _c_table("ROW_HI", [r >> 8 for r in rows]) +
            _c_table("COL_BYTE", [x // 7 for x in range(280)]) +
            _c_table("COL_BIT", [1 << (x % 7) for x in range(280)]))


def line_reference(ins):
    screen = list(ins["screen"])

    def plot(x, y):
        at = hgr_row_start(y) + x // 7 - 0x2000
        screen[at] |= 1 << (x % 7)

    for i in range(LINES):
        x, y = ins["x0s"][i], ins["y0s"][i]
        x_end, y_end = ins["x1s"][i], ins["y1s"][i]
        dx, step_x = x_end - x, 1
        if dx < 0:
            dx, step_x = -dx, -1
        dy, step_y = y - y_end, 1
        if dy > 0:
            dy, step_y = -dy, -1
        err = dx + dy
        while True:
            plot(x, y)
            if x == x_end and y == y_end:
                break
            twice = err << 1
            if twice >= dy:
                err += dy
                x += step_x
            if twice <= dx:
                err += dx
                y += step_y
    return {"screen": screen}


# --- sprites ----------------------------------------------------------------------------

SPRITES = 64
SPRITE_FRAMES = 16
SPRITE = [("x", "u8"), ("y", "u8"), ("dx", "i8"), ("dy", "i8"), ("frame", "u8")]


def sprites_inputs(rng):
    sprites = []
    for _ in range(SPRITES):
        sprites.append({
            "x": 8 + rng.below(232),
            "y": 8 + rng.below(176),
            "dx": rng.below(7) - 3 or 1,
            "dy": rng.below(7) - 3 or -1,
            "frame": rng.below(8),
        })
    return {"sprites": sprites}


def sprites_reference(ins):
    sprites = [dict(s) for s in ins["sprites"]]
    for _ in range(SPRITE_FRAMES):
        for s in sprites:
            s["x"] = (s["x"] + s["dx"]) & 0xFF
            if s["x"] < 8 or s["x"] >= 240:
                s["dx"] = -s["dx"]
            s["y"] = (s["y"] + s["dy"]) & 0xFF
            if s["y"] < 8 or s["y"] >= 184:
                s["dy"] = -s["dy"]
            s["frame"] = (s["frame"] + 1) & 7
    return {"sprites": sprites}


KERNELS = [
    Kernel("mul8", "8-bit multiply: 256 byte products summed in 16 bits",
           [Buf("a", "u8", 256, 0x4000), Buf("b", "u8", 256, 0x4100),
            Buf("result", "u16", None, RESULT)],
           mul8_inputs, mul8_reference),
    Kernel("mul16", "16-bit multiply: 128 word products summed, wrapping",
           [Buf("x", "u16", 128, 0x4000), Buf("y", "u16", 128, 0x4100),
            Buf("result", "u16", None, RESULT)],
           mul16_inputs, mul16_reference),
    Kernel("fill", "Fill 8 KB with a byte",
           [Buf("buf", "u8", 8192, 0x2000), Buf("value", "u8", None, RESULT)],
           fill_inputs, fill_reference),
    Kernel("memcpy", "Copy 4 KB",
           [Buf("src", "u8", 4096, 0x2000), Buf("dst", "u8", 4096, 0x4000)],
           copy_inputs, copy_reference),
    Kernel("sieve", "Sieve of Eratosthenes, 8192 flags, count the primes",
           [Buf("flags", "u8", 8192, 0x2000), Buf("result", "u16", None, RESULT)],
           sieve_inputs, sieve_reference),
    Kernel("strlen", "Length of a 3000-byte string",
           [Buf("str", "u8", 4096, 0x2000), Buf("result", "u16", None, RESULT)],
           strlen_inputs, strlen_reference),
    Kernel("search", "Naive search for 6 bytes in 4 KB of text (found at 3500)",
           [Buf("text", "u8", 4096, 0x2000), Buf("needle", "u8", NEEDLE, 0x5E00),
            Buf("result", "u16", None, RESULT)],
           search_inputs, search_reference),
    Kernel("sort", "Insertion sort of 256 bytes",
           [Buf("arr", "u8", 256, 0x4000)],
           sort_inputs, sort_reference),
    Kernel("crc16", "CRC-16-CCITT, bit by bit, over 1 KB",
           [Buf("data", "u8", 1024, 0x4000), Buf("result", "u16", None, RESULT)],
           crc_inputs, crc_reference),
    Kernel("mandel", "Mandelbrot, 32 x 24 pixels, 16-bit fixed point (6 fraction bits)",
           [Buf("iters", "u8", MANDEL_W * MANDEL_H, 0x4000),
            Buf("x0", "i16", None, RESULT), Buf("y0", "i16", None, RESULT + 2),
            Buf("step", "i16", None, RESULT + 4)],
           mandel_inputs, mandel_reference),
    Kernel("life", "One Life step, 40 x 48 cells, as programs/life",
           [Buf("cur", "u8", 2100, 0x2000), Buf("next", "u8", 2100, 0x4000)],
           life_inputs, life_reference),
    Kernel("line", "24 hi-res lines (Bresenham and table plot, as lib/hgr)",
           [Buf("screen", "u8", 8192, 0x2000, tisple_declared=False),
            Buf("x0s", "u16", LINES, 0x5E00), Buf("y0s", "u8", LINES, 0x5E40),
            Buf("x1s", "u16", LINES, 0x5E80), Buf("y1s", "u8", LINES, 0x5EC0)],
           line_inputs, line_reference, c_headers={"hgr_tables.h": hgr_tables_header}),
    Kernel("sprites", "Move and bounce 64 sprites (array of structs) for 16 frames",
           [Buf("sprites", SPRITE, SPRITES, 0x4000, struct="sprite")],
           sprites_inputs, sprites_reference),
]


def by_name(name):
    for k in KERNELS:
        if k.name == name:
            return k
    raise KeyError(name)


def make_inputs(kernel):
    return kernel.inputs(Rng(0x7157 + sum(map(ord, kernel.name))))
