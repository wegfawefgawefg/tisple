"""LZ4 block decompression of 8 KB (text and a tile map), then a checksum of the output"""

from kernels import Kernel, RESULT
from layout import Buf

SIZE = 8192
PACKED_AT = 0x4000

WORDS = ("the of and to in is you that it he was for on are as with his they at be this "
         "from have or by one had not but what all were when we there can an your which "
         "their said if do will each about how up out them then she many some so these "
         "would other into has more her two like him see time could no make than first "
         "been its who now people my made over did down only way find use may water long "
         "little very after words called just where most know dungeon dragon treasure "
         "sword lamp door north south east west").split()


def make_data(rng):
    text = []
    while len(text) < SIZE // 2:
        sentence = [WORDS[rng.below(len(WORDS))] for _ in range(4 + rng.below(9))]
        text += [ord(c) for c in " ".join(sentence).capitalize() + ". "]
    data = text[:SIZE // 2]
    # a 64 x 64 tile map: runs of floor and wall, some repeated rows, some noise
    row = []
    while len(data) < SIZE:
        if len(row) == 0 or rng.below(4) == 0:
            row = []
            while len(row) < 64:
                row += [rng.below(16)] * (1 + rng.below(12))
            row = row[:64]
        data += row
    return data[:SIZE]


def lz4_compress(data):
    """greedy LZ4 block: 4-byte hash chains of length one, the last 5 bytes literals"""
    out = []
    table = {}
    anchor = i = 0
    limit = len(data) - 12

    def put_length(n):
        while n >= 255:
            out.append(255)
            n -= 255
        out.append(n)

    def put_sequence(literals, match_len, offset):
        lit = len(literals)
        token = (min(lit, 15) << 4) | (min(match_len - 4, 15) if match_len else 0)
        out.append(token)
        if lit >= 15:
            put_length(lit - 15)
        out.extend(literals)
        if match_len:
            out.extend([offset & 255, offset >> 8])
            if match_len - 4 >= 15:
                put_length(match_len - 4 - 15)

    while i < limit:
        key = tuple(data[i:i + 4])
        candidate = table.get(key)
        table[key] = i
        if candidate is None or i - candidate > 65535:
            i += 1
            continue
        n = 4
        while i + n < len(data) - 5 and data[candidate + n] == data[i + n]:
            n += 1
        put_sequence(data[anchor:i], n, i - candidate)
        for k in range(i + 1, i + n):
            if k < limit:
                table[tuple(data[k:k + 4])] = k
        i += n
        anchor = i
    put_sequence(data[anchor:], 0, 0)
    return out


def lz4_decompress(packed):
    out = []
    i = 0
    while True:
        token = packed[i]
        i += 1
        n = token >> 4
        if n == 15:
            while True:
                b = packed[i]
                i += 1
                n += b
                if b != 255:
                    break
        out += packed[i:i + n]
        i += n
        if i >= len(packed):
            return out
        offset = packed[i] | (packed[i + 1] << 8)
        i += 2
        n = token & 15
        if n == 15:
            while True:
                b = packed[i]
                i += 1
                n += b
                if b != 255:
                    break
        for _ in range(n + 4):
            out.append(out[-offset])


def inputs(rng):
    data = make_data(rng)
    packed = lz4_compress(data)
    assert lz4_decompress(packed) == data
    assert len(packed) <= 0x1E00
    return {"packed": packed + [0] * (0x1E00 - len(packed)), "out": [0x5A] * SIZE,
            "packed_len": len(packed), "checksum": 0x5A5A}


def reference(ins):
    out = lz4_decompress(ins["packed"][:ins["packed_len"]])
    a = b = 0
    for v in out:
        a = (a + v) & 0xFFFF
        b = (b + a) & 0xFFFF
    return {"out": out, "checksum": (a ^ b ^ len(out)) & 0xFFFF}


PROGRAM = Kernel("lz4", "LZ4 block decompression: 8 KB of text and tile map",
                 [Buf("out", "u8", SIZE, 0x2000), Buf("packed", "u8", 0x1E00, PACKED_AT),
                  Buf("checksum", "u16", None, RESULT), Buf("packed_len", "u16", None, RESULT + 2)],
                 inputs, reference)
