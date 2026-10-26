// the video scanner: which byte the display fetches at each cycle, and the color burst.
//
// ScannerAddress is ported from AppleWin's Video::VideoGetScannerAddress (GPL-2.0-or-later,
// see third_party/applewin), which follows Sather's UTAIIe scanner equations. the color burst
// counter follows AppleWin's NTSC.cpp. the catch-up model is ours: before a display switch
// changes, and at the end of every line, the scanner records what it fetched into
// video.live; a finished frame is copied to video.done.

#include "machine/apple2.hpp"

#include <cstring>

namespace tisple {

namespace {

constexpr uint32_t kBurstEndCycle = 16; // color burst window: cycles 12-15 of each line
constexpr uint32_t kMixedTextLine = 160;
constexpr int32_t kBurstFull = 1024;

// offsets of the 24 text rows (and 8-line groups of hi-res) within a page
uint16_t RowOffset(uint32_t row) {
    return static_cast<uint16_t>((row & 7) * 0x80 + (row >> 3) * 0x28);
}

uint16_t DisplayPageBit(const SoftSwitches& sw) {
    // with 80store on, page2 picks auxiliary memory instead of the second page
    return sw.page2 && !sw.store80 ? 1 : 0;
}

bool TextLine(const SoftSwitches& sw, uint32_t line) {
    return sw.text || (sw.mixed && line >= kMixedTextLine);
}

uint8_t ModeFor(const SoftSwitches& sw, uint32_t line) {
    if (TextLine(sw, line)) {
        return static_cast<uint8_t>(kVideoText | (sw.altchar ? kVideoAltChar : 0));
    }
    return sw.hires ? kVideoHires : kVideoLores;
}

uint16_t LineBase(const SoftSwitches& sw, uint32_t line) {
    uint16_t page = DisplayPageBit(sw);
    if (!TextLine(sw, line) && sw.hires) {
        return static_cast<uint16_t>(0x2000 + page * 0x2000 + (line & 7) * 0x400 +
                                     RowOffset(line >> 3));
    }
    return static_cast<uint16_t>(0x0400 + page * 0x0400 + RowOffset(line >> 3));
}

void FetchColumns(const Apple2& a, VideoFrame& frame, uint32_t line, uint32_t first, uint32_t end) {
    uint16_t base = LineBase(a.sw, line);
    uint8_t mode = ModeFor(a.sw, line);
    for (uint32_t col = first; col < end; col++) {
        frame.bytes[line][col] = a.cpu.mem[base + col];
        frame.modes[line][col] = mode;
    }
}

// end of a line's burst window: graphics lines set the counter, text lines run it down
void UpdateBurst(Apple2& a, uint32_t line) {
    VideoScan& v = a.video;
    const SoftSwitches& sw = a.sw;
    bool text_now = TextLine(sw, line);
    if (line < kVisibleLines && !text_now) {
        v.burst_counter = kBurstFull;
    } else if (line < kVisibleLines || sw.text || sw.mixed) {
        v.burst_counter = v.burst_counter > 4 ? v.burst_counter - 4 : 0;
    }
    if (line < kVisibleLines) {
        v.live.burst[line] = v.burst_counter >= 2;
    }
}

bool FlashInverse(uint64_t frame_index) {
    // the iie flash clock: 16 frames each way, about 1.9 Hz
    return ((frame_index >> 4) & 1) != 0;
}

void FinishFrame(Apple2& a, uint64_t frame_index) {
    VideoScan& v = a.video;
    v.live.frame_index = frame_index;
    v.live.flash_inverse = FlashInverse(frame_index);
    v.done = v.live;
    v.frames_done++;
}

// the next cycle the run loop should catch up at: each visible line's end; in vertical
// blank, the end of the next frame's line 0
uint64_t NextCheck(uint64_t scanned) {
    uint64_t frame_start = scanned - scanned % kCyclesPerFrame;
    uint64_t line = (scanned - frame_start) / kCyclesPerLine;
    if (line + 1 < kVisibleLines) {
        return frame_start + (line + 1) * kCyclesPerLine;
    }
    return frame_start + kCyclesPerFrame + kCyclesPerLine;
}

} // namespace

void CatchUpVideo(Apple2& a, uint64_t cycle) {
    VideoScan& v = a.video;
    while (v.scanned < cycle) {
        uint64_t frame_index = v.scanned / kCyclesPerFrame;
        uint32_t pos = static_cast<uint32_t>(v.scanned - frame_index * kCyclesPerFrame);
        uint32_t line = pos / kCyclesPerLine;
        uint32_t h = pos - line * kCyclesPerLine;
        uint64_t line_start = v.scanned - h;
        uint64_t stop = 0;
        if (h < kBurstEndCycle) {
            stop = line_start + kBurstEndCycle;
            if (stop > cycle) {
                stop = cycle;
            } else {
                UpdateBurst(a, line);
            }
        } else if (h < kFirstVisibleCycle) {
            stop = line_start + kFirstVisibleCycle;
            stop = stop < cycle ? stop : cycle;
        } else {
            stop = line_start + kCyclesPerLine;
            stop = stop < cycle ? stop : cycle;
            if (line < kVisibleLines) {
                uint32_t first = h - kFirstVisibleCycle;
                uint32_t end = static_cast<uint32_t>(stop - line_start) - kFirstVisibleCycle;
                FetchColumns(a, v.live, line, first, end);
                if (line == kVisibleLines - 1 && end == kVisibleColumns) {
                    FinishFrame(a, frame_index);
                }
            }
        }
        v.scanned = stop;
    }
    v.next_check = NextCheck(v.scanned);
}

void CaptureScreen(const Apple2& a, VideoFrame& out) {
    for (uint32_t line = 0; line < kVisibleLines; line++) {
        FetchColumns(a, out, line, 0, kVisibleColumns);
        // a steady picture: graphics keep the burst on, all-text lets it die away
        out.burst[line] = !a.sw.text;
    }
    out.frame_index = a.cpu.cycles / kCyclesPerFrame;
    out.flash_inverse = FlashInverse(out.frame_index);
}

bool InVblank(uint64_t cycle) {
    return cycle % kCyclesPerFrame >= kVblankStartCycle;
}

uint16_t ScannerAddress(const SoftSwitches& sw, uint64_t cycle) {
    uint32_t pos = static_cast<uint32_t>(cycle % kCyclesPerFrame);

    // horizontal state: H5-H0, preset twice at clock 41 (UTAIIe 3-11)
    uint32_t h_clock = (pos + 40) % kCyclesPerLine;
    uint32_t h_state = 0x18 + h_clock;
    if (h_clock >= 41) {
        h_state -= 1;
    }
    uint32_t h0 = (h_state >> 0) & 1;
    uint32_t h1 = (h_state >> 1) & 1;
    uint32_t h2 = (h_state >> 2) & 1;
    uint32_t h3 = (h_state >> 3) & 1;
    uint32_t h4 = (h_state >> 4) & 1;
    uint32_t h5 = (h_state >> 5) & 1;

    // vertical state: V5-V0 VC VB VA from $100, preset at line 256 (UTAIIe 3-15)
    uint32_t v_line = pos / kCyclesPerLine;
    uint32_t v_state = 0x100 + v_line;
    if (v_line >= 256) {
        v_state -= kLinesPerFrame;
    }
    uint32_t va = (v_state >> 0) & 1;
    uint32_t vb = (v_state >> 1) & 1;
    uint32_t vc = (v_state >> 2) & 1;
    uint32_t v0 = (v_state >> 3) & 1;
    uint32_t v1 = (v_state >> 4) & 1;
    uint32_t v2 = (v_state >> 5) & 1;
    uint32_t v3 = (v_state >> 6) & 1;
    uint32_t v4 = (v_state >> 7) & 1;

    bool hires = sw.hires && !sw.text;
    if (hires && sw.mixed && v4 && v2) {
        hires = false; // the mixed-mode text rows fetch from text memory
    }

    // the row adder (UTAIIe 5-9)
    uint32_t addend1 = (h5 << 2) | (h4 << 1) | h3;
    uint32_t addend2 = (v4 << 3) | (v3 << 2) | (v4 << 1) | v3;
    uint32_t sum = (0x0D + addend1 + addend2) & 0x0F;

    uint32_t address = h0 | (h1 << 1) | (h2 << 2) | (sum << 3);
    address |= (v0 << 7) | (v1 << 8) | (v2 << 9);
    uint32_t p2a = DisplayPageBit(sw) ? 0 : 1;
    uint32_t p2b = 1 - p2a;
    if (hires) {
        address |= (va << 10) | (vb << 11) | (vc << 12) | (p2a << 13) | (p2b << 14);
    } else {
        address |= (p2a << 10) | (p2b << 11);
    }
    return static_cast<uint16_t>(address);
}

} // namespace tisple
