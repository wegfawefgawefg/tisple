#pragma once

// The program's globals and enums as JSON, written beside a built disk (game.vars.json for
// game.dsk), so a player can read the running program's state: the phone player's pad shows
// a button only while the game's state says it does something (web/pad.js "when", docs/phone.md).
//
//   {"disk_fnv": N,                      FNV-1a 32 of the disk file the build wrote
//    "check": {"at": A, "bytes": [...]}, the program's first bytes in main memory: it is loaded
//    "vars": {"state.phase": {"at": 6, "bytes": 1},
//             "state.played": {"at": 1750, "bytes": 1, "count": 6, "step": 1},
//             "state.actors.alive": {...},  an array of structs: each scalar field
//             "state.ticks": {"at": 1762, "bytes": 2, "high": 1}, "signed": true, "aux": true},
//    "enums": {"state.Phase.player": 1}}
//
// Only top-level lets (not const tables) with an address; an element's byte k is at
// at + i * step (+ high for a u16's high byte). "aux": in auxiliary memory. $D000-$FFFF is the
// language card's ram with bank 2 at $D000.

#include "backend/backend.hpp"
#include "check/program.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

std::uint32_t Fnv1a32(const std::vector<std::uint8_t>& bytes);

std::string DescribeVarsJson(const Program& program, const Compiled& compiled,
                             std::uint32_t disk_fnv);

// game.dsk's game.vars.json
std::string VarsJsonPath(const std::string& disk_path);

} // namespace tisple
