; double hi-res drawn from colour pixels, a row at a time: (import dhgr_pixels) then
;   (dhgr_pixels.row page col top groups pixels)  groups 7-pixel groups (1-20) of row top of
;                                   page from byte column col (even) on: each pixel a byte of
;                                   main memory at pixels, its colour 0-15; whole bytes written
;                                   in both memories, what was there gone
;   (dhgr_pixels.read page col top groups pixels)  the other way: the row's bytes back to
;                                   pixels at pixels, about 120 cycles a group and 300 a row
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

; read's decoding, our 6502 code too: W groups of bytes, aux ones at auxmem.from (aux c, aux
; c + 1, ...) and main ones at work 2-3, to pixels at auxmem.to; its loop goes back in two
; hops (BNE to mid, BNE to the start) so it has no address in it
(let decoder (array u8 150)
    (make (array u8 150)
        0xA9 0x00 0x85 0xFE                ; decode: LDA #0 / STA OUT
        0xA0 0x00 0xB1 0xFA 0x85 0xF3      ; dg: LDY #0 / LDA (AUXB),Y / STA B0
        0xB1 0xF4 0x85 0xF6 0xC8 0xB1 0xFA ; LDA (MAINB),Y / STA B1 / INY / LDA (AUXB),Y
        0x85 0xF8 0xB1 0xF4 0x85 0xF9      ; STA B2 / LDA (MAINB),Y / STA B3
        0xA4 0xFE 0xA5 0xF3 0x29 0x0F      ; LDY OUT / LDA B0 / AND #$0F
        0x91 0xFC 0xC8 0xA5 0xF6 0x29 0x01 ; STA (DST),Y / INY / LDA B1 / AND #$01
        0x0A 0x0A 0x0A 0x85 0xF2 0xA5 0xF3 ; ASL A / ASL A / ASL A / STA T / LDA B0
        0x4A 0x4A 0x4A 0x4A 0x05 0xF2      ; LSR A / LSR A / LSR A / LSR A / ORA T
        0x91 0xFC 0xC8 0xA5 0xF6 0x4A      ; STA (DST),Y / INY / LDA B1 / LSR A
        0x29 0x0F 0x91 0xFC 0xC8 0xA5 0xF8 ; AND #$0F / STA (DST),Y / INY / LDA B2
        0x29 0x03 0x0A 0x0A 0x85 0xF2      ; AND #$03 / ASL A / ASL A / STA T
        0xA5 0xF6 0x4A 0x4A 0x4A 0x4A 0x4A ; LDA B1 / LSR A / LSR A / LSR A / LSR A / LSR A
        0x05 0xF2 0x91 0xFC 0xC8 0xB8      ; ORA T / STA (DST),Y / INY / CLV
        0x50 0x02                          ; BVC over
        0xD0 0xAF                          ; mid: BNE dg
        0xA5 0xF8 0x4A 0x4A 0x29 0x0F      ; over: LDA B2 / LSR A / LSR A / AND #$0F
        0x91 0xFC 0xC8 0xA5 0xF9 0x29 0x07 ; STA (DST),Y / INY / LDA B3 / AND #$07
        0x0A 0x85 0xF2 0xA5 0xF8 0x0A 0x2A ; ASL A / STA T / LDA B2 / ASL A / ROL A
        0x2A 0x29 0x01 0x05 0xF2 0x91 0xFC ; ROL A / AND #$01 / ORA T / STA (DST),Y
        0xC8 0xA5 0xF9 0x4A 0x4A 0x4A      ; INY / LDA B3 / LSR A / LSR A / LSR A
        0x91 0xFC 0xC8 0x84 0xFE 0x18      ; STA (DST),Y / INY / STY OUT / CLC
        0xA5 0xFA 0x69 0x02 0x85 0xFA      ; LDA AUXB / ADC #2 / STA AUXB
        0x90 0x02 0xE6 0xFB                ; BCC d1 / INC AUXB+1
        0x18 0xA5 0xF4 0x69 0x02 0x85 0xF4 ; d1: CLC / LDA MAINB / ADC #2 / STA MAINB
        0x90 0x02 0xE6 0xF5                ; BCC d2 / INC MAINB+1
        0xC6 0xF7 0xD0 0xBE 0x60))         ; d2: DEC W / BNE mid / RTS)

(let aux_bytes (array u8 40))
(let main_bytes (array u8 40))

(fn read ((page u8) (col u8) (top u8) (groups u8) (pixels (ptr u8))) void
    (let count u8 (<< groups 1))
    (let at u16 (+ (+ (at dhgr.ROW top) (<< (as u16 (- page dhgr.PAGE1)) 8)) (as u16 col)))
    (let p (ptr u8) (as (ptr u8) at))
    (for i u8 0 count
        (set (at main_bytes i) (load (+ p i))))
    (auxmem.read (addr_of (at aux_bytes 0)) p (as u16 count))
    (set auxmem.from (addr_of (at aux_bytes 0)))
    (let m u16 (as u16 (addr_of (at main_bytes 0))))
    (set (at auxmem.work 2) (as u8 m))
    (set (at auxmem.work 3) (as u8 (>> m 8)))
    (set (at auxmem.work 5) groups)
    (set auxmem.to pixels)
    (jsr (as u16 (addr_of (at decoder 0)))))
