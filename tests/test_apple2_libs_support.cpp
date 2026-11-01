// what the round 1 library tests share (test_apple2_libs.hpp)

#include "test_apple2_libs.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>

namespace tisple_tests {

bool BuildLibProgram(TestResults& results, const std::string& text,
                     const tisple::BuildOptions& options, LibBuild& b) {
    tisple::LoadOptions load;
    load.files.push_back(tisple::VirtualFile{"t.tsp", text});
    std::vector<tisple::Diagnostic> errors;
    b.ok = tisple::BuildProgram("t.tsp", load, b.program, errors) &&
           tisple::CompileProgram(b.program, options, b.code, errors);
    CHECK(results, b.ok);
    if (!b.ok) {
        std::fprintf(stderr, "%s", tisple::FormatDiagnostics(b.program.sources, errors).c_str());
    }
    return b.ok;
}

std::unique_ptr<tisple::Apple2> LoadLibProgram(const LibBuild& b) {
    std::unique_ptr<tisple::Apple2> a = tisple::NewApple2();
    std::string error;
    for (const tisple::MemorySegment& s : b.code.segments) {
        tisple::LoadSegment(*a, s, error);
    }
    tisple::LoadProgram(*a, b.code.image.data(), b.code.image.size(), b.code.origin, error);
    return a;
}

bool RunToReturn(TestResults& results, tisple::Apple2& a, uint64_t cycles) {
    tisple::RunApple2(a, cycles);
    bool returned = a.stop_reason == tisple::MachineStop::kReturned;
    CHECK(results, returned);
    if (!returned) {
        std::fprintf(stderr, "stopped: %s\n", tisple::DescribeStop(a).c_str());
    }
    return returned;
}

bool ReadRomFile(const char* name, std::vector<uint8_t>& out) {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return false;
    }
    std::ifstream in(std::string(home) + "/.local/share/tisple/roms/" + name, std::ios::binary);
    if (!in) {
        return false;
    }
    out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return !out.empty();
}

} // namespace tisple_tests
