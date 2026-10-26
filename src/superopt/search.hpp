#pragma once

// The window superoptimizer: the cheapest sequence (cycles, then bytes) with the window's
// effect on everything live after it.
//
// Exhaustive mode enumerates sequences over the documented NMOS opcodes the evaluator
// models, with operands from the window's own memory operands and a few immediates (0, 1,
// $FF, the window's constants). Bytes outside the window are left out: everything else in
// memory is live, so a candidate could only read them as unknowns, never write them.
// Pruning:
// - branch and bound on cycles, with a lower bound from what is still wrong (each wrong
//   byte needs its own write of at least 3 cycles, wrong registers or flags 2 more);
// - registers and flags the window leaves alone and that are live are never written, nor
//   bytes it does not write (restoring them is never cheaper);
// - every byte the live results depend on has to be read: its cheapest read counts towards
//   the bound until some instruction reads it;
// - an instruction that overwrites all the previous one wrote without reading it, and
//   independent neighbours in the other order, are skipped;
// - a table of states reached (on the test vectors) and the cheapest cost to reach them:
//   two prefixes that agree on every test vector go on the same way, so only the cheaper
//   is extended. This makes the search complete up to test-vector collisions.
// Every candidate that passes the search's vectors is checked by verify.hpp.
//
// Stochastic mode (STOKE-style MCMC) mutates a sequence at random under a cost of its
// mismatches plus its cycles, for windows too long to enumerate; its results are only ever
// "tested", never proved.

#include "superopt/verify.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

struct SearchOptions {
    int max_len = 5;                   // longest candidate
    std::uint64_t max_nodes = 1000000; // the search budget; keeps results deterministic
    int vectors = 16;                  // test vectors during the search
    int verify_vectors = 2048;
    int sim_vectors = 128;
    double exhaustive_bits = 24; // prove by every input up to this many input bits
    std::uint64_t seed = 1;
    std::uint32_t table_bits = 20;      // the table of reached states: 2^bits entries
    std::uint64_t stochastic_steps = 0; // 0: exhaustive search; else MCMC steps
    // only results better than this count (cycles, then bytes); -1: than the window
    int bound_cycles = -1;
    int bound_bytes = -1;
};

struct SearchResult {
    bool found = false; // a cheaper sequence that passed every check
    std::vector<SoIns> code;
    int cycles = 0;
    int bytes = 0;
    Proof proof = Proof::kNone;
    double input_bits = 0;
    bool complete = false; // the enumeration finished inside its budget
    std::uint64_t nodes = 0;
    double seconds = 0;
    int rejected = 0; // candidates that passed the search's vectors but not the checks
    std::string why_rejected;
};

// the instructions candidates are made of, cheapest first
std::vector<SoIns> CandidateSet(const Window& w);

SearchResult Superoptimize(const Window& w, const Facts& facts, const SearchOptions& options);

// the facts of `facts` the candidate still needs: each one dropped and the check rerun
Facts NeededFacts(const Window& w, const std::vector<SoIns>& candidate, const Facts& facts,
                  const SearchOptions& options);

} // namespace tisple
