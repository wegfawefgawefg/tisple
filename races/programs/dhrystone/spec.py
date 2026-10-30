"""Dhrystone 2.1 with 16-bit ints; the checksum folds the values Dhrystone prints"""

from kernels import Kernel, RESULT
from layout import Buf

RUNS = 200


def inputs(_rng):
    return {"runs": RUNS, "checksum": 0x5A5A}


def reference(ins):
    # Dhrystone's own "should be" values, in the order main folds them
    some = "DHRYSTONE PROGRAM, SOME STRING"
    values = [5, 1, ord("A"), ord("B"), 7, ins["runs"] + 10]
    values += [0, 2, 17] + [ord(c) for c in some]
    values += [0, 1, 18] + [ord(c) for c in some]
    values += [5, 13, 7, 1]
    values += [ord(c) for c in "DHRYSTONE PROGRAM, 1'ST STRING"]
    values += [ord(c) for c in "DHRYSTONE PROGRAM, 2'ND STRING"]
    a = b = 0
    for v in values:
        a = (a + v) & 0xFFFF
        b = (b + a) & 0xFFFF
    return {"checksum": a ^ b}


PROGRAM = Kernel("dhrystone", "Dhrystone 2.1, 16-bit ints, %d runs" % RUNS,
                 [Buf("checksum", "u16", None, RESULT), Buf("runs", "u16", None, RESULT + 2)],
                 inputs, reference)
