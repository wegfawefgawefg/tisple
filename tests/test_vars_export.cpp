// game.vars.json (disk/vars_export.hpp): a program sets its globals and keeps running; each one
// read back on the machine at the address the export gives (as the phone player's pad reads
// them, PeekMain whatever the switches say) holds what was set: a u8, an enum and its members,
// a u16, an i8, an array of u8 and an array of structs (split: a plane per field), in zero
// page and out of it

#include "disk/vars_export.hpp"
#include "test_backend.hpp"
#include "test_machine.hpp"

#include <cstdio>
#include <cstring>

namespace tisple_tests {

namespace {

const char* kProgram = R"(
(enum Mode (a b c))
(struct P ((x u8) (y u16)))
(let m Mode)
(let n u8)
(let w u16)
(let s i8)
(let arr (array u8 4))
(let ps (array P 3))
(fn main () void
    ; values the passes cannot know (no key is pressed), so they stay in memory
    (set m (if (= (io_read u8 0xC000) 0xFE)
        Mode.a
        Mode.c))
    (set n 77)
    (set w 0x1234)
    (set s (if (= (io_read u8 0xC000) 0xFE)
        3
        (- 0 5)))
    (for i u8 0 4
        (set (at arr i) (+ i 10)))
    (for i u8 0 3
        (set (at ps i x) (+ i 20))
        (set (at ps i y) (+ 0x0100 (as u16 i))))
    ; reads them all (no key is pressed), so their stores stay
    (while true
        (if (= (io_read u8 0xC000) 0xFF)
            (do
                (let k u8 (& (io_read u8 0xC061) 3))
                (let j u8 (if (= k 3)
                    0
                    k))
                (set n (+ (+ n (as u8 m)) (+ (as u8 s) (at arr k))))
                (set w (+ w (+ (as u16 (at ps j x)) (at ps j y))))))))
)";

// the number after "field": in name's entry, or -1
long Field(const std::string& json, const std::string& name, const char* field) {
    std::size_t at = json.find("\"" + name + "\": {");
    if (at == std::string::npos) {
        return -1;
    }
    std::size_t end = json.find('}', at);
    std::size_t f = json.find(std::string("\"") + field + "\": ", at);
    if (f == std::string::npos || f > end) {
        return -1;
    }
    return std::strtol(json.c_str() + f + std::strlen(field) + 4, nullptr, 10);
}

// element i's byte k (k 1: a u16's high byte) of name, as the pad reads it
int Read(const tisple::Apple2& a, const std::string& json, const std::string& name, long i,
         int k = 0) {
    long at = Field(json, name, "at");
    long step = Field(json, name, "step");
    long high = Field(json, name, "high");
    if (at < 0) {
        return -1;
    }
    long address = at + i * (step < 0 ? 0 : step) + (k == 1 ? high : 0);
    return tisple::PeekMain(a, static_cast<std::uint16_t>(address));
}

} // namespace

void RunVarsExportTests(TestResults& results) {
    tisple::LoadOptions load;
    load.files.push_back(tisple::VirtualFile{"t.tsp", kProgram});
    tisple::Program program;
    tisple::Compiled code;
    std::vector<tisple::Diagnostic> errors;
    bool built = tisple::BuildProgram("t.tsp", load, program, errors) &&
                 tisple::CompileProgram(program, tisple::BuildOptions(), code, errors);
    CHECK(results, built);
    if (!built) {
        std::fprintf(stderr, "%s", tisple::FormatDiagnostics(program.sources, errors).c_str());
        return;
    }
    std::string json = tisple::DescribeVarsJson(program, code, 1234);
    std::string mod = program.modules[static_cast<std::size_t>(program.entry_module)].name + ".";

    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    std::string error;
    for (const tisple::MemorySegment& s : code.segments) {
        CHECK(results, tisple::LoadSegment(*a, s, error));
    }
    CHECK(results,
          tisple::LoadProgram(*a, code.image.data(), code.image.size(), code.origin, error));
    tisple::RunApple2(*a, 200000);
    CHECK(results, !a->stopped);

    CHECK(results, json.find("\"disk_fnv\": 1234") != std::string::npos);
    CHECK(results, json.find("\"" + mod + "Mode.c\": 2") != std::string::npos);
    CHECK(results, Read(*a, json, mod + "m", 0) == 2);
    CHECK(results, Read(*a, json, mod + "n", 0) == 77);
    CHECK(results, Field(json, mod + "w", "bytes") == 2);
    CHECK(results, Read(*a, json, mod + "w", 0) == 0x34 && Read(*a, json, mod + "w", 0, 1) == 0x12);
    CHECK(results, json.find("\"" + mod + "s\": {") != std::string::npos);
    CHECK(results, Read(*a, json, mod + "s", 0) == 0xFB);
    CHECK(results, Field(json, mod + "arr", "count") == 4);
    for (long i = 0; i < 4; i++) {
        CHECK(results, Read(*a, json, mod + "arr", i) == 10 + i);
    }
    CHECK(results, Field(json, mod + "ps.x", "count") == 3);
    for (long i = 0; i < 3; i++) {
        CHECK(results, Read(*a, json, mod + "ps.x", i) == 20 + i);
        CHECK(results,
              Read(*a, json, mod + "ps.y", i) == i && Read(*a, json, mod + "ps.y", i, 1) == 1);
    }
    // the program's first bytes, to know it is loaded
    CHECK(results,
          json.find("\"check\": {\"at\": " + std::to_string(code.origin)) != std::string::npos);
    CHECK(results, tisple::VarsJsonPath("build/game.dsk") == "build/game.vars.json");
}

} // namespace tisple_tests
