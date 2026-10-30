"""A bytecode interpreter: a stack machine running a bubble sort, gcds, Fibonacci, a sieve
and a hash, assembled here"""

from kernels import Kernel, RESULT
from layout import Buf

OPS = ["HALT", "PUSH8", "PUSH16", "LOAD", "STORE", "ADD", "SUB", "AND", "OR", "XOR", "SHL1",
       "SHR1", "LT", "EQ", "JMP", "JZ", "JNZ", "DUP", "DROP", "SWAP", "LOADA", "STOREA",
       "CALL", "RET", "INC", "OUT"]
OP = {name: i for i, name in enumerate(OPS)}
WIDTH = {"PUSH8": 1, "PUSH16": 2, "LOAD": 1, "STORE": 1, "INC": 1, "JMP": 2, "JZ": 2,
         "JNZ": 2, "CALL": 2}

SOURCE = """
; bubble sort mem[0..32), the compare and swap a subroutine
        PUSH8 31  STORE 3
pass:   PUSH8 0  STORE 0
step:   CALL order
        INC 0
        LOAD 0  LOAD 3  LT  JNZ step
        LOAD 3  PUSH8 1  SUB  DUP  STORE 3  JNZ pass
        PUSH8 0  LOADA  OUT  PUSH8 31  LOADA  OUT
        JMP gcds
order:  LOAD 0  LOADA  STORE 4
        LOAD 0  PUSH8 1  ADD  LOADA  STORE 5
        LOAD 5  LOAD 4  LT  JZ ordered
        LOAD 0  LOAD 5  STOREA
        LOAD 0  PUSH8 1  ADD  LOAD 4  STOREA
ordered: RET
; gcd by subtraction of 16 pairs from mem[32..64), each one more than its byte
gcds:   PUSH8 0  STORE 9
pair:   LOAD 9  SHL1  PUSH8 32  ADD  DUP  LOADA  PUSH8 1  ADD  STORE 10
        PUSH8 1  ADD  LOADA  PUSH8 1  ADD  STORE 11
gcd:    LOAD 10  LOAD 11  EQ  JNZ gcd_done
        LOAD 10  LOAD 11  LT  JZ x_bigger
        LOAD 11  LOAD 10  SUB  STORE 11  JMP gcd
x_bigger: LOAD 10  LOAD 11  SUB  STORE 10  JMP gcd
gcd_done: LOAD 10  OUT
        INC 9
        LOAD 9  PUSH8 16  LT  JNZ pair
; 24 Fibonacci numbers, their low bytes out
        PUSH8 0  STORE 6  PUSH8 1  STORE 7  PUSH8 24  STORE 8
fib:    LOAD 6  OUT
        LOAD 6  LOAD 7  ADD
        LOAD 7  STORE 6
        STORE 7
        LOAD 8  PUSH8 1  SUB  DUP  STORE 8  JNZ fib
; a hash of all of mem before the sieve takes it
        PUSH8 0  STORE 12  PUSH8 0  STORE 0
hash:   LOAD 12  SHL1  LOAD 12  SHR1  XOR  LOAD 0  LOADA  XOR  PUSH16 0x7FFF  AND  STORE 12
        INC 0  LOAD 0  PUSH16 256  LT  JNZ hash
        LOAD 12  OUT  LOAD 12  PUSH16 0x8000  OR  STORE 13
; sieve of the bytes: count the primes below 256
        PUSH8 0  STORE 0
fill:   LOAD 0  PUSH8 1  STOREA
        INC 0
        LOAD 0  PUSH16 256  LT  JNZ fill
        PUSH8 2  STORE 0  PUSH8 0  STORE 2
outer:  LOAD 0  LOADA  JZ next
        INC 2
        LOAD 0  LOAD 0  ADD  STORE 1
inner:  LOAD 1  PUSH16 256  LT  JZ next
        LOAD 1  PUSH8 0  STOREA
        LOAD 1  LOAD 0  ADD  STORE 1
        JMP inner
next:   INC 0
        LOAD 0  PUSH16 256  LT  JNZ outer
        LOAD 2  OUT
        LOAD 13  LOAD 2  SWAP  DROP  DROP
        HALT
"""


def assemble(source):
    items = []  # (op name, argument text) or label
    for line in source.splitlines():
        line = line.split(";")[0]
        for tok in line.split():
            items.append(tok)
    labels = {}
    at = 0
    i = 0
    while i < len(items):
        tok = items[i]
        if tok.endswith(":"):
            labels[tok[:-1]] = at
            i += 1
            continue
        at += 1 + WIDTH.get(tok, 0)
        i += 1 + (1 if tok in WIDTH else 0)
    code = []
    i = 0
    while i < len(items):
        tok = items[i]
        if tok.endswith(":"):
            i += 1
            continue
        code.append(OP[tok])
        if tok in WIDTH:
            arg = items[i + 1]
            value = labels[arg] if arg in labels else int(arg, 0)
            code += [value & 255, value >> 8][:WIDTH[tok]]
            i += 1
        i += 1
    return code


CODE = assemble(SOURCE)


def inputs(rng):
    code = CODE + [0] * (1024 - len(CODE))
    return {"code": code, "mem": rng.bytes(256), "out_buf": [0x5A] * 256, "checksum": 0x5A5A}


def run_vm(code, mem):
    stack, rstack, var = [], [], [0] * 16
    out = []
    pc = steps = 0
    while True:
        op = OPS[code[pc]]
        pc += 1
        steps += 1
        if op == "HALT":
            return var, out, steps, len(stack)
        if op in WIDTH:
            arg = code[pc] if WIDTH[op] == 1 else code[pc] | (code[pc + 1] << 8)
            pc += WIDTH[op]
        if op in ("PUSH8", "PUSH16"):
            stack.append(arg)
        elif op == "LOAD":
            stack.append(var[arg])
        elif op == "STORE":
            var[arg] = stack.pop()
        elif op == "INC":
            var[arg] = (var[arg] + 1) & 0xFFFF
        elif op in ("ADD", "SUB", "AND", "OR", "XOR", "LT", "EQ"):
            b = stack.pop()
            a = stack.pop()
            stack.append({"ADD": (a + b) & 0xFFFF, "SUB": (a - b) & 0xFFFF, "AND": a & b,
                          "OR": a | b, "XOR": a ^ b, "LT": int(a < b), "EQ": int(a == b)}[op])
        elif op == "SHL1":
            stack.append((stack.pop() << 1) & 0xFFFF)
        elif op == "SHR1":
            stack.append(stack.pop() >> 1)
        elif op == "JMP":
            pc = arg
        elif op == "JZ":
            if stack.pop() == 0:
                pc = arg
        elif op == "JNZ":
            if stack.pop() != 0:
                pc = arg
        elif op == "DUP":
            stack.append(stack[-1])
        elif op == "DROP":
            stack.pop()
        elif op == "SWAP":
            stack[-1], stack[-2] = stack[-2], stack[-1]
        elif op == "LOADA":
            stack.append(mem[stack.pop() & 255])
        elif op == "STOREA":
            v = stack.pop()
            mem[stack.pop() & 255] = v & 255
        elif op == "CALL":
            rstack.append(pc)
            pc = arg
        elif op == "RET":
            pc = rstack.pop()
        elif op == "OUT":
            out.append(stack.pop() & 255)


def reference(ins):
    mem = list(ins["mem"])
    var, out, steps, depth = run_vm(ins["code"], mem)
    a = b = 0
    for v in var + mem + out + [len(out), steps, depth]:
        a = (a + v) & 0xFFFF
        b = (b + a) & 0xFFFF
    out_buf = list(ins["out_buf"])
    out_buf[:len(out)] = out
    return {"mem": mem, "out_buf": out_buf, "checksum": a ^ b}


PROGRAM = Kernel("vm", "Bytecode interpreter: a stack machine sorting, gcds, Fibonacci, sieve",
                 [Buf("code", "u8", 1024, 0x2000), Buf("mem", "u8", 256, 0x3000),
                  Buf("out_buf", "u8", 256, 0x3100), Buf("checksum", "u16", None, RESULT)],
                 inputs, reference)
