#pragma once

// The 6502 backend: typed program in, BRUN-able binary out.
//
// Memory map (all from the one Layout the interpreter also runs on):
//   origin ($6000):  startup, every function, runtime routines, jump tables, zero-page image
//   then:            frames that did not fit in zero page (overlapped like the zero-page ones)
//   then:            globals and constant tables, zero where there is no initial value
//   then:            256 bytes where the startup saves zero page (not in the file)
// Everything up to the save buffer is the file; it all stays below `limit` ($9600).
//
// Zero page: the startup saves $00-$FF, the program owns all of it, and the exit restores
// it. Every dereferenced pointer lives there, then the hottest scalars and frames.
//
// Calls: every function has a static frame (params, locals, temporaries). Frames of
// functions that can never be live at once share bytes (call graph). Arguments are written
// into the callee's frame, then JSR. Results: 8-bit in A, 16-bit in A (low) and X (high).

#include "asm/asm.hpp"
#include "check/program.hpp"
#include "link/layout.hpp"

#include <string>
#include <vector>

namespace tisple {

struct BuildOptions {
    std::uint16_t origin = 0x6000;
    std::uint32_t limit = 0x9600; // one past the last byte code and data may use
    // keep the code of functions main never calls (tests call them by address)
    bool keep_all_functions = false;
};

// where one function's code is in Compiled::as.items
struct FunctionSpan {
    std::int32_t function = -1; // -1: startup or runtime
    std::string name;
    std::size_t first = 0;
    std::size_t end = 0;
};

struct Compiled {
    std::uint16_t origin = 0x6000;
    std::vector<std::uint8_t> image; // the file: bytes from origin
    Layout layout;                   // every var's address; run the interpreter on it too
    Assembly as;                     // items with addresses, for listings
    std::vector<FunctionSpan> spans;
    std::vector<std::uint16_t> function_address; // by function id; 0: dropped (never called)
    std::uint16_t entry_ready = 0; // zero page saved and initialised; next is JSR main
    std::uint16_t after_main = 0;  // main returned; zero page not restored yet
    std::uint16_t save_buffer = 0; // where zero page is saved
    std::int32_t main = -1;
    std::uint32_t code_bytes = 0; // startup, functions, runtime, tables
    std::uint32_t data_bytes = 0; // frames in memory, globals, constants
    std::uint32_t zp_bytes = 0;   // zero-page bytes the program uses
    std::uint32_t zp_frames = 0;  // of those, the overlapped frames
    std::uint32_t mem_frames = 0; // overlapped frame bytes outside zero page
};

bool CompileProgram(const Program& program, const BuildOptions& options, Compiled& out,
                    std::vector<Diagnostic>& errors);

// every function's code under its source lines, with byte counts (tisple build --asm)
std::string ListProgram(const Program& program, const Compiled& compiled);

} // namespace tisple
