#pragma once

// an unenhanced apple iie (nmos 6502, 64 KB main ram, language card) around our
// cycle-exact cpu wrapper. two starts:
// - direct load: no rom, a raw binary copied into ram and started at its origin. BRK, the
//   entry's final RTS, or a jump into rom space stops the machine.
// - real boot: a rom image the user supplies (never committed), reset through $FFFC.
//
// the whole machine is plain data: no heap, no pointers that survive a copy (the cpu's
// io hook is re-aimed on load), so a snapshot is a struct copy.
//
// timing model, shared by video, $C019 and the floating bus: machine cycle 0 is the start of
// scanline 0. each line is 65 cycles: 25 of horizontal blank, then 40 visible byte fetches.
// lines 192-261 are vertical blank. 17030 cycles per frame.
//
// where this departs from, or guesses at, the real machine (to cross-check in MAME):
// - frame phase: cycle 0 is the start of line 0; a real machine powers up anywhere
// - floating bus: the byte the scanner fetches in the same cycle (AppleWin reads one cycle
//   back for its own clock model)
// - the picture: switch changes are exact to the cycle (the fetch in the switch's own
//   cycle still uses the old mode, and there is no video pipeline delay); memory writes
//   reach the picture at scanline granularity (each line is fetched at its end)
// - color: AppleWin's "color monitor ntsc" model; all-text screens show no color
// - paddles: 2816 / 255 cycles per step; a retrigger restarts the timers; an unplugged
//   joystick reads as centred (127); the cassette input reads 0
// - keys: a scripted key is held for 3 frames; no auto-repeat in headless runs
// - flash: 16 frames each way, normal first
// - ram powers up zeroed, text page 1 filled with spaces ($A0)
// - direct load starts the language card as $C082 (rom reads, writes ignored); reset
//   leaves it at bank 2, rom reads, writes enabled
// - without a rom, $D000-$FFFF and internal $Cxxx rom read as the address's high byte
// - BRK and undocumented opcodes stop a direct-loaded program (the chip would vector or
//   execute them); with a rom and no program loaded, they run as on the chip
// - no interrupts; auxiliary memory, 80 columns and double-res are stubs (noted once)
// - speaker: an ideal two-position cone, box-filtered, with a 35 Hz high-pass

#include "emu/machine.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tisple {

constexpr uint32_t kCyclesPerLine = 65;
constexpr uint32_t kLinesPerFrame = 262;
constexpr uint32_t kCyclesPerFrame = kCyclesPerLine * kLinesPerFrame; // 17030
constexpr uint32_t kVisibleLines = 192;
constexpr uint32_t kVisibleColumns = 40;
constexpr uint32_t kFirstVisibleCycle = 25;                            // within a line
constexpr uint32_t kVblankStartCycle = kVisibleLines * kCyclesPerLine; // within a frame: 12480
// 14.31818 MHz * 65 / 912: the average rate including the long 65th cycle of each line
constexpr double kCpuHz = 14318181.8 * 65.0 / 912.0;

constexpr uint16_t kDefaultOrigin = 0x6000;
// the entry's RTS returns here: we push $FFFE and stop when pc reaches $FFFF
constexpr uint16_t kStopAddress = 0xFFFF;
// a scripted key stays down this long (3 frames), so $C010 bit 7 sees it
constexpr uint64_t kScriptedKeyHoldCycles = 3 * kCyclesPerFrame;
// unconnected paddles: a centred joystick
constexpr uint8_t kPaddleCentre = 127;

constexpr uint32_t kSpeakerRingSize = 16384; // toggles kept for consumers; drain once a frame
constexpr uint32_t kMaxScheduledKeys = 1024;

// display switches and the iie memory switches. 80-column and auxiliary memory are tracked
// (status reads return them) but not built yet: main memory and 40 columns are used.
struct SoftSwitches {
    bool text = true;
    bool mixed = false;
    bool page2 = false;
    bool hires = false;
    bool an[4] = {false, false, false, true}; // an3 on: double-res off
    bool store80 = false;
    bool ramrd = false;
    bool ramwrt = false;
    bool intcxrom = false;
    bool altzp = false;
    bool slotc3rom = false;
    bool col80 = false;
    bool altchar = false;
    bool intc8rom = false; // internal $C800-$CFFF rom, set by a $C3xx access
};

// 16 KB at $D000-$FFFF: two 4 KB banks at $D000 and 8 KB at $E000
struct LanguageCard {
    bool bank2 = true;
    bool read_ram = false;
    bool write_enable = false;
    bool prewrite = false;
    uint8_t bank1_d000[0x1000] = {};
    uint8_t bank2_d000[0x1000] = {};
    uint8_t e000[0x2000] = {};
};

// an image of $C000-$FFFF; a 12 KB rom fills $D000-$FFFF only
struct Rom {
    bool present = false;
    bool has_cx = false; // $C100-$CFFF internal rom present (16 KB image)
    uint8_t bytes[0x4000] = {};
};

struct ScheduledKey {
    uint64_t cycle = 0;
    uint8_t code = 0;
};

struct Keyboard {
    uint8_t latch = 0; // bit 7 is the strobe
    bool key_down = false;
    uint64_t release_cycle = UINT64_MAX;
    ScheduledKey queue[kMaxScheduledKeys] = {};
    uint32_t queue_head = 0;
    uint32_t queue_count = 0;
};

struct GamePort {
    uint8_t paddle[4] = {kPaddleCentre, kPaddleCentre, kPaddleCentre, kPaddleCentre};
    bool button[3] = {};
    uint64_t paddle_end[4] = {}; // timer i reads bit 7 set while cycle < paddle_end[i]
};

struct Speaker {
    uint64_t toggles[kSpeakerRingSize] = {}; // cycle of toggle n at [n % size]
    uint64_t toggle_count = 0;
};

// what the scanner fetched for one frame; the renderer turns it into pixels
enum VideoMode : uint8_t {
    kVideoText = 0,
    kVideoLores = 1,
    kVideoHires = 2,
    kVideoAltChar = 0x08, // flag on text columns
};

struct VideoFrame {
    uint8_t bytes[kVisibleLines][kVisibleColumns] = {};
    uint8_t modes[kVisibleLines][kVisibleColumns] = {};
    bool burst[kVisibleLines] = {}; // color burst seen on the line
    bool flash_inverse = false;
    uint64_t frame_index = 0;
};

struct VideoScan {
    VideoFrame live;      // being drawn: lines above the beam are this frame's
    VideoFrame done;      // the last complete frame
    uint64_t scanned = 0; // fetches at cycles below this are in live
    uint64_t next_check = 0;
    uint64_t frames_done = 0;
    int32_t burst_counter = 0;
};

// slot cards: state lives here as plain data, behaviour in slots.cpp. to add a card, add a
// kind, a state struct field below, and its cases in slots.cpp.
enum class CardKind : uint8_t {
    kEmpty = 0,
    kTestCard, // registers and rom bytes the tests fill in
};

struct TestCardState {
    uint8_t regs[16] = {};
    uint8_t rom[256] = {};
    uint8_t expansion[2048] = {};
    uint32_t io_accesses = 0;
};

struct SlotCards {
    CardKind kind[8] = {};
    uint8_t expansion_slot = 0; // whose $C800 rom is selected; 0 for none
    TestCardState test_card;
};

enum class MachineStop : uint8_t {
    kRunning = 0, // the cycle budget ran out; the machine can go on
    kReturned,    // the entry's final RTS
    kBrk,
    kIllegalOpcode,
    kNoRom, // jumped into rom or slot space with nothing there
};

// stub notes: things a program used that are not built yet, reported once
enum StubNote : uint32_t {
    kNoteAuxMemory = 1u << 0,
    kNote80Columns = 1u << 1,
    kNoteDoubleRes = 1u << 2,
};

struct Apple2 {
    Machine cpu; // registers, pins, cycle counter and the 64 KB of main ram
    SoftSwitches sw;
    LanguageCard lc;
    Rom rom;
    Keyboard keys;
    GamePort game;
    Speaker speaker;
    VideoScan video;
    SlotCards slots;
    bool direct_load = false; // stop on BRK, illegal opcodes and the entry's RTS
    bool stopped = false;
    MachineStop stop_reason = MachineStop::kRunning;
    uint16_t stop_pc = 0;
    uint8_t stop_opcode = 0;
    uint32_t stub_notes = 0;
};

struct RunOutcome {
    MachineStop reason = MachineStop::kRunning;
    uint16_t pc = 0;
    uint64_t cycles = 0; // used by this call
};

// 560 x 192: one pixel per 14 MHz dot, so a hi-res pixel is two wide
constexpr int kScreenWidth = 560;
constexpr int kScreenHeight = 192;

struct Framebuffer {
    std::vector<uint32_t> pixels = std::vector<uint32_t>(kScreenWidth * kScreenHeight); // 0xRRGGBB
};

// setup
std::unique_ptr<Apple2> NewApple2(); // powered on, no rom, text page 1 cleared to spaces
void PowerOn(Apple2& a);
bool LoadRom(Apple2& a, const uint8_t* bytes, size_t size, std::string& error);
// copies a raw binary to origin and starts there with $FFFE pushed as the return address
bool LoadProgram(Apple2& a, const uint8_t* bytes, size_t size, uint16_t origin, std::string& error);
// the reset line: soft switches to their reset state, pc from $FFFC
void ResetApple2(Apple2& a);

// running
RunOutcome RunApple2(Apple2& a, uint64_t cycles);
const char* MachineStopName(MachineStop reason);
std::string DescribeStop(const Apple2& a); // "BRK at $6012", "jumped into rom at $FDED", ...
std::string DescribeStubNotes(uint32_t notes);
uint64_t Cycles(const Apple2& a);

// input
void PressKey(Apple2& a, uint8_t code); // held until ReleaseKey
void ReleaseKey(Apple2& a);
bool ScheduleKey(Apple2& a, uint64_t cycle, uint8_t code); // pressed then released
void SetPaddle(Apple2& a, int index, uint8_t value);
void SetButton(Apple2& a, int index, bool down);

// memory as the cpu would read it, without side effects
uint8_t PeekMemory(const Apple2& a, uint16_t addr);

// speaker: toggle n for n in [toggle_count - kSpeakerRingSize, toggle_count)
uint64_t SpeakerToggleCycle(const Apple2& a, uint64_t n);

// video
void CatchUpVideo(Apple2& a, uint64_t cycle);         // scanner fetches up to cycle
void CaptureScreen(const Apple2& a, VideoFrame& out); // a full frame from memory as it is now
// how color lines look: flat colors like an RGB monitor (the default), AppleWin's composite
// ntsc model with its fringes and fine stripes, or a monochrome monitor
enum class VideoLook : uint8_t {
    kColor,
    kNtsc,
    kMono,
};

const char* VideoLookName(VideoLook look);
void RenderFrame(const VideoFrame& frame, VideoLook look, Framebuffer& fb);
void RenderScreen(const Apple2& a, VideoLook look, Framebuffer& fb); // CaptureScreen + RenderFrame
uint16_t ScannerAddress(const SoftSwitches& sw, uint64_t cycle);
bool InVblank(uint64_t cycle);

// snapshots
std::vector<uint8_t> SaveSnapshot(const Apple2& a);
bool LoadSnapshot(Apple2& a, const uint8_t* bytes, size_t size, std::string& error);

} // namespace tisple
