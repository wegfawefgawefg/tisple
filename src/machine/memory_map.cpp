// main or auxiliary ram below $C000: the iie memory switches of an extended 80-column card,
// after Sather's "Understanding the Apple IIe" (chapter 5) and the IIe Technical Reference.
//
// - $0000-$01FF (zero page and stack): ALTZP, read and write alike
// - with 80STORE on, $0400-$07FF, and $2000-$3FFF too while HIRES is on: PAGE2, read and
//   write alike (PAGE2 then no longer picks the displayed page)
// - the rest of $0200-$BFFF: RAMRD for reads, RAMWRT for writes
// - the language card's $D000-$FFFF: ALTZP (bus.cpp)
//
// the cpu wrapper reaches main ram itself, so with every switch off nothing changes there
// and the bus hook covers $C000-$FFFF only; with any on, the hook takes the whole address
// space and every access below $C000 comes here.

#include "machine/bus.hpp"

namespace tisple {

namespace {

bool AuxSwitchedIn(const SoftSwitches& sw) {
    return sw.ramrd || sw.ramwrt || sw.altzp || (sw.store80 && sw.page2);
}

} // namespace

bool ReachesAux(const SoftSwitches& sw, uint16_t addr, bool is_write) {
    if (addr < 0x0200) {
        return sw.altzp;
    }
    if (sw.store80) {
        bool text = addr >= 0x0400 && addr < 0x0800;
        bool hires = sw.hires && addr >= 0x2000 && addr < 0x4000;
        if (text || hires) {
            return sw.page2;
        }
    }
    return is_write ? sw.ramwrt : sw.ramrd;
}

uint8_t* RamByte(Apple2& a, uint16_t addr, bool is_write) {
    return ReachesAux(a.sw, addr, is_write) ? &a.aux[addr] : &a.cpu.mem[addr];
}

void UpdateMemoryMap(Apple2& a) {
    IoHook& hook = a.cpu.io;
    if (hook.read == nullptr) {
        return; // not installed yet
    }
    if (AuxSwitchedIn(a.sw)) {
        hook.base = 0x0000;
        hook.size = 0x10000;
    } else {
        hook.base = 0xC000;
        hook.size = 0x4000;
    }
}

} // namespace tisple
