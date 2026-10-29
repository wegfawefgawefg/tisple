#pragma once

// Exact delays, (wait_cycles n): code whose bytes depend on the address it lands at, so the
// count is exact wherever it lands, while its size does not. A taken branch costs one cycle
// more when it crosses a page, so the counts are worked out for the address.
//
// - A constant wait of n cycles (2 to 65535) keeps A, X, Y and memory, and may change N, V,
//   Z and C. It is NOPs, BIT $00 and PHP/PLP pairs, before or after a counted loop when that
//   is shorter: A counts down (kept with PHA/PLA), with X counting the rounds of a nested
//   loop for long waits (its first round counts apart, so every n has a short plan). The
//   fewest bytes win; the size is the smallest that a plan for every address fits exactly.
// - A variable wait reads a u16 n from memory (low byte at `low`, high byte after it) and
//   takes n + kVarWaitOverhead cycles for every n: 256-cycle rounds for the high byte, then
//   the low byte's three low bits as branches taken or not (1, 2 and 4 cycles) and an
//   8-cycle loop for the rest. It keeps A, X and Y, counts in one zero-page byte of the
//   compiler's scratch, and may change N, V, Z and C. Its data-dependent branches must not
//   cross a page, so a JMP over a filler goes in at the one point between them that keeps
//   every branch inside its page.

#include <cstdint>
#include <vector>

namespace tisple {

constexpr std::uint32_t kMinWait = 2;
constexpr std::uint32_t kMaxWait = 65535;
constexpr std::uint32_t kVarWaitOverhead = 47;

// bytes of a constant wait of n cycles (kMinWait to kMaxWait), at any address
int WaitSize(std::uint32_t n);
// the code of a constant wait of n cycles at address, WaitSize(n) bytes
void WaitBytes(std::uint32_t n, int size, std::uint16_t address, std::vector<std::uint8_t>& out);

// bytes of a variable wait, at any address
int VarWaitSize();
// the code of a variable wait at address: n at low and low + 1, counting in zero-page scratch
void VarWaitBytes(std::uint16_t address, std::uint16_t low, std::uint8_t scratch,
                  std::vector<std::uint8_t>& out);

} // namespace tisple
