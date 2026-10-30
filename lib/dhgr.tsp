; double hi-res: 560 dots a line, the even byte columns from aux memory, 140 x 192 pixels in 15
; colours (the two greys are one). (import dhgr) then
;   (dhgr.on true)                  double hi-res, 4 lines of 80-column text under it (false:
;                                   all 192 lines); (dhgr.off) back to 40-column text
;   (dhgr.show dhgr.PAGE2)          the page shown; (dhgr.flip) shows the other one;
;                                   dhgr.shown, and (dhgr.hidden) the one not shown
;   (dhgr.clear page colour)        a page in one colour, dhgr.BLACK to dhgr.WHITE
;   (dhgr.box page col top w h colour)      a box of byte columns col to col + w - 1 (0-39),
;                                   rows top to top + h - 1, both memories
;   (dhgr.plot page x y colour)     one pixel, x 0-139
;   (dhgr.fill page bank col top w h even odd)  one memory's bytes of a box (bank dhgr.MAIN or
;                                   dhgr.AUX), even in even columns, odd in odd ones
;   (dhgr.copy page bank col top w h pic)       w bytes a row from pic onto the box
;   (dhgr.masked page bank col top w h pic mask)  each byte (screen & mask) | pic
;   (dhgr.copy_box page bank col top w h from)  the same box of the page from ($20 steps, a
;                                   saved backdrop: $6000, $8000 ...) onto page
;   (dhgr.runs page bank col top h stream flags)  a sprite half stored as runs (masks only
;                                   where it has edges), from its memory or the aux language
;                                   card (flags dhgr.FROM_CARD; dhgr.AS_WHITE: in white)
;   (dhgr.aux_rmw addr keep set)    an aux memory byte becomes (byte & keep) | set; gives what
;                                   it was
;   (dhgr.text_row page row codes)  80 screen codes onto text row 0-23 of the text page the
;                                   mixed window shows with that page (page 2 shows text page 2)
; A pixel is 4 dots, its colour the dots as a lo-res colour number; 7 pixels take 4 bytes: aux,
; main, aux, main. Sprites go on byte columns, a memory at a time: for bank dhgr.AUX pic, mask
; and from are in aux memory (lets :in aux), for dhgr.MAIN in main memory. Nothing is clipped.
; Both pages in both memories ($2000-$5FFF) and text page 2 ($0800-$0BFF, both) are kept for
; the screens. Page flipping needs 80STORE off, so this library never turns it on: aux memory
; is written with RAMWRT and read with RAMRD, by our 6502 code at $1E00 in both memories (with
; RAMRD on the 6502 fetches its instructions from aux memory too). With an interrupt handler,
; call (auxmem.guard) first.

(import apple2 a2)
(import auxmem)
(import auxcard)
(import tables)

(let page1_main (array u8 8192) :address 0x2000 :reserve)
(let page1_aux (array u8 8192) :address 0x2000 :in aux :reserve)
(let page2_main (array u8 8192) :address 0x4000 :reserve)
(let page2_aux (array u8 8192) :address 0x4000 :in aux :reserve)
(let text2_main (array u8 1024) :address 0x0800 :reserve)
(let text2_aux (array u8 1024) :address 0x0800 :in aux :reserve)

(const PAGE1 u8 0x20) ; pages by their high byte
(const PAGE2 u8 0x40)
(const MAIN u8 0)
(const AUX u8 1)

; colours, as lo-res numbers them
(const BLACK u8 0)
(const MAGENTA u8 1)
(const DARK_BLUE u8 2)
(const PURPLE u8 3)
(const DARK_GREEN u8 4)
(const GREY u8 5)
(const MEDIUM_BLUE u8 6)
(const LIGHT_BLUE u8 7)
(const BROWN u8 8)
(const ORANGE u8 9)
(const GREY2 u8 10)
(const PINK u8 11)
(const GREEN u8 12)
(const YELLOW u8 13)
(const AQUA u8 14)
(const WHITE u8 15)

; row y's address on page 1: eighths of the page, 8-row bands, thirds; page 2 is $2000 on
(fn row_start ((y u8)) u16
    (+ (+ 0x2000 (<< (as u16 (& y 7)) 10))
        (+ (<< (as u16 (& (>> y 3) 7)) 7) (* (as u16 (>> y 6)) 40))))

(tables.table ROW u16 192 row_start :boundary 256)

; a colour's 4 bytes (aux, main, aux, main) for 7 pixels: dot d of the 28 lit when bit d & 3
; of the colour is
(macro colour_byte (i)
    (let colour (>> i 2))
    (let first (* (& i 3) 7))
    (let b 0)
    (for k 0 7
        (if (!= (& (>> colour (& (+ first k) 3)) 1) 0)
            (set b (| b (<< 1 k)))))
    b)

(tables.table COLOUR_BYTES u8 64 colour_byte)

; pixel x: its 7-pixel group (2 byte columns) and place in it; a place's dots fall in one byte
; or two (A, then B): the byte's memory and column in the group, its bits, and the colour's
; bits there for each colour (p * 16 + colour)
(macro x_group (x)
    (/ x 7))

(macro x_place (x)
    (% x 7))

(tables.table X_GROUP u8 140 x_group)
(tables.table X_PLACE u8 140 x_place)

; dot 4p + k of the group is in byte (4p + k) / 7: 0 aux, 1 main, 2 aux, 3 main; bit (4p + k) % 7
(macro place_byte (p part)
    (let first (/ (* 4 p) 7))
    (let last (/ (+ (* 4 p) 3) 7))
    (if (= part 0)
        first
        (if (= last first)
            -1
            last)))

(macro part_mask (p part)
    (let byte (place_byte p part))
    (let m 0)
    (if (>= byte 0)
        (for k 0 4
            (let dot (+ (* 4 p) k))
            (if (= (/ dot 7) byte)
                (set m (| m (<< 1 (% dot 7)))))))
    m)

(macro part_bits (i part)
    (let p (>> i 4))
    (let colour (& i 15))
    (let byte (place_byte p part))
    (let m 0)
    (if (>= byte 0)
        (for k 0 4
            (let dot (+ (* 4 p) k))
            (if (and (= (/ dot 7) byte) (!= (& (>> colour k) 1) 0))
                (set m (| m (<< 1 (% dot 7)))))))
    m)

(macro a_bank (p)
    (if (= (& (place_byte p 0) 1) 0)
        1
        0))

(macro a_col (p)
    (>> (place_byte p 0) 1))

(macro b_bank (p)
    (if (= (& (place_byte p 1) 1) 0)
        1
        0))

(macro b_col (p)
    (if (< (place_byte p 1) 0)
        0
        (>> (place_byte p 1) 1)))

(macro a_mask (p)
    (part_mask p 0))

(macro b_mask (p)
    (part_mask p 1))

(macro a_bits (i)
    (part_bits i 0))

(macro b_bits (i)
    (part_bits i 1))

(tables.table A_BANK u8 7 a_bank)
(tables.table A_COL u8 7 a_col)
(tables.table A_MASK u8 7 a_mask)
(tables.table A_BITS u8 112 a_bits)
(tables.table B_BANK u8 7 b_bank)
(tables.table B_COL u8 7 b_col)
(tables.table B_MASK u8 7 b_mask)
(tables.table B_BITS u8 112 b_bits)

; our 6502 code at $1E00 of both memories. Box routines from the jumps at the start, X = 0 for
; main memory or 1 for aux (RAMRD and RAMWRT both), with auxmem.rest the top row (ROWY),
; auxmem.pages the rows (H), auxmem.work: 0 EVEN, 1 ODD (fill's two bytes; copy_box's source
; page), 2-3 MASK, 4 COL, 5 W, 6 PAGEHI; auxmem.from the bytes (SRC), auxmem.to (DST) the
; screen byte, which row_at works out a row at a time. rmw: X the memory, A keep, Y set, DST
; the byte; text_row: SRC 80 codes, DST the row, writing aux with RAMWRT alone. runs (at
; $1F47, after text_row): SRC the runs, work 0 the flags (bit 7 FROM_CARD, bit 0 AS_WHITE), 1 the row's
; first DST, 2-3 a run's masks, 5 a run's length; between rows of runs in the aux language card
; ALTZP goes off and interrupts are let in for a moment (auxcard.call runs it with them off).
(const HOME u16 0x1E00)
(const MASKED u16 0x1E00)
(const COPY u16 0x1E03)
(const COPY_BOX u16 0x1E06)
(const FILL u16 0x1E09)
(const RMW u16 0x1E0C)
(const TEXT_ROW u16 0x1E0F)
(const RUNS u16 0x1F47)
(const KIT_SIZE u16 497)
(let home_main (array u8 KIT_SIZE) :address 0x1E00 :reserve)
(let home_aux (array u8 KIT_SIZE) :address 0x1E00 :in aux :reserve)

(const KIT (array u8 497)
    (make (array u8 497)
        0x4C 0x6B 0x1E 0x4C 0x9D 0x1E      ; JMP masked / JMP copy
        0x4C 0xB4 0x1E 0x4C 0xD4 0x1E      ; JMP copy_box / JMP fill
        0x4C 0x06 0x1F 0x4C 0x23 0x1F      ; JMP rmw / JMP text_row
        0xA5 0xFE 0x29 0x07 0x0A 0x0A      ; row_at: LDA ROWY / AND #$07 / ASL A / ASL A
        0x05 0xF8 0x85 0xFD 0xA5 0xFE      ; ORA PAGEHI / STA DST+1 / LDA ROWY
        0x29 0x38 0x4A 0x4A 0x4A 0x4A      ; AND #$38 / LSR A / LSR A / LSR A / LSR A
        0x05 0xFD 0x85 0xFD 0xA9 0x00 0x6A ; ORA DST+1 / STA DST+1 / LDA #0 / ROR A
        0x85 0xFC 0xA5 0xFE 0x0A 0x2A 0x2A ; STA DST / LDA ROWY / ASL A / ROL A / ROL A
        0x29 0x03 0xA8 0xA5 0xFC 0x18      ; AND #$03 / TAY / LDA DST / CLC
        0x79 0x40 0x1E 0x65 0xF6 0x85 0xFC ; ADC THIRDS,Y / ADC COL / STA DST
        0x60                               ; RTS
        0x00 0x28 0x50                     ; THIRDS: .BYTE 0, 40, 80
        0xE6 0xFE 0xA5 0xFE 0x29 0x07      ; next_row: INC ROWY / LDA ROWY / AND #$07
        0xF0 0x0A 0xA5 0xFD 0x18 0x69 0x04 ; BEQ new_band / LDA DST+1 / CLC / ADC #4
        0x85 0xFD 0xC6 0xF9 0x60           ; STA DST+1 / DEC H / RTS
        0x20 0x12 0x1E 0xC6 0xF9 0x60      ; new_band: JSR row_at / DEC H / RTS
        0x9D 0x02 0xC0 0x9D 0x04 0xC0      ; start: STA $C002,X / STA $C004,X
        0x4C 0x12 0x1E                     ; JMP row_at
        0x8D 0x02 0xC0 0x8D 0x04 0xC0 0x60 ; done: STA $C002 / STA $C004 / RTS
        0x20 0x5B 0x1E                     ; masked: JSR start
        0xA4 0xF7 0x88                     ; m_row: LDY W / DEY
        0xB1 0xFC 0x31 0xF4 0x11 0xFA      ; m_byte: LDA (DST),Y / AND (MASK),Y / ORA (SRC),Y
        0x91 0xFC 0x88 0x10 0xF5 0xA5 0xF7 ; STA (DST),Y / DEY / BPL m_byte / LDA W
        0x18 0x65 0xF4 0x85 0xF4 0x90 0x02 ; CLC / ADC MASK / STA MASK / BCC m_next
        0xE6 0xF5                          ; INC MASK+1
        0x20 0x91 0x1E 0x20 0x43 0x1E      ; m_next: JSR step_src / JSR next_row
        0xD0 0xDF 0xF0 0xD3                ; BNE m_row / BEQ done
        0xA5 0xF7 0x18 0x65 0xFA 0x85 0xFA ; step_src: LDA W / CLC / ADC SRC / STA SRC
        0x90 0x02 0xE6 0xFB                ; BCC s_done / INC SRC+1
        0x60                               ; s_done: RTS
        0x20 0x5B 0x1E                     ; copy: JSR start
        0xA4 0xF7 0x88                     ; c_row: LDY W / DEY
        0xB1 0xFA 0x91 0xFC 0x88 0x10 0xF9 ; c_byte: LDA (SRC),Y / STA (DST),Y / DEY / BPL c_byte
        0x20 0x91 0x1E 0x20 0x43 0x1E      ; JSR step_src / JSR next_row
        0xD0 0xEE 0xF0 0xB0                ; BNE c_row / BEQ done
        0x20 0x5B 0x1E                     ; copy_box: JSR start
        0xA5 0xFC 0x85 0xFA 0xA5 0xFD      ; b_row: LDA DST / STA SRC / LDA DST+1
        0x45 0xF8 0x05 0xF3 0x85 0xFB      ; EOR PAGEHI / ORA ODD / STA SRC+1
        0xA4 0xF7 0x88                     ; LDY W / DEY
        0xB1 0xFA 0x91 0xFC 0x88 0x10 0xF9 ; b_byte: LDA (SRC),Y / STA (DST),Y / DEY / BPL b_byte
        0x20 0x43 0x1E 0xD0 0xE5 0xF0 0x90 ; JSR next_row / BNE b_row / BEQ done
        0xA5 0xF6 0x4A 0x90 0x08 0xA5 0xF2 ; fill: LDA COL / LSR A / BCC f_go / LDA EVEN
        0xA4 0xF3 0x85 0xF3 0x84 0xF2      ; LDY ODD / STA ODD / STY EVEN
        0x20 0x5B 0x1E                     ; f_go: JSR start
        0xA4 0xF7 0x88 0x98 0x4A 0xB0 0x07 ; f_row: LDY W / DEY / TYA / LSR A / BCS f_pair
        0xA5 0xF2 0x91 0xFC 0x88 0x30 0x0C ; LDA EVEN / STA (DST),Y / DEY / BMI f_end
        0xA5 0xF3 0x91 0xFC 0x88 0xA5 0xF2 ; f_pair: LDA ODD / STA (DST),Y / DEY / LDA EVEN
        0x91 0xFC 0x88 0x10 0xF4           ; STA (DST),Y / DEY / BPL f_pair
        0x20 0x43 0x1E 0xD0 0xE1           ; f_end: JSR next_row / BNE f_row
        0x4C 0x64 0x1E                     ; JMP done
        0x85 0xF2 0x84 0xF3 0x9D 0x02 0xC0 ; rmw: STA EVEN / STY ODD / STA $C002,X
        0x9D 0x04 0xC0 0xA0 0x00 0xB1 0xFC ; STA $C004,X / LDY #0 / LDA (DST),Y
        0xAA 0x25 0xF2 0x05 0xF3 0x91 0xFC ; TAX / AND EVEN / ORA ODD / STA (DST),Y
        0x8D 0x02 0xC0 0x8D 0x04 0xC0 0x8A ; STA $C002 / STA $C004 / TXA
        0x60                               ; RTS
        0xA9 0x27 0x85 0xF6                ; text_row: LDA #39 / STA COL
        0xA5 0xF6 0x0A 0xA8 0xB1 0xFA      ; t_col: LDA COL / ASL A / TAY / LDA (SRC),Y
        0x8D 0x05 0xC0 0xA4 0xF6 0x91 0xFC ; STA $C005 / LDY COL / STA (DST),Y
        0x8D 0x04 0xC0 0xA5 0xF6 0x0A 0xA8 ; STA $C004 / LDA COL / ASL A / TAY
        0xC8 0xB1 0xFA 0xA4 0xF6 0x91 0xFC ; INY / LDA (SRC),Y / LDY COL / STA (DST),Y
        0xC6 0xF6 0x10 0xE1 0x60           ; DEC COL / BPL t_col / RTS
        0x20 0x5B 0x1E                     ; runs: JSR start
        0xA5 0xFC 0x85 0xF3 0xA0 0x00      ; row: LDA DST / STA ROWLO / LDY #0
        0xB1 0xFA 0xF0 0x7B 0xE6 0xFA      ; seg: LDA (SRC),Y / BEQ row_end / INC SRC
        0xD0 0x02 0xE6 0xFB                ; BNE c1 / INC SRC+1
        0xC9 0x40 0xB0 0x07 0x65 0xFC      ; c1: CMP #$40 / BCS run / ADC DST
        0x85 0xFC 0x4C 0x50 0x1F           ; STA DST / JMP seg
        0xAA 0x29 0x3F 0x85 0xF7 0xA8 0x88 ; run: TAX / AND #$3F / STA N / TAY / DEY
        0xE0 0x80 0xB0 0x1D 0xA5 0xF2 0x4A ; CPX #$80 / BCS masked / LDA FLAGS / LSR A
        0xB0 0x0C                          ; BCS o_white
        0xB1 0xFA 0x91 0xFC 0x88 0x10 0xF9 ; o_byte: LDA (SRC),Y / STA (DST),Y / DEY / BPL o_byte
        0xA5 0xF7 0x4C 0xBA 0x1F           ; LDA N / JMP advance
        0xA9 0x7F                          ; o_white: LDA #$7F
        0x91 0xFC 0x88 0x10 0xFB 0xA5 0xF7 ; o_wbyte: STA (DST),Y / DEY / BPL o_wbyte / LDA N
        0x4C 0xBA 0x1F                     ; JMP advance
        0xA5 0xFA 0x18 0x65 0xF7 0x85 0xF4 ; masked: LDA SRC / CLC / ADC N / STA MSK
        0xA5 0xFB 0x69 0x00 0x85 0xF5      ; LDA SRC+1 / ADC #0 / STA MSK+1
        0xA5 0xF2 0x4A 0xB0 0x0D           ; LDA FLAGS / LSR A / BCS m_white
        0xB1 0xFC 0x31 0xF4 0x11 0xFA      ; m_byte: LDA (DST),Y / AND (MSK),Y / ORA (SRC),Y
        0x91 0xFC 0x88 0x10 0xF5 0x30 0x0B ; STA (DST),Y / DEY / BPL m_byte / BMI m_done
        0xB1 0xF4 0x49 0x7F 0x11 0xFC      ; m_white: LDA (MSK),Y / EOR #$7F / ORA (DST),Y
        0x91 0xFC 0x88 0x10 0xF5           ; STA (DST),Y / DEY / BPL m_white
        0xA5 0xF7 0x0A                     ; m_done: LDA N / ASL A
        0x18 0x65 0xFA 0x85 0xFA 0x90 0x02 ; advance: CLC / ADC SRC / STA SRC / BCC a1
        0xE6 0xFB                          ; INC SRC+1
        0xA5 0xFC 0x18 0x65 0xF7 0x85 0xFC ; a1: LDA DST / CLC / ADC N / STA DST
        0xA0 0x00 0x4C 0x50 0x1F           ; LDY #0 / JMP seg
        0xE6 0xFA 0xD0 0x02 0xE6 0xFB      ; row_end: INC SRC / BNE r1 / INC SRC+1
        0xA5 0xF3 0x85 0xFC 0x24 0xF2      ; r1: LDA ROWLO / STA DST / BIT FLAGS
        0x10 0x09 0x8D 0x08 0xC0 0x58 0xEA ; BPL r2 / STA $C008 / CLI / NOP
        0x78 0x8D 0x09 0xC0                ; SEI / STA $C009
        0x20 0x43 0x1E 0xF0 0x03           ; r2: JSR next_row / BEQ fin
        0x4C 0x4A 0x1F                     ; JMP row
        0x4C 0x64 0x1E))                   ; fin: JMP done

(let ready bool false)
(let shown u8 PAGE1)

; the code to $1E00 of both memories, once
(fn setup () void
    (if ready
        (return))
    (let home (ptr u8) (as (ptr u8) HOME))
    (for i u16 0 KIT_SIZE
        (store (+ home i) (at KIT i)))
    (auxmem.write home (addr_of (at KIT 0)) KIT_SIZE)
    (set ready true))

(fn on ((mixed bool)) void
    (setup)
    (io_write u8 a2.CLR80STORE 0)
    (io_write u8 a2.SET80VID 0)
    (io_read u8 a2.TXTCLR)
    (if mixed
        (io_read u8 a2.MIXSET)
        (io_read u8 a2.MIXCLR))
    (io_read u8 a2.HIRES)
    (io_read u8 a2.AN3OFF)
    (show shown))

(fn off () void
    (io_read u8 a2.AN3ON)
    (io_write u8 a2.CLR80VID 0)
    (io_read u8 a2.TXTSET)
    (io_read u8 a2.MIXCLR)
    (io_read u8 a2.LORES)
    (io_read u8 a2.LOWSCR))

(fn show ((page u8)) void
    (if (= page PAGE2)
        (io_read u8 a2.HISCR)
        (io_read u8 a2.LOWSCR))
    (set shown page))

(fn hidden () u8
    (if (= shown PAGE1)
        PAGE2
        PAGE1))

(fn flip () void
    (show (hidden)))

; the box's place for the code, and its routine run on memory bank
(fn box_at ((page u8) (col u8) (top u8) (w u8) (h u8)) void
    (setup)
    (set (at auxmem.work 4) col)
    (set (at auxmem.work 5) w)
    (set (at auxmem.work 6) page)
    (set auxmem.rest top)
    (set auxmem.pages h))

(fn fill ((page u8) (bank u8) (col u8) (top u8) (w u8) (h u8) (even u8) (odd u8)) void
    (box_at page col top w h)
    (set (at auxmem.work 0) even)
    (set (at auxmem.work 1) odd)
    (jsr FILL (x bank)))

(fn box ((page u8) (col u8) (top u8) (w u8) (h u8) (colour u8)) void
    (let at_colour u8 (<< colour 2))
    (fill page AUX col top w h (at COLOUR_BYTES at_colour) (at COLOUR_BYTES (+ at_colour 2)))
    (fill page MAIN col top w h
        (at COLOUR_BYTES (+ at_colour 1))
        (at COLOUR_BYTES (+ at_colour 3))))

(fn clear ((page u8) (colour u8)) void
    (box page 0 0 40 192 colour))

(fn copy ((page u8) (bank u8) (col u8) (top u8) (w u8) (h u8) (pic (ptr u8))) void
    (box_at page col top w h)
    (set auxmem.from pic)
    (jsr COPY (x bank)))

(fn masked ((page u8) (bank u8) (col u8) (top u8) (w u8) (h u8) (pic (ptr u8)) (mask (ptr u8))) void
    (box_at page col top w h)
    (set auxmem.from pic)
    (let m u16 (as u16 mask))
    (set (at auxmem.work 2) (as u8 m))
    (set (at auxmem.work 3) (as u8 (>> m 8)))
    (jsr MASKED (x bank)))

(fn copy_box ((page u8) (bank u8) (col u8) (top u8) (w u8) (h u8) (from u8)) void
    (box_at page col top w h)
    (set (at auxmem.work 1) from)
    (jsr COPY_BOX (x bank)))

; runs flags: the runs are in the aux language card (lib/auxcard reaches them), the shape
; drawn in white
(const FROM_CARD u8 0x80)
(const AS_WHITE u8 0x01)

; a sprite half stored as runs onto the page's bank, its top-left at byte column col (even, as
; for masked), h rows: per row, control bytes up to a 0: 1-63 skip that many bytes, $40 + n n
; picture bytes copied (opaque), $80 + n n picture bytes then their n masks, each screen byte
; (screen & mask) | picture; n is 1-63. The runs are in the bank's memory, or with FROM_CARD
; in the aux language card for either bank. AS_WHITE draws the shape in white instead.
(fn runs ((page u8) (bank u8) (col u8) (top u8) (h u8) (stream (ptr u8)) (flags u8)) void
    (box_at page col top 1 h)
    (set (at auxmem.work 0) flags)
    (set auxmem.from stream)
    (if (!= (& flags FROM_CARD) 0)
        (auxcard.call RUNS bank)
        (jsr RUNS (x bank))))

(fn aux_rmw ((addr u16) (keep u8) (set_bits u8)) u8
    (setup)
    (set auxmem.to (as (ptr u8) addr))
    (jsr RMW (a keep) (x AUX) (y set_bits)))

; one byte of a pixel: aux through the code, main straight
(fn plot_byte ((addr u16) (bank u8) (mask u8) (bits u8)) void
    (if (= bank AUX)
        (aux_rmw addr (~ mask) bits)
        (do
            (let p (ptr u8) (as (ptr u8) addr))
            (store p (| (& (load p) (~ mask)) bits)))))

(fn plot ((page u8) (x u8) (y u8) (colour u8)) void
    (let place u8 (at X_PLACE x))
    (let base u16 (+ (+ (at ROW y) (<< (as u16 (- page PAGE1)) 8)) (as u16 (<< (at X_GROUP x) 1))))
    (let i u8 (+ (<< place 4) colour))
    (plot_byte (+ base (as u16 (at A_COL place))) (at A_BANK place) (at A_MASK place) (at A_BITS i))
    (if (!= (at B_MASK place) 0)
        (plot_byte (+ base (as u16 (at B_COL place)))
            (at B_BANK place)
            (at B_MASK place)
            (at B_BITS i))))

; text rows: page 1's window shows text page 1, page 2's text page 2
(macro text_start (r)
    (+ (* (% r 8) 0x80) (* (/ r 8) 40)))

(tables.table TEXT_ROW_AT u16 24 text_start)

(fn text_row ((page u8) (row u8) (codes (ptr u8))) void
    (setup)
    (let base u16 (if (= page PAGE2)
        0x0800
        0x0400))
    (set auxmem.from codes)
    (set auxmem.to (as (ptr u8) (+ base (at TEXT_ROW_AT row))))
    (jsr TEXT_ROW))
