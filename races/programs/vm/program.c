/* A bytecode interpreter: a 16-bit stack machine with 16 variables, a byte memory and a
 * call stack, the loop a switch on the opcode, as interpreters are written in C. The
 * program in code (assembled by spec.py) bubble-sorts, takes gcds, makes Fibonacci
 * numbers, hashes and sieves mem, and outputs bytes; the checksum folds the variables,
 * mem, the output and the step count. */
#include "race.h"

enum {
    HALT, PUSH8, PUSH16, LOAD, STORE, ADD, SUB, AND, OR, XOR, SHL1, SHR1, LT, EQ, JMP, JZ,
    JNZ, DUP, DROP, SWAP, LOADA, STOREA, CALL, RET, INC, OUT
};

#define STACK 32
#define CALLS 16

static uint16_t stack[STACK];
static uint8_t sp;
static uint16_t calls[CALLS];
static uint8_t csp;
static uint16_t vars[16];
static uint16_t pc;
static uint16_t steps;
static uint8_t out_len;

/* (uint16_t)(code[i]) with the parentheses: Oscar64 casts a constant-pointer code before
 * indexing it otherwise (docs/races.md) */
static uint16_t fetch16(void) {
    uint16_t v = code[pc] | ((uint16_t)(code[pc + 1]) << 8);
    pc += 2;
    return v;
}

static void run(void) {
    uint8_t op;
    uint16_t a, b;
    pc = 0;
    sp = 0;
    csp = 0;
    steps = 0;
    for (;;) {
        op = code[pc++];
        steps++;
        switch (op) {
        case HALT: return;
        case PUSH8: stack[sp++] = code[pc++]; break;
        case PUSH16: stack[sp++] = fetch16(); break;
        case LOAD: stack[sp++] = vars[code[pc++]]; break;
        case STORE: vars[code[pc++]] = stack[--sp]; break;
        case ADD: sp--; stack[sp - 1] += stack[sp]; break;
        case SUB: sp--; stack[sp - 1] -= stack[sp]; break;
        case AND: sp--; stack[sp - 1] &= stack[sp]; break;
        case OR: sp--; stack[sp - 1] |= stack[sp]; break;
        case XOR: sp--; stack[sp - 1] ^= stack[sp]; break;
        case SHL1: stack[sp - 1] <<= 1; break;
        case SHR1: stack[sp - 1] >>= 1; break;
        case LT: sp--; stack[sp - 1] = stack[sp - 1] < stack[sp]; break;
        case EQ: sp--; stack[sp - 1] = stack[sp - 1] == stack[sp]; break;
        case JMP: pc = fetch16(); break;
        case JZ:
            a = fetch16();
            if (stack[--sp] == 0) pc = a;
            break;
        case JNZ:
            a = fetch16();
            if (stack[--sp] != 0) pc = a;
            break;
        case DUP: stack[sp] = stack[sp - 1]; sp++; break;
        case DROP: sp--; break;
        case SWAP:
            a = stack[sp - 1];
            stack[sp - 1] = stack[sp - 2];
            stack[sp - 2] = a;
            break;
        case LOADA: stack[sp - 1] = mem[(uint8_t)stack[sp - 1]]; break;
        case STOREA:
            b = stack[--sp];
            a = stack[--sp];
            mem[(uint8_t)a] = (uint8_t)b;
            break;
        case CALL:
            a = fetch16();
            calls[csp++] = pc;
            pc = a;
            break;
        case RET: pc = calls[--csp]; break;
        case INC: vars[code[pc++]]++; break;
        case OUT: out_buf[out_len++] = (uint8_t)stack[--sp]; break;
        }
    }
}

static uint16_t sum_a, sum_b;

static void fold(uint16_t v) {
    sum_a += v;
    sum_b += sum_a;
}

int main(void) {
    uint16_t i;
    out_len = 0;
    for (i = 0; i < 16; i++) vars[i] = 0;
    run();
    sum_a = 0;
    sum_b = 0;
    for (i = 0; i < 16; i++) fold(vars[i]);
    for (i = 0; i < 256; i++) fold(mem[i]);
    for (i = 0; i < out_len; i++) fold(out_buf[i]);
    fold(out_len);
    fold(steps);
    fold(sp);
    checksum = sum_a ^ sum_b;
    return 0;
}
