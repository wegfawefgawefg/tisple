#pragma once

// an apple iie with an extended 80-column card (nmos 6502, 64 KB main ram and 64 KB auxiliary
// ram, a language card in each) around our cycle-exact cpu wrapper. two starts:
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
// - interrupts: only the Mockingboard raises them (mockingboard.hpp)
// - the extended 80-column card is always in (a2audit reports 128K); there is no 64 KB iie
//   and no plain 80-column card with 1 KB
// - auxiliary memory (memory_map.cpp): the card's ram as Sather describes it, checked for
//   every RAMRD, RAMWRT, ALTZP, 80STORE, PAGE2 and HIRES combination at each memory edge
//   (tests/test_machine_aux.cpp) and by a2audit; with every switch off the cpu reaches main
//   ram straight, otherwise every access below $C000 goes through the switches. the
//   scanner's floating bus is always the main ram byte, in 80 columns too
// - 80 columns and double-res: the aux byte shows first, then the main byte, 7 dots each,
//   with no delay between them; 80-column text is our font at half width; double lo-res
//   shows aux nibbles rotated right one bit, as the hardware does; the flat look colours a
//   double hi-res line by 4-dot groups from the line's start, the lo-res colours
// - the alternate character set is the enhanced iie's (MouseText at $40-$5F, our own
//   drawings), although the cpu is the unenhanced iie's nmos 6502
// - speaker: an ideal two-position cone, box-filtered, with a 35 Hz high-pass
// - disk ii (disk2.hpp): 4 cycles a bit cell exactly (a real drive's 4 microseconds are
//   about 4.08 cycles) and a constant 300 rpm; the sequencer by its behaviour, not Apple's
//   state rom; the head moves at once (no settle time, no momentum, no quarter tracks; an
//   odd half-track reads the track below); the drive is up to speed the moment the motor
//   is on; a .nib track is 6656 bytes of 8 cells (no self-sync gaps)
// - our disk ii boot rom (disk2_rom.cpp) waits for nothing, so it is for the emulator only
// - mouse card (mouse.hpp): no interrupts (polled mode only), the 6805 by its protocol as
//   AppleWin models it, answering at once; our own firmware talks to it through an extra
//   port the real card lacks

#include "emu/machine.hpp"
#include "link/segment.hpp"
#include "machine/mockingboard_state.hpp"

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

// display switches and the iie memory switches
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

// 16 KB at $D000-$FFFF: two 4 KB banks at $D000 and 8 KB at $E000, in main ram and again in
// auxiliary ram (ALTZP picks which); the switches are shared
struct LanguageCard {
    bool bank2 = true;
    bool read_ram = false;
    bool write_enable = false;
    bool prewrite = false;
    uint8_t bank1_d000[0x1000] = {};
    uint8_t bank2_d000[0x1000] = {};
    uint8_t e000[0x2000] = {};
    uint8_t aux_bank1_d000[0x1000] = {};
    uint8_t aux_bank2_d000[0x1000] = {};
    uint8_t aux_e000[0x2000] = {};
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
    kVideoDouble = 0x10,  // 80 columns or double-res: aux_bytes show first
};

struct VideoFrame {
    uint8_t bytes[kVisibleLines][kVisibleColumns] = {};
    uint8_t aux_bytes[kVisibleLines][kVisibleColumns] = {}; // fetched in double modes only
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
    kTestCard,     // registers and rom bytes the tests fill in
    kDisk2,        // the disk ii controller and its two drives (disk2.cpp)
    kMouse,        // the applemouse ii interface card (mouse.cpp)
    kMockingboard, // two 6522s and two AY-3-8910s (mockingboard.cpp)
};

struct TestCardState {
    uint8_t regs[16] = {};
    uint8_t rom[256] = {};
    uint8_t expansion[2048] = {};
    uint32_t io_accesses = 0;
};

// disk ii. a track is the loop of bits the head sees, one bit cell per 4 cycles; a disk
// byte is 8 cells (32 cycles) and a self-sync byte is $FF and two zero cells (40 cycles).
// the image formats in src/disk/ turn files into these tracks and back.
constexpr int kDiskTracks = 35;
constexpr int kDiskHalfTracks = 80;    // the head can travel to track 39.5
constexpr uint32_t kTrackBytes = 6656; // room for a .nib track, 53248 bits
constexpr uint32_t kMaxTrackBits = kTrackBytes * 8;
constexpr uint32_t kCyclesPerBitCell = 4;
constexpr uint64_t kMotorOffDelay = 1000000; // the drive spins on about a second after "off"

// where a disk came from, so write-back can save it in the same form
enum class DiskFormat : uint8_t {
    kNone = 0,
    kDos,    // .dsk / .do: 140 KB in DOS 3.3 sector order
    kProdos, // .po: 140 KB in ProDOS block order
    kNib,    // .nib: 35 tracks of 6656 raw disk bytes
};

struct DiskTrack {
    uint32_t bit_count = 0;         // the track's length in bit cells
    uint8_t bits[kTrackBytes] = {}; // cell n is bits[n / 8], most significant bit first
};

struct DiskDrive {
    DiskFormat format = DiskFormat::kNone; // kNone: no disk in the drive
    bool write_protected = false;
    bool dirty = false;      // written since inserted or saved
    uint8_t half_track = 0;  // head position, 0 to kDiskHalfTracks - 1
    uint32_t bit_pos = 0;    // the cell under the head
    uint64_t spun_to = 0;    // cycle the disk has turned to (whole cells counted from here)
    uint8_t head_window = 0; // the last four cells read, for the read amplifier's noise
    DiskTrack tracks[kDiskTracks];
};

struct Disk2State {
    uint8_t slot = 0;
    uint8_t rom[256] = {}; // $Cn00: our boot rom, or the owner's DISK2.rom
    bool motor_on = false;
    uint64_t spin_until = 0;     // with the motor off, the drive still turns until this cycle
    uint8_t drive = 0;           // the selected drive, 0 or 1
    uint8_t magnets = 0;         // stepper phases 0-3 that are on, one bit each
    bool q6 = false;             // load (write) or sense write protect (read)
    bool q7 = false;             // write mode
    uint8_t shift = 0;           // the sequencer's shift register
    uint8_t latch = 0;           // the data register the cpu reads
    int8_t latch_hold = 0;       // microseconds a finished nibble still stays in the latch
    uint32_t noise = 0x2545F491; // random bits when the head sees no flux for a while
    DiskDrive drives[2];
};

// applemouse ii (mouse.hpp has the model). positions are the 6805's signed 16-bit numbers
constexpr uint32_t kMouseRomBytes = 2048; // eight 256-byte pages, one at a time at $Cn00
constexpr uint32_t kMaxScheduledMouse = 1024;

// scripted input: from this cycle the mouse is at x, y (then clamped) with the button so
struct ScheduledMouse {
    uint64_t cycle = 0;
    int32_t x = 0;
    int32_t y = 0;
    bool button = false;
};

// the card's 6821 between the 6502 and the 6805: port A data, port B handshake and rom page
struct MousePia {
    uint8_t pra = 0;
    uint8_t ddra = 0;
    uint8_t cra = 0;
    uint8_t prb = 0;
    uint8_t ddrb = 0;
    uint8_t crb = 0;
    uint8_t in_a = 0; // what the 6805 drives on port A
    uint8_t in_b = 0; // and on port B
};

struct MouseCardState {
    uint8_t slot = 0;
    bool real_rom = false;            // the owner's MouseInterface.rom, else ours
    uint8_t rom[kMouseRomBytes] = {}; // ours fills the first page only
    bool button = false;              // the host's button, down now
    int32_t x = 0;                    // the 6805's position and clamping window
    int32_t y = 0;
    int32_t min_x = 0;
    int32_t max_x = 1023;
    int32_t min_y = 0;
    int32_t max_y = 1023;
    uint8_t mode = 0;         // SETMOUSE's mode byte
    bool read_button = false; // the button at the last READMOUSE
    bool moved = false;       // since the last READMOUSE
    uint8_t data[5] = {};     // our firmware's port: registers 9-13
    // the 6805's side of the pia protocol (adapted from AppleWin's MouseInterface.cpp)
    MousePia pia;
    uint8_t port_a = 0;     // the last byte the 6502 put out on port A
    uint8_t port_b = 0x40;  // the 6805's view of port B: page bits 1-3, ready 6-7
    uint8_t buffer[8] = {}; // a command byte and its data bytes
    uint8_t buffer_pos = 0;
    uint8_t data_len = 1;
    uint32_t accesses = 0; // i/o accesses, for the status line
    ScheduledMouse queue[kMaxScheduledMouse] = {};
    uint32_t queue_head = 0;
    uint32_t queue_count = 0;
};

struct SlotCards {
    CardKind kind[8] = {};
    uint8_t expansion_slot = 0; // whose $C800 rom is selected; 0 for none
    TestCardState test_card;
    Disk2State disk2;               // one disk ii card, in disk2.slot
    MouseCardState mouse;           // one mouse card, in mouse.slot
    MockingboardState mockingboard; // one Mockingboard, in mockingboard.slot
};

enum class MachineStop : uint8_t {
    kRunning = 0, // the cycle budget ran out; the machine can go on
    kReturned,    // the entry's final RTS
    kBrk,
    kIllegalOpcode,
    kNoRom, // jumped into rom or slot space with nothing there
};

struct Apple2 {
    Machine cpu;              // registers, pins, cycle counter and the 64 KB of main ram
    uint8_t aux[0xC000] = {}; // auxiliary ram below $C000; its language card is in lc
    SoftSwitches sw;
    LanguageCard lc;
    Rom rom;
    Keyboard keys;
    GamePort game;
    Speaker speaker;
    VideoScan video;
    SlotCards slots;
    bool direct_load = false; // stop on BRK, illegal opcodes and the entry's RTS
    // reaching this pc means the program returned (0: no such place). a disk made by
    // `tisple disk boot` returns there: its loader's call to the program comes back to it
    uint16_t return_pc = 0;
    bool stopped = false;
    MachineStop stop_reason = MachineStop::kRunning;
    uint16_t stop_pc = 0;
    uint8_t stop_opcode = 0;
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
// more of a program, before LoadProgram: main ram below $C000, the main language card's
// $D000-$FFFF with bank 2 at $D000, or auxiliary ram below $C000
bool LoadSegment(Apple2& a, const MemorySegment& segment, std::string& error);
// the reset line: soft switches to their reset state, pc from $FFFC
void ResetApple2(Apple2& a);

// running
RunOutcome RunApple2(Apple2& a, uint64_t cycles);
// the same, calling trace before each instruction (registers' pc is the one about to run)
using MachineTraceFn = void (*)(void* ctx, const Machine& m);
RunOutcome RunApple2Traced(Apple2& a, uint64_t cycles, MachineTraceFn trace, void* trace_ctx);
const char* MachineStopName(MachineStop reason);
std::string DescribeStop(const Apple2& a); // "BRK at $6012", "jumped into rom at $FDED", ...
uint64_t Cycles(const Apple2& a);

// input
void PressKey(Apple2& a, uint8_t code); // held until ReleaseKey
void ReleaseKey(Apple2& a);
bool ScheduleKey(Apple2& a, uint64_t cycle, uint8_t code); // pressed then released
void SetPaddle(Apple2& a, int index, uint8_t value);
void SetButton(Apple2& a, int index, bool down);

// memory as the cpu would read it, without side effects
uint8_t PeekMemory(const Apple2& a, uint16_t addr);
// auxiliary ram, whatever the switches say: below $C000 as is, $C000-$CFFF its language
// card's bank 1 $D000-$DFFF, then $D000-$FFFF with bank 2
uint8_t PeekAux(const Apple2& a, uint16_t addr);

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

// snapshots: the whole struct, disks included, with runs of zero bytes squeezed
std::vector<uint8_t> SaveSnapshot(const Apple2& a);
bool LoadSnapshot(Apple2& a, const uint8_t* bytes, size_t size, std::string& error);

} // namespace tisple
