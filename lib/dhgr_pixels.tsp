; double hi-res drawn from colour pixels, a row at a time: (import dhgr_pixels) then
;   (dhgr_pixels.row page col top groups pixels)  groups 7-pixel groups (1-20) of row top of
;                                   page from byte column col (even) on: each pixel a byte of
;                                   main memory at pixels, its colour 0-15; whole bytes written
;                                   in both memories, what was there gone
; For a hud drawn in whole pixels (text in colour pixels, badges, cards): the program builds a
; row of pixels, then this writes it, about 210 cycles a group. Our 6502 code, run where the
; layout puts it (RAMRD stays off, so it may be anywhere): per group the 4 bytes worked out with
; shifts and three small tables (a colour's low bits moved up into the next byte), then main
; memory's two written, RAMWRT on, aux memory's two, RAMWRT off. Its three table reads point at
; the tables after it, set the first time. With an interrupt handler, (auxmem.guard) first, as
; for lib/auxmem.

(import auxmem)
(import dhgr)

; zero page: auxmem.from the pixels (SRC), auxmem.to the screen byte (DST), auxmem.rest the row,
; work 4 the byte column, 5 the groups (W), 6 the page; work 0-3 and auxmem.pages scratch
(let code (array u8 172)
    (make (array u8 172)
        0x20 0x12 0x1E 0xA0 0x00                  ; pixels: JSR row_at / LDY #0
        0xB1 0xFA 0x85 0xF2 0xC8 0xB1 0xFA        ; group: LDA (SRC),Y / STA T / INY / LDA (SRC),Y
        0xAA 0xBD 0x7C 0x00 0x05 0xF2             ; TAX / LDA LO4,X / ORA T
        0x85 0xF3 0x8A 0x4A 0x4A 0x4A             ; STA B0 / TXA / LSR A / LSR A / LSR A
        0x85 0xF2 0xC8 0xB1 0xFA 0x0A             ; STA T / INY / LDA (SRC),Y / ASL A
        0x05 0xF2 0x85 0xF2 0xC8 0xB1 0xFA        ; ORA T / STA T / INY / LDA (SRC),Y
        0xAA 0xBD 0x8C 0x00 0x05 0xF2             ; TAX / LDA LO5,X / ORA T
        0x85 0xF4 0x8A 0x4A 0x4A 0x85 0xF2        ; STA B1 / TXA / LSR A / LSR A / STA T
        0xC8 0xB1 0xFA 0x0A 0x0A 0x05 0xF2        ; INY / LDA (SRC),Y / ASL A / ASL A / ORA T
        0x85 0xF2 0xC8 0xB1 0xFA 0xAA             ; STA T / INY / LDA (SRC),Y / TAX
        0xBD 0x9C 0x00 0x05 0xF2 0x85 0xF5        ; LDA LO6,X / ORA T / STA B2
        0x8A 0x4A 0x85 0xF2 0xC8 0xB1 0xFA        ; TXA / LSR A / STA T / INY / LDA (SRC),Y
        0x0A 0x0A 0x0A 0x05 0xF2 0x85 0xF9        ; ASL A / ASL A / ASL A / ORA T / STA B3
        0xC8 0x84 0xF8 0xA0 0x00 0xA5 0xF4        ; INY / STY YS / LDY #0 / LDA B1
        0x91 0xFC 0xC8 0xA5 0xF9 0x91 0xFC        ; STA (DST),Y / INY / LDA B3 / STA (DST),Y
        0x8D 0x05 0xC0 0xA5 0xF5 0x91 0xFC        ; STA $C005 / LDA B2 / STA (DST),Y
        0x88 0xA5 0xF3 0x91 0xFC                  ; DEY / LDA B0 / STA (DST),Y
        0x8D 0x04 0xC0 0xE6 0xFC 0xE6 0xFC        ; STA $C004 / INC DST / INC DST
        0xA4 0xF8 0xC6 0xF7 0xD0 0x8A 0x60        ; LDY YS / DEC W / BNE group / RTS
        0x00 0x10 0x20 0x30 0x40 0x50 0x60 0x70   ; LO4, first half
        0x00 0x10 0x20 0x30 0x40 0x50 0x60 0x70   ; LO4, second half
        0x00 0x20 0x40 0x60 0x00 0x20 0x40 0x60   ; LO5, first half
        0x00 0x20 0x40 0x60 0x00 0x20 0x40 0x60   ; LO5, second half
        0x00 0x40 0x00 0x40 0x00 0x40 0x00 0x40   ; LO6, first half
        0x00 0x40 0x00 0x40 0x00 0x40 0x00 0x40)) ; LO6, second half)

; where the code reads its tables: the operand's offset in code, the table's
(const PATCHES (array u8 6) (make (array u8 6) 14 124 39 140 64 156))

(let ready bool false)

(fn row ((page u8) (col u8) (top u8) (groups u8) (pixels (ptr u8))) void
    (let home u16 (as u16 (addr_of (at code 0))))
    (if (not ready)
        (do
            (for k u8 0 3
                (let at u8 (at PATCHES (* k 2)))
                (let table u16 (+ home (as u16 (at PATCHES (+ (* k 2) 1)))))
                (set (at code at) (as u8 table))
                (set (at code (+ at 1)) (as u8 (>> table 8))))
            (set ready true)))
    (dhgr.setup)
    (set (at auxmem.work 4) col)
    (set (at auxmem.work 5) groups)
    (set (at auxmem.work 6) page)
    (set auxmem.rest top)
    (set auxmem.from pixels)
    (jsr home))
