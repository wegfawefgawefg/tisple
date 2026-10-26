#!/usr/bin/env python3
"""Hand-written 6502 test programs for the built-in Apple IIe (tests/programs/machine_*.bin).

A tiny two-pass assembler, just enough for these listings. Run from the repo root:
    python3 scripts/make_machine_programs.py
The .bin files are committed; tests/test_machine_*.cpp load them.
"""

import os
import re
import sys

# mnemonic -> {mode: opcode}
OPS = {
    "LDA": {"imm": 0xA9, "zp": 0xA5, "abs": 0xAD, "absx": 0xBD, "absy": 0xB9, "indy": 0xB1},
    "LDX": {"imm": 0xA2, "zp": 0xA6, "abs": 0xAE},
    "LDY": {"imm": 0xA0, "zp": 0xA4, "abs": 0xAC},
    "STA": {"zp": 0x85, "abs": 0x8D, "absx": 0x9D, "absy": 0x99, "indy": 0x91},
    "STX": {"zp": 0x86, "abs": 0x8E},
    "STY": {"zp": 0x84, "abs": 0x8C},
    "AND": {"imm": 0x29},
    "ORA": {"imm": 0x09, "zp": 0x05},
    "EOR": {"imm": 0x49},
    "CMP": {"imm": 0xC9, "zp": 0xC5},
    "CPX": {"imm": 0xE0},
    "CPY": {"imm": 0xC0},
    "ADC": {"imm": 0x69},
    "BIT": {"zp": 0x24, "abs": 0x2C},
    "INC": {"zp": 0xE6, "abs": 0xEE},
    "DEC": {"zp": 0xC6},
    "JMP": {"abs": 0x4C},
    "JSR": {"abs": 0x20},
    "BNE": {"rel": 0xD0}, "BEQ": {"rel": 0xF0}, "BPL": {"rel": 0x10}, "BMI": {"rel": 0x30},
    "BCC": {"rel": 0x90}, "BCS": {"rel": 0xB0},
    "INX": {"imp": 0xE8}, "INY": {"imp": 0xC8}, "DEX": {"imp": 0xCA}, "DEY": {"imp": 0x88},
    "TAX": {"imp": 0xAA}, "TXA": {"imp": 0x8A}, "TAY": {"imp": 0xA8}, "TYA": {"imp": 0x98},
    "ASL": {"imp": 0x0A}, "CLC": {"imp": 0x18}, "SEC": {"imp": 0x38}, "NOP": {"imp": 0xEA},
    "RTS": {"imp": 0x60}, "BRK": {"imp": 0x00}, "PHA": {"imp": 0x48}, "PLA": {"imp": 0x68},
}


def number(text, labels):
    if text.startswith("$"):
        return int(text[1:], 16)
    if text.startswith("<"):
        return number(text[1:], labels) & 0xFF
    if text.startswith(">"):
        return number(text[1:], labels) >> 8
    if re.fullmatch(r"\d+", text):
        return int(text)
    return labels[text]


def assemble(origin, source):
    lines = []
    for raw in source.strip().splitlines():
        line = raw.split(";")[0].strip()
        if line:
            lines.append(line)
    labels = {}
    out = []
    for pass_no in (0, 1):
        pc = origin
        out = []
        for line in lines:
            if line.endswith(":"):
                labels[line[:-1]] = pc
                continue
            if line.startswith(".byte"):
                values = [number(v.strip(), labels) for v in line[5:].split(",")]
                out += values
                pc += len(values)
                continue
            parts = line.split(None, 1)
            mnem = parts[0].upper()
            operand = parts[1].replace(" ", "") if len(parts) > 1 else ""
            modes = OPS[mnem]
            if not operand:
                code = [modes["imp"]]
            elif "rel" in modes:
                target = labels.get(operand, pc + 2) if pass_no == 0 else labels[operand]
                offset = target - (pc + 2)
                if pass_no == 1 and not -128 <= offset <= 127:
                    sys.exit(f"branch out of range: {line}")
                code = [modes["rel"], offset & 0xFF]
            elif operand.startswith("#"):
                code = [modes["imm"], number(operand[1:], labels) & 0xFF]
            elif operand.startswith("(") and operand.endswith("),Y"):
                code = [modes["indy"], number(operand[1:-3], labels)]
            else:
                index = ""
                if operand.endswith(",X") or operand.endswith(",Y"):
                    index = operand[-1].lower()
                    operand = operand[:-2]
                try:
                    value = number(operand, labels)
                except KeyError:
                    value = 0xFFFF if pass_no == 0 else number(operand, labels)
                if not index and value < 0x100 and "zp" in modes:
                    code = [modes["zp"], value]
                else:
                    code = [modes["abs" + index], value & 0xFF, value >> 8]
            out += code
            pc += len(code)
    return bytes(out)


PROGRAMS = {}

# every screen code on text page 1: byte X at $400+X, $500+X, ... (normal, inverse, flash,
# lower case), then a message in normal text on row 23
PROGRAMS["text"] = """
    STA $C051       ; TXTSET
    STA $C054       ; page 1
    LDX #0
fill:
    TXA
    STA $0400,X
    STA $0500,X
    STA $0600,X
    STA $0700,X
    INX
    BNE fill
    LDX #0
msg:
    LDA message,X
    BEQ done
    STA $07D0,X     ; row 23
    INX
    BNE msg
done:
    BRK
message:
    .byte $D4,$C9,$D3,$D0,$CC,$C5,$A0,$DD,$DB,$A0,$F4,$E5,$F8,$F4,$A1,0
"""

# lo-res: each column x shows color x mod 16 in both halves of every byte
PROGRAMS["lores"] = """
    STA $C050       ; TXTCLR: graphics
    STA $C052       ; MIXCLR
    STA $C054       ; LOWSCR
    STA $C056       ; LORES
    LDX #0
fill:
    TXA
    AND #$0F
    STA $00
    ASL
    ASL
    ASL
    ASL
    ORA $00
    STA $0400,X
    STA $0500,X
    STA $0600,X
    STA $0700,X
    INX
    BNE fill
    BRK
"""

# hi-res stripes on page 1, by column: white, violet, green, blue, orange (8 columns each)
HIRES_FILL = """
    LDA #<{page}
    STA $00
    LDA #>{page}
    STA $01
    LDY #0
fill{n}:
    TYA
    AND #$7F
    TAX
    LDA {table},X
    STA ($00),Y
    INY
    BNE fill{n}
    INC $01
    LDA $01
    CMP #{end}
    BNE fill{n}
"""


def stripe_table():
    # bytes of one 128-byte group: three rows of 40 columns, then 8 screen-hole bytes
    pairs = [(0x7F, 0x7F), (0x55, 0x2A), (0x2A, 0x55), (0xD5, 0xAA), (0xAA, 0xD5)]
    row = [pairs[c // 8][c & 1] for c in range(40)]
    table = row * 3 + [0] * 8
    return ".byte " + ",".join(f"${b:02X}" for b in table)


def bars_table():
    # page 2: white bars two columns wide
    row = [0x7F if (c // 2) % 2 == 0 else 0x00 for c in range(40)]
    table = row * 3 + [0] * 8
    return ".byte " + ",".join(f"${b:02X}" for b in table)


PROGRAMS["hires"] = """
    STA $C050       ; graphics
    STA $C052       ; full screen
    STA $C054       ; page 1
    STA $C057       ; HIRES
""" + HIRES_FILL.format(page="$2000", end="$40", n=1, table="stripes") + """
    BRK
stripes:
""" + stripe_table() + "\n"

# mixed: the hi-res stripes with four text rows below, saying so
PROGRAMS["mixed"] = """
    STA $C050
    STA $C053       ; MIXSET
    STA $C054
    STA $C057
""" + HIRES_FILL.format(page="$2000", end="$40", n=1, table="stripes") + """
    LDX #0
msg:
    LDA message,X
    BEQ done
    STA $0650,X     ; row 20
    INX
    BNE msg
done:
    BRK
message:
    .byte $CD,$C9,$D8,$C5,$C4,$A0,$CD,$CF,$C4,$C5,$A0,$F4,$E5,$F8,$F4,0
stripes:
""" + stripe_table() + "\n"

# page 2: stripes on page 1, white bars on page 2, and page 2 shown
PROGRAMS["page2"] = """
    STA $C050
    STA $C052
    STA $C057
""" + HIRES_FILL.format(page="$2000", end="$40", n=1, table="stripes") + \
    HIRES_FILL.format(page="$4000", end="$60", n=2, table="bars") + """
    STA $C055       ; HISCR: show page 2
    BRK
stripes:
""" + stripe_table() + """
bars:
""" + bars_table() + "\n"

# speaker: 1000 clicks 469 cycles apart (a 544 Hz square wave, ~0.46 s), then RTS
PROGRAMS["speaker"] = """
    LDY #4
outer:
    LDX #250
click:
    LDA $C030
    LDA #57
    STA $02
delay:
    DEC $02         ; 8 cycles a pass
    BNE delay
    DEX
    BNE click
    DEY
    BNE outer
    RTS
"""

# keyboard: wait for a key, keep it at $0300 and clear the strobe; twice; then BRK
PROGRAMS["keys"] = """
    LDX #0
wait:
    LDA $C000
    BPL wait
    STA $0300,X
    STA $C010       ; clear the strobe
    INX
    CPX #2
    BNE wait
    LDA $C000       ; strobe now clear: bit 7 off
    STA $0302
    BRK
"""


def main():
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    out_dir = os.path.join(root, "tests", "programs")
    for name, source in PROGRAMS.items():
        code = assemble(0x6000, source)
        path = os.path.join(out_dir, f"machine_{name}.bin")
        with open(path, "wb") as f:
            f.write(code)
        print(f"{path}: {len(code)} bytes")


if __name__ == "__main__":
    main()
