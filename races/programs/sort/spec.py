"""Sorting as a program does it: quicksort with an explicit stack, insertion sort to
finish, a stable insertion sort of records and a high-score table"""

from kernels import Kernel, RESULT
from layout import Buf

KEYS = 1024
RECORDS = 64
TOP = 16
SCORE = [("score", "u16"), ("id", "u8")]


def inputs(rng):
    keys = [rng.next() & 0xFFFF for _ in range(KEYS)]
    for i in range(0, KEYS, 7):  # some repeats, as real data has
        keys[i] = keys[(i * 5) % KEYS]
    table = [{"score": rng.below(10000), "id": i} for i in range(RECORDS)]
    for i in range(0, RECORDS, 5):  # ties, so stability shows
        table[i]["score"] = table[(i + 3) % RECORDS]["score"]
    news = [rng.below(12000) for _ in range(RECORDS)]
    top = [{"score": 0x5A5A, "id": 0x5A} for _ in range(TOP)]
    return {"keys": keys, "table": table, "news": news, "top": top, "checksum": 0x5A5A}


def fold(a, b, values):
    for v in values:
        a = (a + v) & 0xFFFF
        b = (b + a) & 0xFFFF
    return a, b


def reference(ins):
    keys = sorted(ins["keys"])
    table = sorted((dict(r) for r in ins["table"]), key=lambda r: -r["score"])
    top = [dict(r) for r in table[:TOP]]
    for k, score in enumerate(ins["news"]):
        if score <= top[TOP - 1]["score"]:
            continue
        at = 0
        while top[at]["score"] >= score:
            at += 1
        top.insert(at, {"score": score, "id": 100 + k})
        top.pop()
    a, b = fold(0, 0, keys)
    for r in table:
        a, b = fold(a, b, [r["score"], r["id"]])
    for r in top:
        a, b = fold(a, b, [r["score"], r["id"]])
    return {"keys": keys, "table": table, "top": top, "checksum": a ^ b}


PROGRAM = Kernel("sort", "Quicksort (explicit stack) and insertion sort of 1024 words, "
                 "a stable record sort, a high-score table",
                 [Buf("keys", "u16", KEYS, 0x2000),
                  Buf("table", SCORE, RECORDS, 0x2800, struct="entry"),
                  Buf("news", "u16", RECORDS, 0x2900),
                  Buf("top", SCORE, TOP, 0x2A00, struct="entry"),
                  Buf("checksum", "u16", None, RESULT)],
                 inputs, reference)
