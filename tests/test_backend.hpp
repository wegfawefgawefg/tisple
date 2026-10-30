#pragma once

#include "backend/backend.hpp"
#include "backend/sim_run.hpp"
#include "check.hpp"
#include "interp/interp.hpp"
#include "test_check.hpp"

#include <string>
#include <vector>

namespace tisple_tests {

// a checked program compiled by the backend; the interpreter runs on the same layout
struct Compiled {
    Built built;
    tisple::Compiled code;
    bool ok = false;
    std::string errors;
};

Compiled CompileText(const std::string& text, const std::vector<tisple::VirtualFile>& extra = {});
// the same with these mid-level pass options
Compiled CompileText(const std::string& text, const tisple::OptOptions& opt);
Compiled CompileFile(const std::string& path);
// as `tisple build` compiles it: functions main never calls are left out, so only main can
// be called (the others above keep every function, for calls by address)
Compiled CompileMainText(const std::string& text);
// the same with these mid-level pass options (-O0, --opt-bisect N, --no-pass NAME)
Compiled CompileMainText(const std::string& text, const tisple::OptOptions& opt);

// the interpreter and the simulator side by side, calls made on both
struct Diff {
    const Compiled* c = nullptr;
    tisple::Interp in;
    std::vector<tisple::IoAccess> interp_io;
    tisple::SimRunner sim;
    std::uint64_t max_cycles = 50000000;
    std::uint64_t last_cycles = 0;
    std::string message; // the first difference
};

bool StartDiff(Diff& d, const Compiled& c);
// calls on both, then compares the return value, every global byte and the I/O accesses
bool DiffCall(Diff& d, const std::string& function, const std::vector<std::uint16_t>& args);
// the same, checked: a failure prints the program
void CheckDiff(TestResults& results, Diff& d, const std::string& function,
               const std::vector<std::uint16_t>& args);
// compile, start and call one function; the text is printed on any failure
void CheckProgram(TestResults& results, const std::string& text, const std::string& function,
                  const std::vector<std::uint16_t>& args);

// instructions of one mnemonic in a function's code
int CountMn(const Compiled& c, const std::string& function, tisple::Mn mn);

// compares every global byte; false with a message
bool SameGlobals(const Diff& d, std::string& message);

// a program the backend refuses, with an error that has part in it (test_link_regions.cpp)
void ExpectLinkError(TestResults& results, const std::string& text, const std::string& part);
// every segment of c (its main file too) clear of zero page to text page 1, I/O, the other
// segments in its memory and the :reserve lets (test_link_regions.cpp)
void CheckSegmentsApart(TestResults& results, const Compiled& c);
void RunLinkRegionTests(TestResults& results);
// lib/text80, lib/text40 and lib/auxmem on the machine (test_text_libs.cpp)
void RunTextLibTests(TestResults& results);
// lib/mockingboard with the emulated card (test_mockingboard_lib.cpp)
void RunMockingboardLibTests(TestResults& results);
// programs/text_demo run, then booted from its disk (test_text_demo.cpp)
void RunTextDemoTests(TestResults& results);

void RunAsmTests(TestResults& results);
// exact delays on the simulator (test_wait.cpp)
void RunWaitTests(TestResults& results);
void RunBackendTests(TestResults& results);
// const data on the 6502 (test_backend_data.cpp)
void RunBackendDataTests(TestResults& results);
void RunBackendOpTests(TestResults& results);
void RunBackendProgramTests(TestResults& results);
void RunBackendIndexTests(TestResults& results);
void RunBackendFuzzTests(TestResults& results);
void RunBackendDivTests(TestResults& results);
// division by variables and signed constants (test_backend_div_var.cpp)
void RunBackendDivVarTests(TestResults& results);
void RunBackendCarryTests(TestResults& results);
void RunBackendReduceTests(TestResults& results);
void RunBackendWideTests(TestResults& results);
void RunBackendPeepTests(TestResults& results);
void RunBackendBindTests(TestResults& results);
void RunBackendBuildTests(TestResults& results);
// functions called in one place (test_backend_inline.cpp)
void RunBackendInlineTests(TestResults& results);
// match dispatch (test_backend_match.cpp)
void RunBackendMatchTests(TestResults& results);
// shifts (test_backend_shift.cpp)
void RunBackendShiftTests(TestResults& results);
// 8-bit subtractions (test_backend_sub8.cpp)
void RunBackendSub8Tests(TestResults& results);
// signed compares of values never negative (test_backend_signed_cmp.cpp)
void RunBackendSignedCmpTests(TestResults& results);
// u16 for loops counted in a byte (test_backend_count16.cpp)
void RunBackendCount16Tests(TestResults& results);
// short loops copied with the counter a number (test_backend_unroll.cpp)
void RunBackendUnrollTests(TestResults& results);
// the code size budget (test_backend_size.cpp)
void RunBackendSizeTests(TestResults& results);
// I/O bases, forwarded parameters, the IRQ entry's registers (test_backend_io.cpp)
void RunBackendIoTests(TestResults& results);
// 16-bit runs a byte at a time (test_backend_spine.cpp)
void RunBackendSpineTests(TestResults& results);
// store indices into their registers early (test_backend_preload.cpp)
void RunBackendPreloadTests(TestResults& results);
// byte arrays indexed by a sum (test_backend_place_sum.cpp)
void RunBackendPlaceSumTests(TestResults& results);
// compares with a number on the left (test_backend_number_cmp.cpp)
void RunBackendNumberCmpTests(TestResults& results);
// division operands straight into rt_a and rt_b (test_backend_rt_args.cpp)
void RunBackendRtArgsTests(TestResults& results);
// compares with 0 the flags already make (test_backend_zero_cmp.cpp)
void RunBackendZeroCmpTests(TestResults& results);
// loop pointers given back after their loops (test_backend_loop_ptrs.cpp)
void RunBackendLoopPtrsTests(TestResults& results);
void RunBackendFoldTests(TestResults& results);
void RunBackendTableTests(TestResults& results);
void RunBackendJsrTests(TestResults& results);
// calls through function pointers (test_backend_fnptr.cpp)
void RunBackendFnPtrTests(TestResults& results);
// interrupt handlers (test_backend_irq.cpp)
void RunBackendIrqTests(TestResults& results);
void RunBackendWaitTests(TestResults& results);
void RunBackendStringTests(TestResults& results);
void RunBackendWhileTests(TestResults& results);
void RunBackendMulTests(TestResults& results);
void RunBackendBytewiseTests(TestResults& results);
void RunBackendStepsTests(TestResults& results);
// inlined copies' locals sharing bytes (test_backend_frames.cpp)
void RunBackendFrameTests(TestResults& results);

} // namespace tisple_tests
