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
// A program that does not fit goes on (link.cpp): functions that do not fit move, whole, to
// the memory below the origin (from low_start, around text page 1 and every :address and
// :reserve let), and data follows them; then the language card's 12 KB at $D000-$FFFF, when
// no constant address there needs the rom (link.hpp; the startup switches its ram in).
// Lets :in aux go in auxiliary memory. Each piece outside the main file is a segment.
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
#include "link/segment.hpp"
#include "opt/opt.hpp"

#include <string>
#include <vector>

namespace tisple {

struct BuildOptions {
    std::uint16_t origin = 0x6000;
    std::uint32_t limit = 0x9600;     // one past the last byte code and data may use
    std::uint32_t low_start = 0x0800; // the memory below the origin, from here, when needed
    bool use_card = true;             // the language card's ram, when needed and allowed
    // keep the code of functions main never calls (tests call them by address)
    bool keep_all_functions = false;
    // functions never inlined, by qualified name (the race times from t.init's return to
    // t.checksum's entry)
    std::vector<std::string> no_inline;
    OptOptions opt; // the mid-level passes (opt/opt.hpp)
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
    // the rest of a program that did not fit: below the origin, the language card, aux memory
    std::vector<MemorySegment> segments;
    bool card_ram = false; // the startup switches the language card's ram in
    Layout layout;         // every var's address; run the interpreter on it too
    Assembly as;           // items with addresses, for listings
    std::vector<FunctionSpan> spans;
    std::vector<std::uint16_t> function_address; // by function id; 0: dropped (never called)
    std::uint16_t entry_ready = 0; // zero page saved and initialised; next is JSR main
    std::uint16_t after_main = 0;  // main returned; zero page not restored yet
    std::uint16_t save_buffer = 0; // where zero page is saved
    // the interrupt handler's entries (irq.hpp): $3FE's, and the cpu's $FFFE in the card's ram;
    // 0 without a handler
    std::uint16_t irq_entry = 0;
    std::uint16_t irq_direct = 0;
    std::int32_t main = -1;
    std::uint32_t code_bytes = 0; // startup, functions, runtime, tables
    std::uint32_t data_bytes = 0; // frames in memory, globals, constants
    std::uint32_t zp_bytes = 0;   // zero-page bytes the program uses
    std::uint32_t zp_frames = 0;  // of those, the overlapped frames
    std::uint32_t mem_frames = 0; // overlapped frame bytes outside zero page
    OptReport opt;                // what the mid-level passes did
};

bool CompileProgram(const Program& program, const BuildOptions& options, Compiled& out,
                    std::vector<Diagnostic>& errors);

// every function's code under its source lines, with byte counts (tisple build --asm)
std::string ListProgram(const Program& program, const Compiled& compiled);

// a byte of the built program as the cpu sees it with the program's memory switched in:
// the file or a main or language card segment; 0 outside them
std::uint8_t CompiledByte(const Compiled& compiled, std::uint16_t address);

// every piece of the program: the file at its origin first, then the other segments
std::vector<MemorySegment> ProgramSegments(const Compiled& compiled);

// "4120 bytes at $6000 (code ..." for the file and a line for each other segment
std::string DescribeSegments(const Compiled& compiled);

} // namespace tisple
