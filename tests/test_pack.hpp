#pragma once

// what the packed-data tests share (test_pack.cpp, test_unpack.cpp)

#include "check.hpp"

#include <cstdint>
#include <vector>

namespace tisple_tests {

// ZX0 packing and the plain decoder, (pack bytes) in macro code, lib/tables' packed tables
void RunPackTests(TestResults& results);
// lib/unpack's 6502 code on the machine, every mode, against the plain decoder
void RunUnpackLibTests(TestResults& results);
// the same and lib/dhgr's code under the Mockingboard's interrupts, with auxmem's guard
void RunUnpackIrqTests(TestResults& results);

// a small deterministic generator
struct TestRandom {
    uint32_t state = 1;
    uint32_t Next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    int Below(int n) {
        return static_cast<int>(Next() % static_cast<uint32_t>(n));
    }
};

// a made-up double hi-res picture's half (8 KB, in the page's byte order): bands of colour,
// round and square shapes with outlines, dithered shading, a little noise. seed picks the
// scene, half the memory (0 main, 1 aux)
std::vector<uint8_t> DhgrSample(uint32_t seed, int half);

// test data of several shapes: random bytes, few values, runs, repeats far and near, mixes
std::vector<uint8_t> PackSample(TestRandom& r, int size);

} // namespace tisple_tests
