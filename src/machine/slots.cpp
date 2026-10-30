#include "machine/slots.hpp"

#include "machine/disk2.hpp"
#include "machine/mockingboard.hpp"
#include "machine/mouse.hpp"

namespace tisple {

namespace {

CardKind KindAt(const Apple2& a, int slot) {
    if (slot < 1 || slot > 7) {
        return CardKind::kEmpty;
    }
    return a.slots.kind[slot];
}

} // namespace

uint8_t CardIoAccess(Apple2& a, int slot, int reg, bool is_write, uint8_t value, bool& drives) {
    drives = false;
    switch (KindAt(a, slot)) {
    case CardKind::kTestCard: {
        TestCardState& card = a.slots.test_card;
        card.io_accesses++;
        if (is_write) {
            card.regs[reg] = value;
            return 0;
        }
        drives = true;
        return card.regs[reg];
    }
    case CardKind::kDisk2:
        return Disk2IoAccess(a, reg, is_write, value, drives);
    case CardKind::kMouse:
        return MouseIoAccess(a, reg, is_write, value, drives);
    default:
        return 0;
    }
}

uint8_t CardRomAccess(Apple2& a, int slot, int offset, bool is_write, uint8_t value, bool& drives) {
    drives = false;
    switch (KindAt(a, slot)) {
    case CardKind::kTestCard:
        drives = !is_write;
        return a.slots.test_card.rom[offset];
    case CardKind::kDisk2:
        drives = !is_write;
        return a.slots.disk2.rom[offset];
    case CardKind::kMouse:
        drives = !is_write;
        return MouseRomByte(a, offset);
    case CardKind::kMockingboard:
        return MockingboardRomAccess(a, offset, is_write, value, drives);
    default:
        return 0;
    }
}

uint8_t CardExpansionAccess(Apple2& a, int slot, int offset, bool is_write, uint8_t value,
                            bool& drives) {
    (void)value;
    drives = false;
    switch (KindAt(a, slot)) {
    case CardKind::kTestCard:
        drives = !is_write;
        return a.slots.test_card.expansion[offset];
    default:
        return 0;
    }
}

bool CardHasRom(const Apple2& a, int slot, bool expansion) {
    switch (KindAt(a, slot)) {
    case CardKind::kTestCard:
        return true;
    case CardKind::kDisk2:
    case CardKind::kMouse:
        return !expansion; // no $C800 rom
    default:
        return false;
    }
}

uint8_t CardRomPeek(const Apple2& a, int slot, uint16_t addr) {
    switch (KindAt(a, slot)) {
    case CardKind::kTestCard:
        if (addr >= 0xC800) {
            return a.slots.test_card.expansion[addr - 0xC800];
        }
        return a.slots.test_card.rom[addr & 0xFF];
    case CardKind::kDisk2:
        return addr >= 0xC800 ? 0 : a.slots.disk2.rom[addr & 0xFF];
    case CardKind::kMouse:
        return addr >= 0xC800 ? 0 : MouseRomByte(a, addr & 0xFF);
    default:
        return 0;
    }
}

void ResetCards(Apple2& a) {
    for (int slot = 1; slot <= 7; slot++) {
        if (a.slots.kind[slot] == CardKind::kDisk2) {
            ResetDisk2(a);
        }
        if (a.slots.kind[slot] == CardKind::kMouse) {
            ResetMouse(a);
        }
        if (a.slots.kind[slot] == CardKind::kMockingboard) {
            ResetMockingboard(a);
        }
    }
}

void InsertCard(Apple2& a, int slot, CardKind kind) {
    if (slot >= 1 && slot <= 7) {
        a.slots.kind[slot] = kind;
    }
}

} // namespace tisple
