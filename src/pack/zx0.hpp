#pragma once

// ZX0, Einar Saukas's format (version 2: forward, the offset's high part inverted), for
// packed tables: (pack bytes) in macro code, lib/tables' packed, lib/unpack's 6502 code.
//
// - The stream is literal runs, matches at a new offset and matches at the last offset, told
//   apart by single bits; lengths and the offset's high part are interlaced Elias gamma codes,
//   the offset's low 7 bits a byte. Offsets reach 32640 back. docs/dhgr.md, Packed data.
// - PackZx0 is a near-optimal parse, not the reference compressor's exhaustive one: a forward
//   pass that keeps a few cheapest ways to reach each byte (each with its last offset), fed by
//   hash chains at four match lengths. About 3% bigger than the reference on double hi-res
//   pictures, in under 1 ms a KB.
// - UnpackZx0 (zx0_unpack.cpp) is the plain decoder, for tests and checks; it never reads or
//   writes outside.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tisple {

// the packed stream; empty for empty data, which ZX0 cannot hold (its first step is a literal)
std::vector<std::uint8_t> PackZx0(const std::vector<std::uint8_t>& data);

// out is the unpacked bytes; false when packed is not a whole stream: cut short, a match from
// before the start, more than max_out bytes, or bytes after the end marker
bool UnpackZx0(const std::vector<std::uint8_t>& packed, std::size_t max_out,
               std::vector<std::uint8_t>& out);

} // namespace tisple
