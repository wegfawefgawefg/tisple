"""BYTE's sieve (Jim Gilbreath, BYTE, September 1981): 10 passes over 8191 flags"""

from kernels import Kernel, RESULT
from layout import Buf

SIZE = 8190


def inputs(rng):
    return {"flags": rng.bytes(SIZE + 1), "checksum": 0x5A5A}


def reference(_ins):
    flags = [1] * (SIZE + 1)
    count = 0
    for i in range(SIZE + 1):
        if flags[i]:
            prime = i + i + 3
            k = i + prime
            while k <= SIZE:
                flags[k] = 0
                k += prime
            count += 1
    return {"flags": flags, "checksum": count}


PROGRAM = Kernel("bytesieve", "BYTE's sieve as published: 10 passes, 8191 flags, 1899 primes",
                 [Buf("flags", "u8", SIZE + 1, 0x2000), Buf("checksum", "u16", None, RESULT)],
                 inputs, reference)
