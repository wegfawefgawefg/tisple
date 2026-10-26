#pragma once

// The superoptimizer's 6502: the documented ALU, load/store, shift, increment, transfer and
// flag instructions over a window's state, binary mode only (D clear), fast enough to run
// millions of candidate sequences. Memory is a handful of cells:
// - direct cells, $nn or $nnnn, at their address;
// - pseudo-cells, an indexed or indirect operand like ($10),Y or $7600,X taken as one fixed
//   unknown byte. That is only right while its index register still holds its entry value
//   (a sequence that changes Y and then reads ($10),Y reads some other byte: the state is
//   marked bad), and it assumes pseudo-cells do not alias each other or a direct cell.
// The test file checks this against the cycle-exact core in src/emu on every opcode.

#include "asm/opcodes.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tisple {

constexpr int kMaxCells = 8;

// registers and flags as bits of one mask (liveness, reads, writes)
constexpr std::uint8_t kLocA = 0x01;
constexpr std::uint8_t kLocX = 0x02;
constexpr std::uint8_t kLocY = 0x04;
constexpr std::uint8_t kLocN = 0x08;
constexpr std::uint8_t kLocZ = 0x10;
constexpr std::uint8_t kLocC = 0x20;
constexpr std::uint8_t kLocV = 0x40;
constexpr std::uint8_t kLocRegs = kLocA | kLocX | kLocY;
constexpr std::uint8_t kLocFlags = kLocN | kLocZ | kLocC | kLocV;

enum class CellIndex : std::uint8_t {
    None,
    X, // zp,X abs,X (zp,X)
    Y, // zp,Y abs,Y (zp),Y
};

struct Cell {
    Mode mode = Mode::Zp; // Zp or Abs for direct cells; the operand's mode for pseudo-cells
    std::uint16_t operand = 0;
    CellIndex index = CellIndex::None;
    bool writable = false; // candidates may write it (the window does)
    bool live = true;      // must hold the right value at the end
};

struct SoIns {
    Mn mn = Mn::NOP;
    Mode mode = Mode::Imp;
    std::uint8_t imm = 0;
    std::int8_t cell = -1; // memory operand, into the window's cells
};

struct SoState {
    std::uint8_t a = 0;
    std::uint8_t x = 0;
    std::uint8_t y = 0;
    std::uint8_t p = 0; // N V Z C in their status-register bits
    std::uint8_t cells[kMaxCells] = {};
    std::uint8_t x0 = 0; // entry X and Y: pseudo-cells are valid while these still hold
    std::uint8_t y0 = 0;
    std::uint8_t bad = 0; // a pseudo-cell was used with its index changed
    std::uint8_t pad = 0;
};

static_assert(sizeof(SoState) == 16);

bool IsDirectCell(const Cell& cell);
// can the evaluator run this mnemonic at all
bool IsModelled(Mn mn);
// reads and writes of registers and flags (kLoc masks), and of memory
struct LocEffects {
    std::uint8_t reads = 0;
    std::uint8_t writes = 0;
    bool mem_read = false;
    bool mem_write = false;
};
LocEffects EffectsOf(Mn mn, Mode mode);

void Exec(const SoIns& ins, const Cell* cells, SoState& s);
void ExecAll(const std::vector<SoIns>& code, const Cell* cells, SoState& s);

int InsCycles(const SoIns& ins);
int InsBytes(const SoIns& ins);
// "ADC ($10),Y", "LDA #$00", "STA $39"
std::string InsText(const SoIns& ins, const Cell* cells);
std::string CodeText(const std::vector<SoIns>& code, const Cell* cells);

} // namespace tisple
