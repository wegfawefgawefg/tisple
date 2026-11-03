; the aux language card: the extended 80-column card's own 16 KB at $D000-$FFFF, which ALTZP
; switches in (with aux memory's zero page and stack) in place of main memory's card. Nothing
; but machine code that runs with ALTZP on reaches it: lib/unpack's aux_to_card and
; main_to_card put bytes there, lib/dhgr's runs draws from there (dhgr.FROM_CARD).
; (import auxcard) then
;   (auxcard.call addr x bank)  machine code at addr run with ALTZP on, X = x: the card's
;                          bank (auxcard.BANK2, $D000-$FFFF, or BANK1: $D000-$DFFF its own,
;                          $E000-$FFFF shared) in for reading and writing, lib/auxmem's zero page arguments
;                          (work, pages, from, to, rest: $F2-$FE) copied to aux memory's zero
;                          page, interrupts off; afterwards ALTZP off, the card (rom, or bank 1)
;                          and the interrupt flag as they were.
; The code at addr must be in main memory below $C000 (ALTZP switches the card's ram) and run
; on aux memory's stack. It may let interrupts in for a moment with ALTZP off: main memory's
; zero page and stack are as they were, the card's ram is in and its $FFFE vector reaches the
; handler (in a program not in the card, call points it at lib/auxmem's guard, so call
; (auxmem.guard) first, as for any aux memory code with interrupts on). lib/dhgr's runs does,
; between rows, and lib/unpack's card modes at each page out.

(import auxmem)

; our 6502 code at $1DA6 in main memory, after lib/unpack's: the card's state pushed ($C011 bit
; 7: bank 2 in, $C012 bit 7: its ram in), the bank asked for in for reading and writing (two
; reads of $C083 or $C08B), for a program in the rom the ram's IRQ vector to the guard ($0340),
; the zero page arguments to aux memory's, ALTZP on, the call (its operand written by call),
; ALTZP off, then the rom ($C082) or the bank that was in back (one read: the card stays
; written, as an odd read twice made it)
(const HOME u16 0x1DA6)
(const CALL u16 0x1DD8)   ; the JSR's operand
(const SWITCH u16 0x1DB1) ; and the two BITs' low bytes, $1DB1 and $1DB4: the bank to switch in
(const BANK2 u8 0x83)     ; banks by their switch: $C083 bank 2, $C08B bank 1, both ram read
(const BANK1 u8 0x8B)     ; and written
(const CODE_SIZE u8 77)
(const SWITCH_AT u8 11) ; SWITCH and CALL in the code, from HOME
(const CALL_AT u8 50)

; the code at its home, loaded there with the program (a boot disk's loader puts it there;
; other builds' startup copies it in), so it costs no other bytes
(let code (array u8 77)
    (make (array u8 77)
        0x08 0x78 0xAD 0x11 0xC0 0x48 ; card: PHP / SEI / LDA $C011 / PHA
        0xAD 0x12 0xC0 0x48           ; LDA $C012 / PHA
        0x2C 0x83 0xC0 0x2C 0x83 0xC0 ; BIT $C083 / BIT $C083 (or $C08B: the bank asked for)
        0x0A 0xB0 0x0A                ; ASL A / BCS ram
        0xA9 0x40 0x8D 0xFE 0xFF      ; LDA #<GUARD / STA $FFFE
        0xA9 0x03 0x8D 0xFF 0xFF      ; LDA #>GUARD / STA $FFFF
        0xA0 0x0C                     ; ram: LDY #12
        0xB9 0xF2 0x00 0x8D 0x09 0xC0 ; copy: LDA $00F2,Y / STA $C009: ALTZP on
        0x99 0xF2 0x00 0x8D 0x08 0xC0 ; STA $00F2,Y / STA $C008: off
        0x88 0x10 0xF1                ; DEY / BPL copy
        0x8D 0x09 0xC0 0x20 0x00 0x00 ; STA $C009 / JSR addr
        0x8D 0x08 0xC0                ; STA $C008
        0x68 0x30 0x06                ; PLA / BMI was_ram
        0x68 0x2C 0x82 0xC0 0x28 0x60 ; PLA / BIT $C082 / PLP / RTS
        0x68 0x30 0x05                ; was_ram: PLA / BMI was_2
        0x2C 0x8B 0xC0 0x28 0x60      ; BIT $C08B / PLP / RTS
        0x2C 0x83 0xC0 0x28 0x60)     ; was_2: BIT $C083 / PLP / RTS
    :address 0x1DA6)

; the code at addr run with ALTZP on (above), X = x_reg, bank BANK2 or BANK1 in
; (its bytes patched through code, which keeps it in the program)
(fn call ((addr u16) (x_reg u8) (bank u8)) void
    (set (at code SWITCH_AT) bank)
    (set (at code (+ SWITCH_AT 3)) bank)
    (set (at code CALL_AT) (as u8 addr))
    (set (at code (+ CALL_AT 1)) (as u8 (>> addr 8)))
    (jsr HOME (x x_reg)))
