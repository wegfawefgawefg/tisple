#include "check.hpp"
#include "source_file.hpp"
#include "test_emu.hpp"
#include "test_reader.hpp"

#include <cstdio>

namespace tisple_tests {

void RunSourceFileTests(TestResults& results) {
    tisple::SourceFile file = tisple::SourceFileFromText("t.tsp", "ab\ncd\n");
    CHECK(results, tisple::PosFromOffset(file, 0).line == 1);
    CHECK(results, tisple::PosFromOffset(file, 3).line == 2);
    CHECK(results, tisple::PosFromOffset(file, 4).column == 2);
}

} // namespace tisple_tests

int main() {
    tisple_tests::TestResults results;

    // one line per test group
    tisple_tests::RunSourceFileTests(results);
    tisple_tests::RunEmuTests(results);
    tisple_tests::RunReaderTests(results);
    tisple_tests::RunFormatTests(results);
    tisple_tests::RunShowTests(results);

    std::printf("%d passed, %d failed\n", results.passed, results.failed);
    return results.failed == 0 ? 0 : 1;
}
