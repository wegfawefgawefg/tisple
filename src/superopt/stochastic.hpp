#pragma once

// STOKE-style stochastic search for windows too long to enumerate: a fixed number of slots,
// each an instruction or empty, mutated at random (change an instruction, change only its
// operand, swap two slots, empty a slot, fill one) and kept by the Metropolis rule under
// cost = 8 x (wrong bits on the test vectors) + cycles. It starts from the window itself.
// Results are tested, never proved, unless the exhaustive check afterwards covers them.

#include "superopt/search.hpp"

#include <cstdint>
#include <vector>

namespace tisple {

// sequences that matched every test vector and passed the random check, each cheaper than
// the one before; steps gets the steps taken
std::vector<std::vector<SoIns>> StochasticSearch(const Window& w, const Facts& facts,
                                                 const SearchOptions& options,
                                                 std::uint64_t& steps, int& rejected);

} // namespace tisple
