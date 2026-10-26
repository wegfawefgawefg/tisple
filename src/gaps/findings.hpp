#pragma once

// Turning a profile into findings: instructions whose redundancy condition held on (by
// default) every one of a reasonable number of executions, with the cycles removing them
// would save. A finding is a hint that some fact would make the instruction redundant, not
// a proof that it is.

#include "backend/backend.hpp"
#include "gaps/profile.hpp"
#include "gaps/sites.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

enum class Pattern : std::uint8_t {
    kFlagAlready,     // CLC/SEC/CLV/...: the flag was already in that state
    kLoadAlready,     // LDA/LDX/LDY: the register already held the value
    kTransferAlready, // TAX/TAY/TXA/TYA/...: the target already held it
    kStoreAlready,    // memory already held the value
    kStoreDead,       // overwritten before anything read it
    kCompareOneWay,   // CMP/CPX/CPY/BIT then a branch that always went the same way
    kBranchOneWay,    // another branch that always went the same way
    kFlagsUnread,     // CMP/CPX/CPY/BIT/CLC/SEC/CLV: nothing read the flags it set
    kStrided,         // a stored byte stepped by the same amount each time: strength reduction
    kCount,
};

const char* PatternName(Pattern pattern);

struct Finding {
    Pattern pattern = Pattern::kFlagAlready;
    std::uint16_t address = 0;
    std::uint64_t count = 0;  // executions of the instruction
    std::uint64_t held = 0;   // on how many the condition held (of `out_of`)
    std::uint64_t out_of = 0; // executions, or ended stores / flag writes for those patterns
    std::uint64_t saved = 0;  // cycles removing it would save in this run
    bool estimate = false;    // saved is a rough estimate (strided values)
    std::string reason;       // "carry already clear", "branch never taken", ...
    std::string key;          // for grouping across programs: pattern and instruction shapes
    std::string function;
    std::string source;
    std::string text; // the instruction
};

struct FindOptions {
    std::uint64_t min_count = 16; // executions (or ended events) before a site is judged
    double min_fraction = 1.0;    // of them, how many must show the condition
    double strided_fraction = 0.9;
};

std::vector<Finding> FindGaps(const Compiled& compiled, const SiteMap& map, const Profiler& p,
                              const FindOptions& options);

// the cycles all findings could save, counting each address once (its largest saving)
std::uint64_t TotalSaved(const std::vector<Finding>& findings);

} // namespace tisple
