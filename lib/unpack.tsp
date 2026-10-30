; packed tables back to bytes: what (tables.packed ...) and (tables.packed_bytes ...) made
; while compiling (ZX0), unpacked by our own 6502 code. (import unpack) then
;   (unpack.main_to_main dst src)  packed bytes at src in main memory, out to dst in main
;   (unpack.aux_to_aux dst src)    packed in aux memory (a let :in aux), out to aux memory
;   (unpack.aux_to_main dst src)   packed in aux, out to main
;   (unpack.main_to_aux dst src)   packed in main, out to aux
; with pointers from addr_of: (unpack.main_to_main (addr_of (at screen 0)) (addr_of (at PIC 0))).
; The packed bytes say how many come out; dst needs room for all of them. About 46 cycles a
; byte out on double hi-res pictures (docs/dhgr.md, Packed data). With an interrupt
; handler running, call (auxmem.guard) first, as for lib/auxmem, unless both are main.

(import auxmem)

; the unpacker, our own 6502 code: ZX0 (version 2: forward, the offset's high part inverted).
; In: X = mode (bit 0: the packed bytes in aux memory, bit 1: out to aux memory), auxmem.from
; the packed bytes (SRC), auxmem.to where they go (DST). It keeps Y = 0 between steps and
; borrows auxmem.pages and auxmem.rest (LENHI, LENLO: a length) and auxmem.work (RDSRC, RDDST:
; 0 or 1, the RAMRD for reading the packed bytes and for copying a match out of what is already
; unpacked; MP where a copy comes from; OFF minus the match offset; BITS the bits left, then a
; 1). RAMWRT stays on the output's memory throughout; RAMRD follows the packed bytes, and the
; output's memory while a match is copied. With RAMRD on the 6502 fetches its instructions from
; aux memory too, so the code runs from $1C00 with a copy at the same address in aux memory.
; Its steps: lit reads the literal run's length (a gamma code: a 1 ends it, a 0 is followed by
; the next bit down), one byte straight or more through copy; a 0 bit then means a match at the
; last offset, a 1 one at a new offset (newoff: its high part, inverted gamma, 256 the end; a
; byte with the low 7 bits; the length's first bit is that byte's bit 0).
(const HOME u16 0x1C00)
(const CODE_SIZE u16 394)
(let home_main (array u8 512) :address 0x1C00 :reserve)
(let home_aux (array u8 512) :address 0x1C00 :in aux :reserve)

(const CODE (array u8 394)
    (make (array u8 394)
        0x8A 0x29 0x01 0x85 0xF2 0x8A 0x4A ; unpack: TXA / AND #1 / STA RDSRC / TXA / LSR A
        0xAA 0x9D 0x04 0xC0 0x86 0xF3      ; TAX / STA $C004,X / STX RDDST
        0xA6 0xF2 0x9D 0x02 0xC0 0xA0 0x00 ; LDX RDSRC / STA $C002,X / LDY #0
        0xA9 0x80 0x85 0xF8 0xA9 0xFF      ; LDA #$80 / STA BITS / LDA #$FF
        0x85 0xF6 0x85 0xF7                ; STA OFF / STA OFF+1
        0x06 0xF8 0xD0 0x0B 0xB1 0xFA      ; lit: ASL BITS / BNE bit1 / LDA (SRC),Y
        0xE6 0xFA 0xD0 0x02 0xE6 0xFB      ; INC SRC / BNE fill1 / INC SRC+1
        0x2A 0x85 0xF8                     ; fill1: ROL A / STA BITS
        0x90 0x12 0xB1 0xFA 0x91 0xFC      ; bit1: BCC lits / LDA (SRC),Y / STA (DST),Y
        0xE6 0xFA 0xD0 0x02 0xE6 0xFB      ; INC SRC / BNE l1_dst / INC SRC+1
        0xE6 0xFC 0xD0 0x1A 0xE6 0xFD      ; l1_dst: INC DST / BNE lit_end / INC DST+1
        0xD0 0x16                          ; BNE lit_end
        0x20 0x1C 0x1D 0xA5 0xFA 0x85 0xF4 ; lits: JSR gamma_1 / LDA SRC / STA MP
        0xA5 0xFB 0x85 0xF5 0x20 0x55 0x1D ; LDA SRC+1 / STA MP+1 / JSR copy
        0xA5 0xF4 0x85 0xFA 0xA5 0xF5      ; LDA MP / STA SRC / LDA MP+1
        0x85 0xFB                          ; STA SRC+1
        0x06 0xF8 0xD0 0x0B 0xB1 0xFA      ; lit_end: ASL BITS / BNE bit2 / LDA (SRC),Y
        0xE6 0xFA 0xD0 0x02 0xE6 0xFB      ; INC SRC / BNE fill2 / INC SRC+1
        0x2A 0x85 0xF8                     ; fill2: ROL A / STA BITS
        0xB0 0x2E 0x20 0x02 0x1D           ; bit2: BCS newoff / JSR gamma
        0x18 0xA5 0xFC 0x65 0xF6 0x85 0xF4 ; match: CLC / LDA DST / ADC OFF / STA MP
        0xA5 0xFD 0x65 0xF7 0x85 0xF5      ; LDA DST+1 / ADC OFF+1 / STA MP+1
        0xA6 0xF3 0x9D 0x02 0xC0           ; LDX RDDST / STA $C002,X
        0x20 0x55 0x1D 0xA6 0xF2           ; JSR copy / LDX RDSRC
        0x9D 0x02 0xC0 0x06 0xF8 0xD0 0x0B ; STA $C002,X / ASL BITS / BNE bit3
        0xB1 0xFA 0xE6 0xFA 0xD0 0x02      ; LDA (SRC),Y / INC SRC / BNE fill3
        0xE6 0xFB                          ; INC SRC+1
        0x2A 0x85 0xF8                     ; fill3: ROL A / STA BITS
        0x90 0x88                          ; bit3: BCC lit
        0xA9 0x01 0x85 0xFE 0xA9 0x00      ; newoff: LDA #1 / STA LENLO / LDA #0
        0x85 0xF9                          ; STA LENHI
        0x06 0xF8 0xD0 0x0B 0xB1 0xFA      ; m_loop: ASL BITS / BNE bit4 / LDA (SRC),Y
        0xE6 0xFA 0xD0 0x02 0xE6 0xFB      ; INC SRC / BNE fill4 / INC SRC+1
        0x2A 0x85 0xF8                     ; fill4: ROL A / STA BITS
        0xB0 0x1B 0x06 0xF8 0xD0 0x0B      ; bit4: BCS m_done / ASL BITS / BNE bit5
        0xB1 0xFA 0xE6 0xFA 0xD0 0x02      ; LDA (SRC),Y / INC SRC / BNE fill5
        0xE6 0xFB                          ; INC SRC+1
        0x2A 0x85 0xF8                     ; fill5: ROL A / STA BITS
        0xA5 0xFE 0x2A 0x49 0x01 0x85 0xFE ; bit5: LDA LENLO / ROL A / EOR #1 / STA LENLO
        0x26 0xF9 0x4C 0x9E 0x1C           ; ROL LENHI / JMP m_loop
        0xA5 0xF9 0xD0 0x2D 0xA5 0xFE 0x4A ; m_done: LDA LENHI / BNE done / LDA LENLO / LSR A
        0x85 0xF7 0xA9 0x00 0x6A 0x85 0xF6 ; STA OFF+1 / LDA #0 / ROR A / STA OFF
        0xB1 0xFA 0xE6 0xFA 0xD0 0x02      ; LDA (SRC),Y / INC SRC / BNE o_low
        0xE6 0xFB                          ; INC SRC+1
        0xAA 0x4A 0x38 0xE5 0xF6 0x85 0xF6 ; o_low: TAX / LSR A / SEC / SBC OFF / STA OFF
        0xA9 0x00 0xE5 0xF7 0x85 0xF7 0x8A ; LDA #0 / SBC OFF+1 / STA OFF+1 / TXA
        0x4A 0x20 0x26 0x1D 0xE6 0xFE      ; LSR A / JSR gamma_c / INC LENLO
        0xD0 0x02 0xE6 0xF9                ; BNE o_go / INC LENHI
        0x4C 0x6B 0x1C                     ; o_go: JMP match
        0x8D 0x02 0xC0 0x8D 0x04 0xC0 0x60 ; done: STA $C002 / STA $C004 / RTS
        0xA9 0x01 0x85 0xFE 0xA9 0x00      ; gamma: LDA #1 / STA LENLO / LDA #0
        0x85 0xF9 0x06 0xF8 0xD0 0x0B      ; STA LENHI / ASL BITS / BNE bit6
        0xB1 0xFA 0xE6 0xFA 0xD0 0x02      ; LDA (SRC),Y / INC SRC / BNE fill6
        0xE6 0xFB                          ; INC SRC+1
        0x2A 0x85 0xF8                     ; fill6: ROL A / STA BITS
        0x90 0x15 0x60                     ; bit6: BCC g_more / RTS
        0xA9 0x01 0x85 0xFE 0xA9 0x00      ; gamma_1: LDA #1 / STA LENLO / LDA #0
        0x85 0xF9 0xF0 0x0A                ; STA LENHI / BEQ g_more
        0xA9 0x01 0x85 0xFE 0xA9 0x00      ; gamma_c: LDA #1 / STA LENLO / LDA #0
        0x85 0xF9 0xB0 0x24                ; STA LENHI / BCS g_done
        0x06 0xF8 0xD0 0x0B 0xB1 0xFA      ; g_more: ASL BITS / BNE bit7 / LDA (SRC),Y
        0xE6 0xFA 0xD0 0x02 0xE6 0xFB      ; INC SRC / BNE fill7 / INC SRC+1
        0x2A 0x85 0xF8                     ; fill7: ROL A / STA BITS
        0x26 0xFE 0x26 0xF9 0x06 0xF8      ; bit7: ROL LENLO / ROL LENHI / ASL BITS
        0xD0 0x0B 0xB1 0xFA 0xE6 0xFA      ; BNE bit8 / LDA (SRC),Y / INC SRC
        0xD0 0x02 0xE6 0xFB                ; BNE fill8 / INC SRC+1
        0x2A 0x85 0xF8                     ; fill8: ROL A / STA BITS
        0x90 0xDC                          ; bit8: BCC g_more
        0x60                               ; g_done: RTS
        0xA6 0xF9 0xF0 0x0E                ; copy: LDX LENHI / BEQ c_rest
        0xB1 0xF4 0x91 0xFC 0xC8 0xD0 0xF9 ; c_page: LDA (MP),Y / STA (DST),Y / INY / BNE c_page
        0xE6 0xF5 0xE6 0xFD 0xCA 0xD0 0xF2 ; INC MP+1 / INC DST+1 / DEX / BNE c_page
        0xA6 0xFE 0xF0 0x1E                ; c_rest: LDX LENLO / BEQ c_done
        0xB1 0xF4 0x91 0xFC 0xC8 0xCA      ; c_loop: LDA (MP),Y / STA (DST),Y / INY / DEX
        0xD0 0xF8 0x98 0x18 0x65 0xF4      ; BNE c_loop / TYA / CLC / ADC MP
        0x85 0xF4 0x90 0x02 0xE6 0xF5      ; STA MP / BCC c_dst / INC MP+1
        0x98 0x18 0x65 0xFC 0x85 0xFC      ; c_dst: TYA / CLC / ADC DST / STA DST
        0x90 0x02 0xE6 0xFD                ; BCC c_end / INC DST+1
        0xA0 0x00                          ; c_end: LDY #0
        0x60))                             ; c_done: RTS

(const MAIN_TO_MAIN u8 0)
(const AUX_TO_MAIN u8 1)
(const MAIN_TO_AUX u8 2)
(const AUX_TO_AUX u8 3)

(let in_main bool false)
(let in_aux bool false)

; the code to $1C00 in main memory, and in aux memory too when the mode reaches it
(fn setup ((mode u8)) void
    (if (not in_main)
        (do
            (let home (ptr u8) (as (ptr u8) HOME))
            (for i u16 0 CODE_SIZE
                (store (+ home i) (at CODE i)))
            (set in_main true)))
    (if (and (!= mode MAIN_TO_MAIN) (not in_aux))
        (do
            (auxmem.write (as (ptr u8) HOME) (addr_of (at CODE 0)) CODE_SIZE)
            (set in_aux true))))

(fn run ((dst (ptr u8)) (src (ptr u8)) (mode u8)) void
    (setup mode)
    (set auxmem.from src)
    (set auxmem.to dst)
    (jsr HOME (x mode)))

(fn main_to_main ((dst (ptr u8)) (src (ptr u8))) void
    (run dst src MAIN_TO_MAIN))

(fn aux_to_aux ((dst (ptr u8)) (src (ptr u8))) void
    (run dst src AUX_TO_AUX))

(fn aux_to_main ((dst (ptr u8)) (src (ptr u8))) void
    (run dst src AUX_TO_MAIN))

(fn main_to_aux ((dst (ptr u8)) (src (ptr u8))) void
    (run dst src MAIN_TO_AUX))
