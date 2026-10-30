#include "check.hpp"
#include "source_file.hpp"
#include "test_backend.hpp"
#include "test_check.hpp"
#include "test_disk.hpp"
#include "test_emu.hpp"
#include "test_gaps.hpp"
#include "test_ide.hpp"
#include "test_machine.hpp"
#include "test_opt.hpp"
#include "test_project.hpp"
#include "test_race.hpp"
#include "test_reader.hpp"

#include <cstdio>
#include <cstdlib>

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

    // TISPLE_FUZZ_FILE diffs one program and runs nothing else, so a reducer can call it often
    const char* fuzz_file = std::getenv("TISPLE_FUZZ_FILE");
    if (fuzz_file && *fuzz_file) {
        tisple_tests::RunBackendFuzzTests(results);
        return results.failed == 0 ? 0 : 1;
    }

    // one line per test group
    tisple_tests::RunSourceFileTests(results);
    tisple_tests::RunEmuTests(results);
    tisple_tests::RunMachineTests(results);
    tisple_tests::RunDiskTests(results);
    tisple_tests::RunReaderTests(results);
    tisple_tests::RunFormatTests(results);
    tisple_tests::RunShowTests(results);
    tisple_tests::RunCheckExampleTests(results);
    tisple_tests::RunCheckNameTests(results);
    tisple_tests::RunCheckTypeTests(results);
    tisple_tests::RunCheckFormTests(results);
    tisple_tests::RunCheckDecisionTests(results);
    tisple_tests::RunExpandTests(results);
    tisple_tests::RunMacroTests(results);
    tisple_tests::RunIndexTests(results);
    tisple_tests::RunProjectTests(results);
    tisple_tests::RunLayoutTests(results);
    tisple_tests::RunCheckDataTests(results);
    tisple_tests::RunInterpTests(results);
    tisple_tests::RunInterpArithTests(results);
    tisple_tests::RunAsmTests(results);
    tisple_tests::RunWaitTests(results);
    tisple_tests::RunBackendTests(results);
    tisple_tests::RunBackendOpTests(results);
    tisple_tests::RunBackendIndexTests(results);
    tisple_tests::RunBackendDivTests(results);
    tisple_tests::RunBackendDivVarTests(results);
    tisple_tests::RunBackendCarryTests(results);
    tisple_tests::RunBackendReduceTests(results);
    tisple_tests::RunBackendWideTests(results);
    tisple_tests::RunBackendPeepTests(results);
    tisple_tests::RunBackendBindTests(results);
    tisple_tests::RunBackendBuildTests(results);
    tisple_tests::RunBackendInlineTests(results);
    tisple_tests::RunBackendMatchTests(results);
    tisple_tests::RunBackendShiftTests(results);
    tisple_tests::RunBackendSub8Tests(results);
    tisple_tests::RunBackendSignedCmpTests(results);
    tisple_tests::RunBackendCount16Tests(results);
    tisple_tests::RunBackendUnrollTests(results);
    tisple_tests::RunBackendSizeTests(results);
    tisple_tests::RunBackendSqueezeTests(results);
    tisple_tests::RunBackendIoTests(results);
    tisple_tests::RunBackendSpineTests(results);
    tisple_tests::RunBackendPreloadTests(results);
    tisple_tests::RunBackendPlaceSumTests(results);
    tisple_tests::RunBackendNumberCmpTests(results);
    tisple_tests::RunBackendRtArgsTests(results);
    tisple_tests::RunBackendZeroCmpTests(results);
    tisple_tests::RunBackendLoopPtrsTests(results);
    tisple_tests::RunBackendFoldTests(results);
    tisple_tests::RunBackendTableTests(results);
    tisple_tests::RunBackendJsrTests(results);
    tisple_tests::RunBackendFnPtrTests(results);
    tisple_tests::RunBackendIrqTests(results);
    tisple_tests::RunBackendWaitTests(results);
    tisple_tests::RunBackendStringTests(results);
    tisple_tests::RunBackendDataTests(results);
    tisple_tests::RunBackendWhileTests(results);
    tisple_tests::RunBackendMulTests(results);
    tisple_tests::RunBackendBytewiseTests(results);
    tisple_tests::RunBackendStepsTests(results);
    tisple_tests::RunBackendFrameTests(results);
    tisple_tests::RunBackendProgramTests(results);
    tisple_tests::RunOptTests(results);
    tisple_tests::RunLinkRegionTests(results);
    tisple_tests::RunTextLibTests(results);
    tisple_tests::RunMockingboardLibTests(results);
    tisple_tests::RunMockingboardLineTests(results);
    tisple_tests::RunMockingboardAuxTests(results);
    tisple_tests::RunTextDemoTests(results);
    tisple_tests::RunBackendFuzzTests(results);
    tisple_tests::RunGapsTests(results);
    tisple_tests::RunSuperoptTests(results);
    tisple_tests::RunRaceTests(results);
    tisple_tests::RunRaceCTests(results);

    std::printf("%d passed, %d failed\n", results.passed, results.failed);
    return results.failed == 0 ? 0 : 1;
}
