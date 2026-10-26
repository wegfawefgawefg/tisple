#include "test_check.hpp"

#include <cstdio>

namespace tisple_tests {

namespace {

Built Finish(Built built) {
    if (built.ok) {
        built.ok = tisple::LayoutProgram(built.program, tisple::LayoutOptions(), built.layout,
                                         built.errors);
    }
    return built;
}

} // namespace

Built BuildText(const std::string& text, const std::vector<tisple::VirtualFile>& extra) {
    tisple::LoadOptions options;
    options.lib_dir = "lib0";
    options.files.push_back(tisple::VirtualFile{"t.tsp", text});
    for (const tisple::VirtualFile& file : extra) {
        options.files.push_back(file);
    }
    Built built;
    built.ok = tisple::BuildProgram("t.tsp", options, built.program, built.errors);
    return Finish(std::move(built));
}

Built BuildFile(const std::string& path) {
    Built built;
    built.ok = tisple::BuildProgram(std::string(TISPLE_SOURCE_DIR) + "/" + path,
                                    tisple::LoadOptions(), built.program, built.errors);
    return Finish(std::move(built));
}

std::string ErrorText(const Built& built) {
    return tisple::FormatDiagnostics(built.program.sources, built.errors);
}

std::string FirstError(const Built& built) {
    if (built.errors.empty()) {
        return "";
    }
    return tisple::FormatDiagnostic(built.program.sources, built.errors[0]);
}

void ExpectError(TestResults& results, const std::string& text, const std::string& expected,
                 const std::vector<tisple::VirtualFile>& extra) {
    Built built = BuildText(text, extra);
    std::string got = FirstError(built);
    bool ok = !built.ok && got == expected;
    if (!ok) {
        std::fprintf(stderr, "program: %s\n  want: %s\n  got:  %s\n", text.c_str(),
                     expected.c_str(), got.c_str());
    }
    CHECK(results, ok);
}

void StartInterp(tisple::Interp& in, const Built& built) {
    tisple::InitInterp(in, built.program, built.layout);
}

std::uint16_t CallOk(TestResults& results, tisple::Interp& in, const std::string& function,
                     const std::vector<std::uint16_t>& args) {
    std::int32_t id = tisple::FindFunction(*in.program, function);
    CHECK(results, id >= 0);
    if (id < 0) {
        std::fprintf(stderr, "no function %s\n", function.c_str());
        return 0;
    }
    tisple::InterpResult result = tisple::CallFunction(in, id, args);
    bool finished = result.kind == tisple::StopKind::Finished;
    if (!finished) {
        std::fprintf(stderr, "%s stopped: %s\n", function.c_str(), result.message.c_str());
    }
    CHECK(results, finished);
    return result.value;
}

} // namespace tisple_tests
