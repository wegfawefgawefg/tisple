#include "backend/gen.hpp"

// 16-bit adds with bytes known before running. A result byte that is the sum of two numbers
// is a number, with no code; one that adds 0 to another byte (with no carry coming in) is
// that byte, read where it already is; only the rest is computed into the destination. So
// $2000 + (y << 10) does one byte of work, and a widened byte plus a value whose low byte is
// 0 passes the low byte straight through.

namespace tisple {

namespace {

// -1 unless byte k is a plain number
int KnownByte(const Opnd& o, int k) {
    const ByteRef& b = o.b[k];
    if (o.in_a || b.kind != ByteKind::Imm || b.value.sym >= 0) {
        return -1;
    }
    return b.value.offset & 0xFF;
}

// a byte the result can name as it is: a number, or a plain memory byte
bool Passable(const ByteRef& b) {
    return b.kind == ByteKind::Imm || b.kind == ByteKind::Mem;
}

enum class From : std::uint8_t {
    Number,
    A,
    B,
    Computed,
};

} // namespace

Opnd Add16(Gen& g, const Opnd& a, const Opnd& b, const Opnd* dst) {
    Opnd d = dst ? *dst : NewTemp(g, 2);
    if (a.in_a || b.in_a || a.size != 2 || b.size != 2) {
        Combine16(g, Op::Add, a, b, d);
        return d;
    }
    From from[2] = {From::Computed, From::Computed};
    int number[2] = {0, 0};
    bool carry = false;
    for (int k = 0; k < 2 && !carry; k++) {
        int x = KnownByte(a, k);
        int y = KnownByte(b, k);
        if (x >= 0 && y >= 0 && x + y <= 0xFF) {
            from[k] = From::Number;
            number[k] = x + y;
        } else if (x == 0 && Passable(b.b[k])) {
            from[k] = From::B;
        } else if (y == 0 && Passable(a.b[k])) {
            from[k] = From::A;
        } else {
            carry = true;
        }
    }
    if (from[0] == From::Computed) {
        Combine16(g, Op::Add, a, b, d);
        return d;
    }
    // byte 1 is computed into d after byte 0 is named: d must not be where byte 0 is
    if (from[1] == From::Computed) {
        const Opnd& named = from[0] == From::A ? a : b;
        if (from[0] != From::Number && MayChangeRead(g, named, 0, d, 1)) {
            d = NewTemp(g, 2);
        }
        LoadA(g, a, 1);
        if (!CarryClearNow(g)) {
            Emit(g, Mn::CLC, Mode::Imp);
        }
        OpA(g, Mn::ADC, b, 1);
        StoreA(g, d, 1);
    }
    Opnd r = d;
    r.temp = d.temp;
    r.moved = false;
    for (int k = 0; k < 2; k++) {
        switch (from[k]) {
        case From::Number:
            r.b[k] = ByteRef();
            r.b[k].value = Num(number[k]);
            break;
        case From::A:
            r.b[k] = a.b[k];
            r.temp = r.temp && a.temp;
            r.moved = r.moved || a.moved;
            break;
        case From::B:
            r.b[k] = b.b[k];
            r.temp = r.temp && b.temp;
            r.moved = r.moved || b.moved;
            break;
        case From::Computed:
            break;
        }
    }
    return r;
}

} // namespace tisple
