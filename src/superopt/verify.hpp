#pragma once

// Is a candidate the same as a window on everything live after it?
// - random test vectors on the evaluator, edge values mixed in;
// - then every input, when the inputs (registers, flags and bytes either sequence reads
//   before writing, and live ones only one of them writes) are few enough: proved;
// - and test vectors on the cycle-exact core in src/emu, which also checks the evaluator.
// Facts narrow the inputs to what the profile saw on entry; a result that needs them holds
// only while they do.

#include "superopt/window.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

// input locations: A X Y, then N Z C V, then the cells
constexpr int kLocCount = 7 + kMaxCells;

struct Facts {
    std::uint8_t lo[kLocCount] = {};
    std::uint8_t hi[kLocCount] = {};
    Facts();
};

// lo..hi of everything, so a test vector of the window
struct VectorRng {
    std::uint64_t s = 1;
    std::uint64_t Next();
};

SoState RandomState(const Window& w, const Facts& facts, VectorRng& rng);
std::vector<SoState> MakeVectors(const Window& w, const Facts& facts, int count,
                                 std::uint64_t seed);

// agree on every live register, flag and byte; neither went bad
bool SameLive(const Window& w, const SoState& x, const SoState& y);

enum class Proof : std::uint8_t {
    kNone,
    kTested, // random test vectors only
    kProved, // every input
};

const char* ProofName(Proof proof);

struct EquivResult {
    bool equal = false;
    Proof proof = Proof::kNone;
    std::uint64_t cases = 0;
    double input_bits = 0; // log2 of the input space
    SoState counterexample;
};

EquivResult CheckEquivalent(const Window& w, const std::vector<SoIns>& a,
                            const std::vector<SoIns>& b, const Facts& facts, int random_vectors,
                            double exhaustive_bits, std::uint64_t seed);

// both on the simulator, n vectors; false with why on a difference or when the evaluator
// disagrees with the simulator about a
bool CheckOnSimulator(const Window& w, const std::vector<SoIns>& a, const std::vector<SoIns>& b,
                      const Facts& facts, int vectors, std::uint64_t seed, std::string& why);

// the facts that differ from "anything": "C=0, A in $00..$08, ($10),Y in $00..$01"
std::string FactsText(const Window& w, const Facts& facts);
bool HasFacts(const Facts& facts);

} // namespace tisple
