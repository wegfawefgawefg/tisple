#include "machine/slots.hpp"

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
    default:
        return 0;
    }
}

uint8_t CardRomAccess(Apple2& a, int slot, int offset, bool is_write, uint8_t value, bool& drives) {
    (void)value;
    drives = false;
    switch (KindAt(a, slot)) {
    case CardKind::kTestCard:
        drives = !is_write;
        return a.slots.test_card.rom[offset];
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
    (void)expansion;
    return KindAt(a, slot) == CardKind::kTestCard;
}

uint8_t CardRomPeek(const Apple2& a, int slot, uint16_t addr) {
    switch (KindAt(a, slot)) {
    case CardKind::kTestCard:
        if (addr >= 0xC800) {
            return a.slots.test_card.expansion[addr - 0xC800];
        }
        return a.slots.test_card.rom[addr & 0xFF];
    default:
        return 0;
    }
}

void InsertCard(Apple2& a, int slot, CardKind kind) {
    if (slot >= 1 && slot <= 7) {
        a.slots.kind[slot] = kind;
    }
}

} // namespace tisple
