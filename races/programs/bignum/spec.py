"""Decimal scores and a bignum: 8 games of 50 point awards into an 8-digit decimal score
and a high-score table, then 100! in bytes, printed in decimal by repeated division"""

import math

from kernels import Kernel, RESULT
from layout import Buf

DIGITS = 8
GAMES = 8
AWARDS = 50
HIGH = 8
DEC_MAX = 200
POINTS = [10, 10, 25, 50, 50, 100, 100, 250, 500, 1000, 2500, 5000, 9999, 65535]


def inputs(rng):
    points = [POINTS[rng.below(len(POINTS))] for _ in range(GAMES * AWARDS)]
    return {"points": points, "high": [0] * (HIGH * DIGITS), "dec": [0x5A] * DEC_MAX,
            "checksum": 0x5A5A}


def reference(ins):
    high = [0] * HIGH
    score = 0
    for g in range(GAMES):
        score = 0
        for k in range(AWARDS):
            score = (score + ins["points"][g * AWARDS + k]) % 10 ** DIGITS
        for at in range(HIGH):
            if score > high[at]:
                high.insert(at, score)
                high.pop()
                break
    high_digits = []
    for h in high:
        high_digits += [int(c) for c in "%08d" % h]
    dec = [ord(c) for c in str(math.factorial(100))]
    a = b = 0
    for v in high_digits + [int(c) for c in "%08d" % score] + dec + [len(dec)]:
        a = (a + v) & 0xFFFF
        b = (b + a) & 0xFFFF
    out = list(ins["dec"])
    out[:len(dec)] = dec
    return {"high": high_digits, "dec": out, "checksum": a ^ b}


PROGRAM = Kernel("bignum", "Decimal scores and a high-score table, then 100! printed by "
                 "bignum division",
                 [Buf("points", "u16", GAMES * AWARDS, 0x2000),
                  Buf("high", "u8", HIGH * DIGITS, 0x3100), Buf("dec", "u8", DEC_MAX, 0x3000),
                  Buf("checksum", "u16", None, RESULT)],
                 inputs, reference)
