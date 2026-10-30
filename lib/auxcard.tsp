; the aux language card: the extended 80-column card's own 16 KB at $D000-$FFFF, which ALTZP
; switches in (with aux memory's zero page and stack) in place of main memory's card. Nothing
; but machine code that runs with ALTZP on reaches it: lib/unpack's aux_to_card and
; main_to_card put bytes there, lib/dhgr's runs draws from there (dhgr.FROM_CARD).
; (import auxcard) then
;   (auxcard.call addr x)  machine code at addr run with ALTZP on, X = x: the card's bank 2 RAM
;                          in for reading and writing, lib/auxmem's zero page arguments
;                          (work, pages, from, to, rest: $F2-$FE) copied to aux memory's zero
;                          page, interrupts off; afterwards ALTZP off, the card (rom, or bank 1)
;                          and the interrupt flag as they were.
; The code at addr must be in main memory below $C000 (ALTZP switches the card's ram) and run
; on aux memory's stack. It may let interrupts in for a moment with ALTZP off: main memory's
; zero page and stack are as they were, bank 2's ram is in and its $FFFE vector reaches the
; handler (in a program not in the card, call points it at lib/auxmem's guard, so call
; (auxmem.guard) first, as for any aux memory code with interrupts on). lib/dhgr's runs does,
; between rows.

(import auxmem)

; our 6502 code at $1D8A in main memory, after lib/unpack's: the card's state pushed ($C011 bit
; 7: bank 2 in, $C012 bit 7: its ram in), bank 2's ram in for reading and writing (two reads of
; $C083), for a program in the rom the ram's IRQ vector to the guard ($0340), the zero page
; arguments to aux memory's, ALTZP on, the call (its operand written by call), ALTZP off, then
; the rom ($C082) or bank 1 ($C08B twice) back when they were in
(const HOME u16 0x1D8A)
(const CALL u16 0x1DBC) ; the JSR's operand
(const CODE_SIZE u8 75)
(let home (array u8 CODE_SIZE) :address 0x1D8A :reserve)

(const CODE (array u8 75)
    (make (array u8 75)
        0x08 0x78 0xAD 0x11 0xC0 0x48 ; card: PHP / SEI / LDA $C011 / PHA
        0xAD 0x12 0xC0 0x48           ; LDA $C012 / PHA
        0x2C 0x83 0xC0 0x2C 0x83 0xC0 ; BIT $C083 / BIT $C083
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
        0x68 0x30 0x06                ; was_ram: PLA / BMI was_2
        0x2C 0x8B 0xC0 0x2C 0x8B 0xC0 ; BIT $C08B / BIT $C08B
        0x28 0x60))                   ; was_2: PLP / RTS

(let ready bool false)

; the code at addr run with ALTZP on (above), X = x_reg
(fn call ((addr u16) (x_reg u8)) void
    (if (not ready)
        (do
            (let p (ptr u8) (as (ptr u8) HOME))
            (for i u8 0 CODE_SIZE
                (store (+ p i) (at CODE i)))
            (set ready true)))
    (store (as (ptr u8) CALL) (as u8 addr))
    (store (as (ptr u8) (+ CALL 1)) (as u8 (>> addr 8)))
    (jsr HOME (x x_reg)))
