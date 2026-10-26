// $C000-$FFFF: soft switches, keyboard, speaker, game port, language card, rom, slots.
//
// semantics follow Sather's "Understanding the Apple IIe" and the Apple IIe Technical
// Reference, cross-read against Mii (MIT, third_party/mii) and AppleWin (GPL-2.0-or-later,
// third_party/applewin), Memory.cpp and Joystick.cpp. an access is any bus cycle, read or
// write, so dummy reads and read-modify-write cycles count; only $C000-$C00F are write-only.

#include "machine/bus.hpp"

#include "machine/slots.hpp"

namespace tisple {

namespace {

// pattern returned by rom space when no rom is loaded: the address's high byte
uint8_t NoRomByte(uint16_t addr) {
    return static_cast<uint8_t>(addr >> 8);
}

uint64_t AccessCycle(const Apple2& a) {
    // the wrapper counts the cycle before performing it
    return a.cpu.cycles - 1;
}

uint8_t WithBit7(const Apple2& a, bool bit, uint64_t t) {
    return static_cast<uint8_t>((bit ? 0x80 : 0x00) | (FloatingBus(a, t) & 0x7F));
}

// writes to $C000-$C00F: pairs of off/on switches
void MemorySwitchWrite(Apple2& a, uint16_t addr, uint64_t t) {
    SoftSwitches& sw = a.sw;
    bool on = (addr & 1) != 0;
    switch ((addr & 0x0F) >> 1) {
    case 0:
        CatchUpVideo(a, t + 1); // display page choice depends on 80store
        sw.store80 = on;
        break;
    case 1:
        sw.ramrd = on;
        break;
    case 2:
        sw.ramwrt = on;
        break;
    case 3:
        sw.intcxrom = on;
        break;
    case 4:
        sw.altzp = on;
        break;
    case 5:
        sw.slotc3rom = on;
        break;
    case 6:
        CatchUpVideo(a, t + 1);
        sw.col80 = on;
        break;
    default:
        CatchUpVideo(a, t + 1);
        sw.altchar = on;
        break;
    }
    if (on && (sw.ramrd || sw.ramwrt || sw.altzp)) {
        a.stub_notes |= kNoteAuxMemory;
    }
    if (on && sw.col80) {
        a.stub_notes |= kNote80Columns;
    }
}

// $C011-$C01F status bits
bool StatusBit(const Apple2& a, uint16_t addr, uint64_t t) {
    const SoftSwitches& sw = a.sw;
    switch (addr & 0x0F) {
    case 0x1:
        return a.lc.bank2;
    case 0x2:
        return a.lc.read_ram;
    case 0x3:
        return sw.ramrd;
    case 0x4:
        return sw.ramwrt;
    case 0x5:
        return sw.intcxrom;
    case 0x6:
        return sw.altzp;
    case 0x7:
        return sw.slotc3rom;
    case 0x8:
        return sw.store80;
    case 0x9:
        return !InVblank(t); // RDVBLBAR: 1 while the beam draws
    case 0xA:
        return sw.text;
    case 0xB:
        return sw.mixed;
    case 0xC:
        return sw.page2;
    case 0xD:
        return sw.hires;
    case 0xE:
        return sw.altchar;
    default:
        return sw.col80;
    }
}

uint8_t KeyboardAccess(Apple2& a, uint16_t addr, bool is_write, uint64_t t) {
    Keyboard& k = a.keys;
    if ((addr & 0xF0) == 0x00) {
        return k.latch; // $C000-$C00F read: the key and its strobe
    }
    // $C010 read, or any write to $C010-$C01F, clears the strobe
    if (is_write) {
        k.latch &= 0x7F;
        return 0;
    }
    if ((addr & 0x0F) == 0) {
        uint8_t value = static_cast<uint8_t>((k.key_down ? 0x80 : 0x00) | (k.latch & 0x7F));
        k.latch &= 0x7F;
        return value;
    }
    return static_cast<uint8_t>((StatusBit(a, addr, t) ? 0x80 : 0x00) | (k.latch & 0x7F));
}

void SpeakerToggle(Apple2& a, uint64_t t) {
    Speaker& s = a.speaker;
    s.toggles[s.toggle_count % kSpeakerRingSize] = t;
    s.toggle_count++;
}

// $C050-$C05F: display modes and annunciators
void DisplaySwitch(Apple2& a, uint16_t addr, uint64_t t) {
    SoftSwitches& sw = a.sw;
    bool on = (addr & 1) != 0;
    int which = (addr & 0x0F) >> 1;
    bool* target = nullptr;
    switch (which) {
    case 0:
        target = &sw.text;
        break;
    case 1:
        target = &sw.mixed;
        break;
    case 2:
        target = &sw.page2;
        break;
    case 3:
        target = &sw.hires;
        break;
    default:
        sw.an[which - 4] = on;
        if (which == 7 && !on && sw.col80) {
            a.stub_notes |= kNoteDoubleRes;
        }
        return;
    }
    if (*target == on) {
        return;
    }
    // the scanner draws everything up to this cycle with the old mode
    CatchUpVideo(a, t + 1);
    *target = on;
    if (which == 2 && sw.store80) {
        a.stub_notes |= kNoteAuxMemory;
    }
}

// $C060-$C06F (bit 3 ignored): cassette in, buttons, paddle timers
uint8_t GameInput(const Apple2& a, uint16_t addr, uint64_t t) {
    int which = addr & 0x07;
    if (which == 0) {
        return WithBit7(a, false, t); // nothing on the cassette input
    }
    if (which < 4) {
        return WithBit7(a, a.game.button[which - 1], t);
    }
    return WithBit7(a, t < a.game.paddle_end[which - 4], t);
}

// $C070-$C07F: start the four paddle timers. 2816 cycles at full scale (from KEGS via
// AppleWin's Joystick.cpp), about 11 per step, the loop length of the rom's PREAD.
void TriggerPaddles(Apple2& a, uint64_t t) {
    for (int i = 0; i < 4; i++) {
        a.game.paddle_end[i] = t + 1 + (static_cast<uint64_t>(a.game.paddle[i]) * 2816) / 255;
    }
}

// $C080-$C08F, per Sather: odd reads twice in a row enable writing, even accesses
// write-protect; bit 1 picks rom or ram for reads; bit 3 picks the $D000 bank
void LanguageCardAccess(Apple2& a, uint16_t addr, bool is_write) {
    LanguageCard& lc = a.lc;
    lc.bank2 = (addr & 0x08) == 0;
    if ((addr & 0x01) != 0) {
        if (!is_write && lc.prewrite) {
            lc.write_enable = true;
        }
        lc.prewrite = !is_write;
        lc.read_ram = (addr & 0x02) != 0;
    } else {
        lc.write_enable = false;
        lc.prewrite = false;
        lc.read_ram = (addr & 0x02) == 0;
    }
}

uint8_t IoAccess(Apple2& a, uint16_t addr, bool is_write, uint8_t value) {
    uint64_t t = AccessCycle(a);
    switch ((addr >> 4) & 0x0F) {
    case 0x0:
        if (is_write) {
            MemorySwitchWrite(a, addr, t);
            return 0;
        }
        return KeyboardAccess(a, addr, false, t);
    case 0x1:
        return KeyboardAccess(a, addr, is_write, t);
    case 0x3:
        SpeakerToggle(a, t);
        break;
    case 0x5:
        DisplaySwitch(a, addr, t);
        break;
    case 0x6:
        return is_write ? 0 : GameInput(a, addr, t);
    case 0x7:
        TriggerPaddles(a, t);
        break;
    case 0x8:
        LanguageCardAccess(a, addr, is_write);
        break;
    case 0x9:
    case 0xA:
    case 0xB:
    case 0xC:
    case 0xD:
    case 0xE:
    case 0xF: {
        bool drives = false;
        uint8_t got = CardIoAccess(a, (addr >> 4) & 0x07, addr & 0x0F, is_write, value, drives);
        if (drives) {
            return got;
        }
        break;
    }
    default:
        break; // $C020 cassette out, $C040 strobe: nothing to model
    }
    return is_write ? 0 : FloatingBus(a, t);
}

uint8_t InternalRomByte(const Apple2& a, uint16_t addr) {
    return a.rom.has_cx ? a.rom.bytes[addr - 0xC000] : NoRomByte(addr);
}

// $C100-$CFFF, read or write: slot rom selection and its side effects
uint8_t CxAccess(Apple2& a, uint16_t addr, bool is_write, uint8_t value) {
    SoftSwitches& sw = a.sw;
    uint8_t result = 0;
    bool drives = false;
    if (addr < 0xC800) {
        int slot = (addr >> 8) & 0x07;
        bool internal_c3 = slot == 3 && !sw.slotc3rom;
        if (sw.intcxrom || internal_c3) {
            if (internal_c3) {
                sw.intc8rom = true;
            }
            drives = !is_write;
            result = InternalRomByte(a, addr);
        } else {
            a.slots.expansion_slot = static_cast<uint8_t>(slot);
            result = CardRomAccess(a, slot, addr & 0xFF, is_write, value, drives);
        }
    } else if (sw.intcxrom || sw.intc8rom) {
        drives = !is_write;
        result = InternalRomByte(a, addr);
    } else {
        result =
            CardExpansionAccess(a, a.slots.expansion_slot, addr - 0xC800, is_write, value, drives);
    }
    if (addr == 0xCFFF) {
        // releases every $C800 rom
        sw.intc8rom = false;
        a.slots.expansion_slot = 0;
    }
    if (is_write) {
        return 0;
    }
    return drives ? result : FloatingBus(a, AccessCycle(a));
}

uint8_t* LanguageCardByte(Apple2& a, uint16_t addr) {
    if (addr >= 0xE000) {
        return &a.lc.e000[addr - 0xE000];
    }
    return a.lc.bank2 ? &a.lc.bank2_d000[addr - 0xD000] : &a.lc.bank1_d000[addr - 0xD000];
}

uint8_t BusRead(void* ctx, uint16_t addr) {
    Apple2& a = *static_cast<Apple2*>(ctx);
    if (addr < 0xC100) {
        return IoAccess(a, addr, false, 0);
    }
    if (addr < 0xD000) {
        return CxAccess(a, addr, false, 0);
    }
    if (a.lc.read_ram) {
        return *LanguageCardByte(a, addr);
    }
    return a.rom.present ? a.rom.bytes[addr - 0xC000] : NoRomByte(addr);
}

void BusWrite(void* ctx, uint16_t addr, uint8_t value) {
    Apple2& a = *static_cast<Apple2*>(ctx);
    if (addr < 0xC100) {
        IoAccess(a, addr, true, value);
        return;
    }
    if (addr < 0xD000) {
        CxAccess(a, addr, true, value);
        return;
    }
    if (a.lc.write_enable) {
        *LanguageCardByte(a, addr) = value;
    }
}

} // namespace

void InstallBus(Apple2& a) {
    IoHook hook;
    hook.base = 0xC000;
    hook.size = 0x4000;
    hook.read = BusRead;
    hook.write = BusWrite;
    hook.ctx = &a;
    SetIoHook(a.cpu, hook);
}

uint8_t FloatingBus(const Apple2& a, uint64_t cycle) {
    return a.cpu.mem[ScannerAddress(a.sw, cycle)];
}

bool CanExecute(const Apple2& a, uint16_t addr) {
    if (addr < 0xC100) {
        return false;
    }
    if (addr >= 0xD000) {
        return a.lc.read_ram || a.rom.present;
    }
    const SoftSwitches& sw = a.sw;
    int slot = (addr >> 8) & 0x07;
    bool internal =
        addr < 0xC800 ? sw.intcxrom || (slot == 3 && !sw.slotc3rom) : sw.intcxrom || sw.intc8rom;
    if (internal) {
        return a.rom.has_cx;
    }
    int card_slot = addr < 0xC800 ? slot : a.slots.expansion_slot;
    return CardHasRom(a, card_slot, addr >= 0xC800);
}

uint8_t PeekMemory(const Apple2& a, uint16_t addr) {
    if (addr < 0xC000) {
        return a.cpu.mem[addr];
    }
    if (addr < 0xC100) {
        return 0; // i/o: reading has effects, so peeking shows nothing
    }
    if (addr < 0xD000) {
        const SoftSwitches& sw = a.sw;
        int slot = (addr >> 8) & 0x07;
        bool internal = addr < 0xC800 ? sw.intcxrom || (slot == 3 && !sw.slotc3rom)
                                      : sw.intcxrom || sw.intc8rom;
        if (internal) {
            return InternalRomByte(a, addr);
        }
        int card_slot = addr < 0xC800 ? slot : a.slots.expansion_slot;
        return CardRomPeek(a, card_slot, addr);
    }
    if (a.lc.read_ram) {
        if (addr >= 0xE000) {
            return a.lc.e000[addr - 0xE000];
        }
        return a.lc.bank2 ? a.lc.bank2_d000[addr - 0xD000] : a.lc.bank1_d000[addr - 0xD000];
    }
    return a.rom.present ? a.rom.bytes[addr - 0xC000] : NoRomByte(addr);
}

void ResetSoftSwitches(Apple2& a) {
    // the iie reset line clears the memory switches; display switches are left to the rom
    SoftSwitches& sw = a.sw;
    sw.store80 = false;
    sw.ramrd = false;
    sw.ramwrt = false;
    sw.intcxrom = false;
    sw.altzp = false;
    sw.slotc3rom = false;
    sw.col80 = false;
    sw.altchar = false;
    sw.intc8rom = false;
    a.slots.expansion_slot = 0;
    // language card after reset: bank 2, rom reads, ram writes enabled
    a.lc.bank2 = true;
    a.lc.read_ram = false;
    a.lc.write_enable = true;
    a.lc.prewrite = false;
}

} // namespace tisple
