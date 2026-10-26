#include "check.hpp"
#include "source_file.hpp"
#include "test_backend.hpp"
#include "test_check.hpp"
#include "test_emu.hpp"
#include "test_gaps.hpp"
#include "test_ide.hpp"
#include "test_machine.hpp"
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
    tisple_tests::RunMachineTests(results);
    tisple_tests::RunReaderTests(results);
    tisple_tests::RunFormatTests(results);
    tisple_tests::RunShowTests(results);
    tisple_tests::RunCheckExampleTests(results);
    tisple_tests::RunCheckNameTests(results);
    tisple_tests::RunCheckTypeTests(results);
    tisple_tests::RunCheckFormTests(results);
    tisple_tests::RunCheckDecisionTests(results);
    tisple_tests::RunExpandTests(results);
    tisple_tests::RunIndexTests(results);
    tisple_tests::RunLayoutTests(results);
    tisple_tests::RunInterpTests(results);
    tisple_tests::RunInterpArithTests(results);
    tisple_tests::RunAsmTests(results);
    tisple_tests::RunBackendTests(results);
    tisple_tests::RunBackendOpTests(results);
    tisple_tests::RunBackendIndexTests(results);
    tisple_tests::RunBackendDivTests(results);
    tisple_tests::RunBackendProgramTests(results);
    tisple_tests::RunBackendFuzzTests(results);
    tisple_tests::RunGapsTests(results);
    tisple_tests::RunSuperoptTests(results);

    std::printf("%d passed, %d failed\n", results.passed, results.failed);
    return results.failed == 0 ? 0 : 1;
}
