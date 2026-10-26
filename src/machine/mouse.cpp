// the applemouse ii card: see mouse.hpp for the model. the 6821 is VICE's mc6821 as AppleWin
// adapted it (6821.cpp, Andreas Boose, GPL-2.0-or-later), and the 6805's side of the
// protocol (the port B strobes, command lengths and answers) follows AppleWin's
// MouseInterface.cpp (GPL-2.0-or-later, based on Kyle Kim's Apple in PC). the commands'
// meanings follow Apple's firmware documentation: CLAMPMOUSE takes signed bounds, and
// HOMEMOUSE goes to the window's corner rather than to 0, 0.

#include "machine/mouse.hpp"

#include "machine/slots.hpp"

#include <cstdio>
#include <cstring>

namespace tisple {

namespace {

constexpr uint8_t kStatusDown = 0x80;
constexpr uint8_t kStatusWasDown = 0x40;
constexpr uint8_t kStatusMoved = 0x20;

constexpr uint8_t kCommandSet = 0x00;
constexpr uint8_t kCommandRead = 0x10;
constexpr uint8_t kCommandServe = 0x20;
constexpr uint8_t kCommandClear = 0x30;
constexpr uint8_t kCommandPos = 0x40;
constexpr uint8_t kCommandInit = 0x50;
constexpr uint8_t kCommandClamp = 0x60;
constexpr uint8_t kCommandHome = 0x70;
constexpr uint8_t kCommandTime = 0x90;

constexpr int kPortCommand = 8; // our firmware's port: the command
constexpr int kPortData = 9;    // and its five data bytes

MouseCardState& Card(Apple2& a) {
    return a.slots.mouse;
}

uint64_t AccessCycle(const Apple2& a) {
    return a.cpu.cycles - 1; // the wrapper counts the cycle before performing it
}

int32_t Signed16(uint8_t low, uint8_t high) {
    return static_cast<int16_t>(static_cast<uint16_t>(low | (high << 8)));
}

void ClampPosition(MouseCardState& m) {
    if (m.x > m.max_x) {
        m.x = m.max_x;
    }
    if (m.x < m.min_x) {
        m.x = m.min_x;
    }
    if (m.y > m.max_y) {
        m.y = m.max_y;
    }
    if (m.y < m.min_y) {
        m.y = m.min_y;
    }
}

void MoveBy(MouseCardState& m, int32_t dx, int32_t dy) {
    int32_t x = m.x;
    int32_t y = m.y;
    m.x += dx;
    m.y += dy;
    ClampPosition(m);
    if ((m.mode & 1) != 0 && (m.x != x || m.y != y)) {
        m.moved = true;
    }
}

// scripted moves whose time has come
void CatchUp(Apple2& a) {
    MouseCardState& m = Card(a);
    uint64_t now = AccessCycle(a);
    while (m.queue_count > 0 && m.queue[m.queue_head].cycle <= now) {
        const ScheduledMouse& e = m.queue[m.queue_head];
        MoveBy(m, e.x - m.x, e.y - m.y);
        m.button = e.button;
        m.queue_head = (m.queue_head + 1) % kMaxScheduledMouse;
        m.queue_count--;
    }
}

void SetPosition(MouseCardState& m, int32_t x, int32_t y) {
    m.x = x;
    m.y = y;
}

// ---- the 6805: a command's immediate part, then its part after the data bytes

// READ, SERVE, SET, CLEAR and HOME act at once; READ and SERVE fill reply
void StartCommand(MouseCardState& m, uint8_t command, uint8_t reply[5]) {
    switch (command & 0xF0) {
    case kCommandSet:
        m.mode = command & 0x0F;
        return;
    case kCommandRead: {
        uint8_t status = 0;
        status |= m.button ? kStatusDown : 0;
        status |= m.read_button ? kStatusWasDown : 0;
        status |= m.moved ? kStatusMoved : 0;
        m.read_button = m.button;
        m.moved = false;
        reply[0] = static_cast<uint8_t>(m.x & 0xFF);
        reply[1] = static_cast<uint8_t>((m.x >> 8) & 0xFF);
        reply[2] = static_cast<uint8_t>(m.y & 0xFF);
        reply[3] = static_cast<uint8_t>((m.y >> 8) & 0xFF);
        reply[4] = status;
        return;
    }
    case kCommandServe:
        reply[0] = 0; // no interrupt was ever the mouse's
        return;
    case kCommandClear:
        SetPosition(m, 0, 0);
        m.read_button = false;
        m.moved = false;
        return;
    case kCommandHome:
        SetPosition(m, m.min_x, m.min_y);
        return;
    default:
        return;
    }
}

// POS, CLAMP and INIT act once their data bytes are in (args: the four after the command)
void FinishCommand(MouseCardState& m, uint8_t command, const uint8_t args[4]) {
    switch (command & 0xF0) {
    case kCommandPos:
        SetPosition(m, Signed16(args[0], args[1]), Signed16(args[2], args[3]));
        return;
    case kCommandClamp: {
        int32_t low = Signed16(args[0], args[2]);
        int32_t high = Signed16(args[1], args[3]);
        if ((command & 1) == 0) {
            m.min_x = low;
            m.max_x = high;
        } else {
            m.min_y = low;
            m.max_y = high;
        }
        ClampPosition(m);
        return;
    }
    case kCommandInit:
        m.min_x = 0;
        m.max_x = 1023;
        m.min_y = 0;
        m.max_y = 1023;
        SetPosition(m, 0, 0);
        return;
    default:
        return;
    }
}

// ---- the pia protocol: the 6502 strobes bytes in and out through port B

// how many bytes a command moves, itself included (AppleWin's OnCommand)
uint8_t CommandLength(uint8_t command) {
    switch (command & 0xF0) {
    case kCommandRead:
        return 6;
    case kCommandServe:
        return 2;
    case kCommandPos:
    case kCommandClamp:
        return 5;
    case kCommandInit:
        return 3;
    case kCommandTime: {
        static const uint8_t kTimeLengths[4] = {1, 3, 2, 4};
        return kTimeLengths[(command >> 2) & 3];
    }
    case 0xA0:
        return 2;
    default:
        return 1;
    }
}

void OnCommand(MouseCardState& m) {
    uint8_t command = m.buffer[0];
    m.data_len = CommandLength(command);
    StartCommand(m, command, m.buffer + 1);
    if ((command & 0xF0) == kCommandInit) {
        m.buffer[1] = 0xFF; // AppleWin's answer; what the 6805 says is not known
    }
    m.pia.in_a = m.buffer[1];
}

// the 6502 changed port B's outputs
void OnPortB(MouseCardState& m, uint8_t data) {
    uint8_t diff = static_cast<uint8_t>((m.port_b ^ data) & 0x3E);
    if (diff == 0) {
        return;
    }
    m.port_b = static_cast<uint8_t>((m.port_b & ~0x3E) | (data & 0x3E));
    if ((diff & 0x20) != 0) {
        if ((data & 0x20) != 0) {
            m.port_b |= 0x80; // ready to take a byte
        } else {
            // the byte on port A is in
            m.buffer[m.buffer_pos++] = m.port_a;
            if (m.buffer_pos == 1) {
                OnCommand(m);
            }
            if (m.buffer_pos == m.data_len || m.buffer_pos > 7) {
                FinishCommand(m, m.buffer[0], m.buffer + 1);
                m.buffer_pos = 0;
            }
            m.port_b &= 0x7F;
            m.pia.in_b = m.port_b;
        }
    }
    if ((diff & 0x10) != 0) {
        if ((data & 0x10) != 0) {
            m.port_b &= static_cast<uint8_t>(~0x40); // getting the next byte ready
        } else {
            // the byte on port A was taken
            if (m.buffer_pos != 0) {
                m.buffer_pos++;
            }
            if (m.buffer_pos == m.data_len || m.buffer_pos > 7) {
                m.buffer_pos = 0;
            } else {
                m.pia.in_a = m.buffer[m.buffer_pos];
            }
            m.port_b |= 0x40;
        }
    }
    m.pia.in_b = m.port_b;
}

void PiaWrite(MouseCardState& m, int rs, uint8_t value) {
    MousePia& p = m.pia;
    switch (rs) {
    case 0:
        if ((p.cra & 0x04) != 0) {
            p.pra = value;
            if (p.ddra != 0) {
                m.port_a = p.pra & p.ddra;
            }
        } else if (p.ddra != value) {
            p.ddra = value;
            if (p.ddra != 0) {
                m.port_a = p.pra & p.ddra;
            }
        }
        return;
    case 1:
        p.cra = value;
        return;
    case 2:
        if ((p.crb & 0x04) != 0) {
            p.prb = value;
            if (p.ddrb != 0) {
                OnPortB(m, p.prb & p.ddrb);
            }
        } else if (p.ddrb != value) {
            p.ddrb = value;
            if (p.ddrb != 0) {
                OnPortB(m, p.prb & p.ddrb);
            }
        }
        return;
    default:
        p.crb = value;
        return;
    }
}

uint8_t PiaRead(MouseCardState& m, int rs) {
    MousePia& p = m.pia;
    switch (rs) {
    case 0:
        if ((p.cra & 0x04) == 0) {
            return p.ddra;
        }
        p.cra &= 0x3F;
        return static_cast<uint8_t>((p.pra & p.ddra) | (p.in_a & ~p.ddra));
    case 1:
        return p.cra;
    case 2:
        if ((p.crb & 0x04) == 0) {
            return p.ddrb;
        }
        p.crb &= 0x3F;
        return static_cast<uint8_t>((p.prb & p.ddrb) | (p.in_b & ~p.ddrb));
    default:
        return p.crb;
    }
}

// ---- our firmware's port

uint8_t PortAccess(MouseCardState& m, int reg, bool is_write, uint8_t value) {
    if (reg == kPortCommand) {
        if (is_write) {
            StartCommand(m, value, m.data);
            FinishCommand(m, value, m.data);
        }
        return 0;
    }
    int index = reg - kPortData;
    if (index < 0 || index >= 5) {
        return 0;
    }
    if (is_write) {
        m.data[index] = value;
    }
    return m.data[index];
}

void ResetState(MouseCardState& m) {
    m.pia = MousePia();
    m.port_a = 0;
    m.port_b = 0x40;
    m.pia.in_b = m.port_b;
    m.mode = 0;
    m.x = 0;
    m.y = 0;
    m.min_x = 0;
    m.max_x = 1023;
    m.min_y = 0;
    m.max_y = 1023;
    m.read_button = false;
    m.moved = false;
    m.buffer_pos = 0;
    m.data_len = 1;
    std::memset(m.buffer, 0, sizeof(m.buffer));
    std::memset(m.data, 0, sizeof(m.data));
}

} // namespace

uint8_t MouseIoAccess(Apple2& a, int reg, bool is_write, uint8_t value, bool& drives) {
    MouseCardState& m = Card(a);
    m.accesses++;
    CatchUp(a);
    drives = !is_write;
    if (reg < 8) {
        if (is_write) {
            PiaWrite(m, reg & 3, value);
            return 0;
        }
        return PiaRead(m, reg & 3);
    }
    return PortAccess(m, reg, is_write, value);
}

uint8_t MouseRomByte(const Apple2& a, int offset) {
    const MouseCardState& m = a.slots.mouse;
    if (!m.real_rom) {
        return m.rom[offset & 0xFF];
    }
    uint32_t page = (static_cast<uint32_t>(m.port_b) << 7) & 0x0700;
    return m.rom[page + static_cast<uint32_t>(offset & 0xFF)];
}

void ResetMouse(Apple2& a) {
    ResetState(Card(a));
}

void InsertMouseCard(Apple2& a, int slot, const uint8_t* rom) {
    InsertCard(a, slot, CardKind::kMouse);
    MouseCardState& m = Card(a);
    m.slot = static_cast<uint8_t>(slot);
    std::memset(m.rom, 0, sizeof(m.rom));
    m.real_rom = rom != nullptr;
    if (rom != nullptr) {
        std::memcpy(m.rom, rom, sizeof(m.rom));
    } else {
        BuildMouseRom(slot, m.rom);
    }
    ResetState(m);
}

bool HasMouseCard(const Apple2& a) {
    const MouseCardState& m = a.slots.mouse;
    return m.slot >= 1 && m.slot <= 7 && a.slots.kind[m.slot] == CardKind::kMouse;
}

void MoveMouse(Apple2& a, int32_t dx, int32_t dy) {
    MoveBy(Card(a), dx, dy);
}

void SetMouseButton(Apple2& a, bool down) {
    Card(a).button = down;
}

bool ScheduleMouse(Apple2& a, uint64_t cycle, int32_t x, int32_t y, bool button) {
    MouseCardState& m = Card(a);
    if (m.queue_count == kMaxScheduledMouse) {
        return false;
    }
    // keep the queue sorted by cycle: insert from the back
    uint32_t i = m.queue_count;
    while (i > 0) {
        const ScheduledMouse& prev = m.queue[(m.queue_head + i - 1) % kMaxScheduledMouse];
        if (prev.cycle <= cycle) {
            break;
        }
        m.queue[(m.queue_head + i) % kMaxScheduledMouse] = prev;
        i--;
    }
    m.queue[(m.queue_head + i) % kMaxScheduledMouse] = {cycle, x, y, button};
    m.queue_count++;
    return true;
}

std::string DescribeMouse(const Apple2& a) {
    const MouseCardState& m = a.slots.mouse;
    if (!HasMouseCard(a) || m.accesses == 0) {
        return "";
    }
    char text[128];
    std::snprintf(text, sizeof(text), "mouse: slot %d, x %d y %d, button %s, mode $%02X%s\n",
                  m.slot, m.x, m.y, m.button ? "down" : "up", m.mode,
                  m.real_rom ? ", MouseInterface.rom" : "");
    return text;
}

} // namespace tisple
