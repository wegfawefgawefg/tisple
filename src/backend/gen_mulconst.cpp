#include "backend/gen.hpp"

// Multiplying by a constant: shifts and adds, top digit first (Horner), the multiplicand read
// once per nonzero digit. The constant's digits are its binary ones or its signed digits (the
// non-adjacent form, digits 1, 0 and -1 with no two nonzero side by side: 125 is 128 - 4 + 1),
// whichever does less; a -1 digit subtracts. Only the digits below the product's width count,
// so a byte times any constant takes at most 4 nonzero digits and is always inline: 255 is -1.

namespace tisple {

namespace {

// 16-bit products of at most this many nonzero digits are inline; more call the routine
constexpr int kMaxInlineMulDigits = 4;

struct MulDigits {
    int digit[16] = {};
    int top = -1; // the highest nonzero digit
    int nonzero = 0;
};

MulDigits Binary(std::uint32_t c, int bits) {
    MulDigits d;
    for (int i = 0; i < bits; i++) {
        d.digit[i] = static_cast<int>((c >> i) & 1);
        if (d.digit[i] != 0) {
            d.top = i;
            d.nonzero++;
        }
    }
    return d;
}

// the non-adjacent form of c mod 2^bits
MulDigits NonAdjacent(std::uint32_t c, int bits) {
    MulDigits d;
    std::int64_t v = c & ((1u << bits) - 1);
    for (int i = 0; i < bits && v != 0; i++) {
        if ((v & 1) != 0) {
            d.digit[i] = (v & 3) == 1 ? 1 : -1;
            v -= d.digit[i];
            d.top = i;
            d.nonzero++;
        }
        v >>= 1;
    }
    return d;
}

// cycles of a byte multiply in A: a shift per digit below the top, an add or subtract per
// nonzero one, a negation when the top digit is -1
int ByteCost(const MulDigits& d) {
    return 2 * d.top + 5 * (d.nonzero - 1) + (d.digit[d.top] < 0 ? 6 : 0);
}

// the digits a byte multiply uses
MulDigits ByteDigits(std::uint32_t c) {
    MulDigits plain = Binary(c, 8);
    MulDigits signed_digits = NonAdjacent(c, 8);
    return ByteCost(signed_digits) < ByteCost(plain) ? signed_digits : plain;
}

// the digits a 16-bit multiply uses: the signed ones when they are fewer and the top digit is
// 1 (the 16-bit code starts from the multiplicand itself)
MulDigits WordDigits(std::uint32_t c) {
    MulDigits plain = Binary(c, 16);
    MulDigits signed_digits = NonAdjacent(c, 16);
    if (signed_digits.digit[signed_digits.top] > 0 && signed_digits.nonzero < plain.nonzero) {
        return signed_digits;
    }
    return plain;
}

// a byte (a's high byte a known 0) times a constant, mod 65536: the low byte shifts in A,
// the high byte in memory, and each digit adds or subtracts the byte with a carry or borrow
// into the high byte
bool MulByteConst16(Gen& g, const Opnd& a, const MulDigits& d, const Opnd& dst) {
    if (KnownImm(g, a, 1) != 0 || !IsPlainMemory(a) || KnownImm(g, a, 0) >= 0) {
        return false;
    }
    Opnd high = dst;
    high.size = 1;
    high.b[0] = dst.b[1];
    if (dst.b[1].kind != ByteKind::Mem || Overlaps(g, a, dst)) {
        high = NewTemp(g, 1);
    }
    Emit(g, Mn::LDA, Mode::Imm, Num(0));
    StoreA(g, high, 0);
    LoadA(g, a, 0);
    for (int bit = d.top - 1; bit >= 0; bit--) {
        Emit(g, Mn::ASL, Mode::Acc);
        RmwByte(g, Mn::ROL, high, 0);
        if (d.digit[bit] == 0) {
            continue;
        }
        std::int32_t skip = NewLabel(g);
        if (d.digit[bit] > 0) {
            Emit(g, Mn::CLC, Mode::Imp);
            OpA(g, Mn::ADC, a, 0);
            Branch(g, Mn::BCC, skip);
            RmwByte(g, Mn::INC, high, 0);
        } else {
            Emit(g, Mn::SEC, Mode::Imp);
            OpA(g, Mn::SBC, a, 0);
            Branch(g, Mn::BCS, skip);
            RmwByte(g, Mn::DEC, high, 0);
        }
        PlaceLabel(g, skip);
    }
    StoreA(g, dst, 0);
    if (high.b[0].kind != dst.b[1].kind || high.b[0].value.sym != dst.b[1].value.sym ||
        high.b[0].value.offset != dst.b[1].value.offset) {
        LoadA(g, high, 0);
        StoreA(g, dst, 1);
    }
    return true;
}

} // namespace

bool InlineMulConst16(std::uint32_t c) {
    return WordDigits(c & 0xFFFF).nonzero <= kMaxInlineMulDigits;
}

void MulConst8A(Gen& g, std::uint32_t c) {
    c &= 0xFF;
    if (c == 0) {
        Emit(g, Mn::LDA, Mode::Imm, Num(0));
        return;
    }
    MulDigits d = ByteDigits(c);
    Opnd x;
    if (d.nonzero > 1) {
        x = SpillA(g);
    }
    if (d.digit[d.top] < 0) {
        Emit(g, Mn::EOR, Mode::Imm, Num(0xFF));
        Emit(g, Mn::CLC, Mode::Imp);
        Emit(g, Mn::ADC, Mode::Imm, Num(1));
    }
    for (int bit = d.top - 1; bit >= 0; bit--) {
        Emit(g, Mn::ASL, Mode::Acc);
        if (d.digit[bit] > 0) {
            Emit(g, Mn::CLC, Mode::Imp);
            OpA(g, Mn::ADC, x, 0);
        } else if (d.digit[bit] < 0) {
            Emit(g, Mn::SEC, Mode::Imp);
            OpA(g, Mn::SBC, x, 0);
        }
    }
}

void MulConst16(Gen& g, const Opnd& a, std::uint32_t factor, const Opnd& dst) {
    factor &= 0xFFFF;
    if (factor == 0) {
        CopyOpnd(g, ImmOpnd(0, 2), dst);
        return;
    }
    MulDigits d = WordDigits(factor);
    if (MulByteConst16(g, a, d, dst)) {
        return;
    }
    // the product builds up in r, which shifts in place; x is read once per nonzero digit
    bool plain = dst.b[0].kind == ByteKind::Mem && dst.b[1].kind == ByteKind::Mem;
    Opnd r = plain ? dst : NewTemp(g, 2);
    Opnd x = a;
    bool stable = IsPlainMemory(a) && !Overlaps(g, a, r);
    if (d.nonzero > 1 && !a.temp && !IsImm(a) && !stable) {
        x = NewTemp(g, 2);
        CopyOpnd(g, a, x);
    }
    CopyOpnd(g, x, r);
    for (int bit = d.top - 1; bit >= 0; bit--) {
        RmwByte(g, Mn::ASL, r, 0);
        RmwByte(g, Mn::ROL, r, 1);
        if (d.digit[bit] != 0) {
            Combine16(g, d.digit[bit] > 0 ? Op::Add : Op::Sub, r, x, r);
        }
    }
    if (!plain) {
        CopyOpnd(g, r, dst);
    }
}

} // namespace tisple
