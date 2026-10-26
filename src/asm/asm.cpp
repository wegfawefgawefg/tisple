#include "asm/asm.hpp"

#include <cstdio>

namespace tisple {

namespace {

std::string Hex2(std::uint32_t value) {
    char text[8];
    std::snprintf(text, sizeof(text), "$%02X", value & 0xFF);
    return text;
}

std::string Hex4(std::uint32_t value) {
    char text[8];
    std::snprintf(text, sizeof(text), "$%04X", value & 0xFFFF);
    return text;
}

// the operand's value; false if its symbol is not defined yet
bool Resolve(const Assembly& as, const AsmValue& value, std::uint32_t& out) {
    std::int64_t number = value.offset;
    if (value.sym >= 0) {
        const AsmSymbol& sym = as.symbols[static_cast<std::size_t>(value.sym)];
        if (!sym.defined) {
            return false;
        }
        number += sym.value;
    }
    std::uint32_t bits = static_cast<std::uint32_t>(number) & 0xFFFF;
    if (value.part == Part::Low) {
        bits &= 0xFF;
    } else if (value.part == Part::High) {
        bits = (bits >> 8) & 0xFF;
    }
    out = bits;
    return true;
}

bool KnownZp(const Assembly& as, const AsmValue& value) {
    if (value.part != Part::Whole) {
        return false;
    }
    if (value.sym < 0) {
        return value.offset >= 0 && value.offset < 0x100;
    }
    return as.symbols[static_cast<std::size_t>(value.sym)].zp;
}

int ItemSize(const Assembly& as, const AsmItem& item) {
    switch (item.kind) {
    case ItemKind::Ins:
        if (item.mode == Mode::Rel && item.long_branch) {
            return 5;
        }
        return ModeSize(EncodedMode(as, item));
    case ItemKind::Byte:
        return 1;
    default:
        return 0;
    }
}

std::string SymbolText(const Assembly& as, const AsmValue& value) {
    if (value.sym < 0) {
        return "";
    }
    std::string text = as.symbols[static_cast<std::size_t>(value.sym)].name;
    if (value.offset > 0) {
        text += "+" + std::to_string(value.offset);
    } else if (value.offset < 0) {
        text += std::to_string(value.offset);
    }
    if (value.part == Part::Low) {
        text = "<" + text;
    } else if (value.part == Part::High) {
        text = ">" + text;
    }
    return text;
}

std::string OperandText(Mode mode, std::uint32_t v) {
    switch (mode) {
    case Mode::Imp:
        return "";
    case Mode::Acc:
        return " A";
    case Mode::Imm:
        return " #" + Hex2(v);
    case Mode::Zp:
        return " " + Hex2(v);
    case Mode::ZpX:
        return " " + Hex2(v) + ",X";
    case Mode::ZpY:
        return " " + Hex2(v) + ",Y";
    case Mode::Abs:
    case Mode::Rel:
        return " " + Hex4(v);
    case Mode::AbsX:
        return " " + Hex4(v) + ",X";
    case Mode::AbsY:
        return " " + Hex4(v) + ",Y";
    case Mode::Ind:
        return " (" + Hex4(v) + ")";
    case Mode::IndX:
        return " (" + Hex2(v) + ",X)";
    case Mode::IndY:
        return " (" + Hex2(v) + "),Y";
    default:
        return "";
    }
}

} // namespace

std::int32_t NewSymbol(Assembly& as, const std::string& name) {
    AsmSymbol sym;
    sym.name = name;
    as.symbols.push_back(sym);
    return static_cast<std::int32_t>(as.symbols.size() - 1);
}

void DefineSymbol(Assembly& as, std::int32_t sym, std::uint32_t value) {
    AsmSymbol& s = as.symbols[static_cast<std::size_t>(sym)];
    s.defined = true;
    s.value = value;
}

AsmItem MakeIns(Mn mn, Mode mode, AsmValue value) {
    AsmItem item;
    item.kind = ItemKind::Ins;
    item.mn = mn;
    item.mode = mode;
    item.value = value;
    return item;
}

AsmItem MakeLabel(std::int32_t sym) {
    AsmItem item;
    item.kind = ItemKind::Label;
    item.value.sym = sym;
    return item;
}

AsmItem MakeByte(AsmValue value) {
    AsmItem item;
    item.kind = ItemKind::Byte;
    item.value = value;
    return item;
}

AsmValue Num(std::int32_t number) {
    AsmValue value;
    value.offset = number;
    return value;
}

AsmValue SymValue(std::int32_t sym, std::int32_t offset, Part part) {
    AsmValue value;
    value.sym = sym;
    value.offset = offset;
    value.part = part;
    return value;
}

Mode EncodedMode(const Assembly& as, const AsmItem& item) {
    if (item.mode == Mode::Abs && KnownZp(as, item.value) && OpcodeFor(item.mn, Mode::Zp) >= 0) {
        return Mode::Zp;
    }
    return item.mode;
}

std::uint32_t LayoutCode(Assembly& as, std::uint16_t origin) {
    for (AsmItem& item : as.items) {
        item.long_branch = false;
    }
    std::uint32_t end = origin;
    // grow branches until every one reaches; sizes only grow, so this ends
    for (int round = 0; round < 1000; round++) {
        std::uint32_t pc = origin;
        for (AsmItem& item : as.items) {
            item.address = static_cast<std::uint16_t>(pc & 0xFFFF);
            item.size = static_cast<std::uint8_t>(ItemSize(as, item));
            if (item.kind == ItemKind::Label) {
                DefineSymbol(as, item.value.sym, pc);
            }
            pc += item.size;
        }
        end = pc;
        bool changed = false;
        for (AsmItem& item : as.items) {
            if (item.kind != ItemKind::Ins || item.mode != Mode::Rel || item.long_branch) {
                continue;
            }
            std::uint32_t target = 0;
            if (!Resolve(as, item.value, target)) {
                continue;
            }
            std::int32_t delta = static_cast<std::int32_t>(target) - (item.address + 2);
            if (delta < -128 || delta > 127) {
                item.long_branch = true;
                changed = true;
            }
        }
        if (!changed) {
            break;
        }
    }
    return end;
}

bool EncodeCode(const Assembly& as, std::vector<std::uint8_t>& out, std::string& error) {
    out.clear();
    for (const AsmItem& item : as.items) {
        if (item.kind == ItemKind::Label || item.kind == ItemKind::Note) {
            continue;
        }
        std::uint32_t v = 0;
        bool has_operand =
            item.kind == ItemKind::Byte || (item.mode != Mode::Imp && item.mode != Mode::Acc);
        if (has_operand && !Resolve(as, item.value, v)) {
            error = "undefined symbol " + as.symbols[static_cast<std::size_t>(item.value.sym)].name;
            return false;
        }
        if (item.kind == ItemKind::Byte) {
            out.push_back(static_cast<std::uint8_t>(v & 0xFF));
            continue;
        }
        Mode mode = EncodedMode(as, item);
        if (mode == Mode::Rel && item.long_branch) {
            // inverted branch over a JMP
            out.push_back(static_cast<std::uint8_t>(OpcodeFor(InvertBranch(item.mn), Mode::Rel)));
            out.push_back(3);
            out.push_back(static_cast<std::uint8_t>(OpcodeFor(Mn::JMP, Mode::Abs)));
            out.push_back(static_cast<std::uint8_t>(v & 0xFF));
            out.push_back(static_cast<std::uint8_t>(v >> 8));
            continue;
        }
        int opcode = OpcodeFor(item.mn, mode);
        if (opcode < 0) {
            error = std::string("no ") + MnName(item.mn) + " in this addressing mode";
            return false;
        }
        out.push_back(static_cast<std::uint8_t>(opcode));
        int size = ModeSize(mode);
        if (mode == Mode::Rel) {
            std::int32_t delta = static_cast<std::int32_t>(v) - (item.address + 2);
            out.push_back(static_cast<std::uint8_t>(delta & 0xFF));
            continue;
        }
        if (size == 2 && mode != Mode::Imm && v > 0xFF) {
            error = std::string(MnName(item.mn)) + " " + SymbolText(as, item.value) +
                    ": zero-page operand " + Hex4(v) + " is not in zero page";
            return false;
        }
        if (size >= 2) {
            out.push_back(static_cast<std::uint8_t>(v & 0xFF));
        }
        if (size == 3) {
            out.push_back(static_cast<std::uint8_t>(v >> 8));
        }
    }
    return true;
}

std::string ItemText(const Assembly& as, const AsmItem& item) {
    std::uint32_t v = 0;
    bool known = Resolve(as, item.value, v);
    std::string sym = SymbolText(as, item.value);
    if (item.kind == ItemKind::Byte) {
        return ".byte " + Hex2(v) + (sym.empty() ? "" : "  ; " + sym);
    }
    if (item.kind != ItemKind::Ins) {
        return "";
    }
    Mode mode = EncodedMode(as, item);
    std::string text = MnName(item.mn);
    if (mode == Mode::Rel && item.long_branch) {
        text = std::string(MnName(InvertBranch(item.mn))) + " *+5 / JMP";
        mode = Mode::Abs;
    }
    text += known ? OperandText(mode, v) : " ?";
    if (!sym.empty() && mode != Mode::Rel) {
        text += "  ; " + sym;
    }
    return text;
}

int ItemCycles(const Assembly& as, const AsmItem& item) {
    if (item.kind != ItemKind::Ins) {
        return 0;
    }
    Mode mode = EncodedMode(as, item);
    int opcode = OpcodeFor(item.mn, mode);
    if (opcode < 0) {
        return 0;
    }
    return OpcodeInfoFor(static_cast<std::uint8_t>(opcode)).cycles;
}

} // namespace tisple
