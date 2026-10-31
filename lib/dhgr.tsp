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
;                                   mixed window shows with that page (page 2 shows text page 2:
;                                   (import dhgr_text2) keeps it out of the program's layout)
; A pixel is 4 dots, its colour the dots as a lo-res colour number; 7 pixels take 4 bytes: aux,
; main, aux, main. Sprites go on byte columns, a memory at a time: for bank dhgr.AUX pic, mask
; and from are in aux memory (lets :in aux), for dhgr.MAIN in main memory. Nothing is clipped.
; Both pages in both memories ($2000-$5FFF) are kept for the screens; text page 2 ($0800-$0BFF,
; the mixed window's text with page 2) only with (import dhgr_text2), so a program that does not
; show it there has that 1 KB of each memory. Page flipping needs 80STORE off, so this library
; never turns it on: aux memory is written with RAMWRT and read with RAMRD, in with_aux (the
; box code, plot and text_row are Tisple; runs is our 6502 code at $1E00 in both memories, for
; its runs in the aux language card). With an interrupt handler, call (auxmem.guard) first.

(import apple2 a2)
(import auxmem)
(import auxcard)
(import tables)

(let page1_main (array u8 8192) :address 0x2000 :reserve)
(let page1_aux (array u8 8192) :address 0x2000 :in aux :reserve)
(let page2_main (array u8 8192) :address 0x4000 :reserve)
(let page2_aux (array u8 8192) :address 0x4000 :in aux :reserve)

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

(let ready bool false)
(let shown u8 PAGE1)

(fn on ((mixed bool)) void
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

; the boxes: with_aux code (docs/language.md, Aux memory code). For dhgr.AUX the screen, pic,
; mask and from are aux memory's, reached with RAMRD and RAMWRT on; for dhgr.MAIN both stay
; off. A row's first byte is row_at's: ROW's address for it, which the link copies to aux
; memory for this code, plus the page and the column, worked out once a box (start). A row's
; bytes go right to left: the 6502 counts down to 0 for nothing (DEY / BPL), so the last
; index is an i8 (w is 1-40).

; where byte column col of the page whose high byte is page is, from ROW's addresses
(fn start ((page u8) (col u8)) u16
    (+ (<< (as u16 (- page PAGE1)) 8) (as u16 col)))

; the first byte of row y of a box whose start that is (a row down is the next y: the table
; holds what the old code worked out a band at a time)
(fn row_at ((y u8) (from u16)) (ptr u8)
    (as (ptr u8) (+ (at ROW y) from)))

(fn fill ((page u8) (bank u8) (col u8) (top u8) (w u8) (h u8) (even u8) (odd u8)) void
    (let at u16 (start page col))
    ; byte i of a row is column col + i: a the even columns' byte, b the odd ones'
    (let a u8 even)
    (let b u8 odd)
    (if (!= (& col 1) 0)
        (do
            (set a odd)
            (set b even)))
    ; an odd w ends a row with an even byte on its own, the pairs (odd, even) below it
    (let last u8 (- w 1))
    (let single bool (= (& w 1) 1))
    (let pairs i8
        (as i8 (if single
            (- w 2)
            last)))
    (with_aux false (= bank AUX)
        (for y u8 top (+ top h)
            (let dst (ptr u8) (row_at y at))
            (if single
                (store (+ dst last) a))
            (let i i8 pairs)
            (while (>= i 0)
                (store (+ dst (as u8 i)) b)
                (set i (- i 1))
                (store (+ dst (as u8 i)) a)
                (set i (- i 1))))))

(fn box ((page u8) (col u8) (top u8) (w u8) (h u8) (colour u8)) void
    (let at_colour u8 (<< colour 2))
    (fill page AUX col top w h (at COLOUR_BYTES at_colour) (at COLOUR_BYTES (+ at_colour 2)))
    (fill page MAIN col top w h
        (at COLOUR_BYTES (+ at_colour 1))
        (at COLOUR_BYTES (+ at_colour 3))))

(fn clear ((page u8) (colour u8)) void
    (box page 0 0 40 192 colour))

(fn copy ((page u8) (bank u8) (col u8) (top u8) (w u8) (h u8) (pic (ptr u8))) void
    (let at u16 (start page col))
    (let last i8 (as i8 (- w 1)))
    (let aux bool (= bank AUX))
    (with_aux aux aux
        (for y u8 top (+ top h)
            (let dst (ptr u8) (row_at y at))
            (let i i8 last)
            (while (>= i 0)
                (store (+ dst (as u8 i)) (load (+ pic (as u8 i))))
                (set i (- i 1)))
            (set pic (+ pic w)))))

(fn masked ((page u8) (bank u8) (col u8) (top u8) (w u8) (h u8) (pic (ptr u8)) (mask (ptr u8))) void
    (let at u16 (start page col))
    (let last i8 (as i8 (- w 1)))
    (let aux bool (= bank AUX))
    (with_aux aux aux
        (for y u8 top (+ top h)
            (let dst (ptr u8) (row_at y at))
            (let i i8 last)
            (while (>= i 0)
                (store (+ dst (as u8 i))
                    (| (& (load (+ dst (as u8 i))) (load (+ mask (as u8 i))))
                        (load (+ pic (as u8 i)))))
                (set i (- i 1)))
            (set pic (+ pic w))
            (set mask (+ mask w)))))

(fn copy_box ((page u8) (bank u8) (col u8) (top u8) (w u8) (h u8) (from u8)) void
    (let at u16 (start page col))
    (let pages u8 (- from page)) ; from the box to its source: whole pages
    (let last i8 (as i8 (- w 1)))
    (let aux bool (= bank AUX))
    (with_aux aux aux
        (for y u8 top (+ top h)
            (let dst (ptr u8) (row_at y at))
            (let src (ptr u8) (as (ptr u8) (+ (as u16 dst) (<< (as u16 pages) 8))))
            (let i i8 last)
            (while (>= i 0)
                (store (+ dst (as u8 i)) (load (+ src (as u8 i))))
                (set i (- i 1))))))

(fn aux_rmw ((addr u16) (keep u8) (set_bits u8)) u8
    (let p (ptr u8) (as (ptr u8) addr))
    (with_aux true true
        (let was u8 (load p))
        (store p (| (& was keep) set_bits))
        was))

; one byte of a pixel: aux through with_aux, main straight
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

; codes 2c to aux memory (RAMWRT on), 2c + 1 to main memory, at column c: codes + c moves
; on a byte a column, so both are read at c and c + 1 from it
(fn text_row ((page u8) (row u8) (codes (ptr u8))) void
    (let base u16 (if (= page PAGE2)
        0x0800
        0x0400))
    (let dst (ptr u8) (as (ptr u8) (+ base (at TEXT_ROW_AT row))))
    (for c u8 0 40
        (with_aux false true
            (store (+ dst c) (load (+ codes c))))
        (store (+ dst c) (load (+ codes (+ c 1))))
        (set codes (+ codes 1))))

; runs: our 6502 code at $1E00 of both memories, with the old box code's row_at, next_row,
; start and done, which it calls. The runs in the aux language card are reached with ALTZP on
; (lib/auxcard), which compiled code cannot run under (its zero page is switched), so this
; stays machine code; it reads the runs from main or aux memory too. In: X = 0 for main memory
; or 1 for aux (RAMRD and RAMWRT both), auxmem.rest the top row (ROWY), auxmem.pages the rows
; (H), auxmem.work 4 COL, 6 PAGEHI, 0 the flags (bit 7 FROM_CARD, bit 0 AS_WHITE), 1 the row's
; first DST, 2-3 a run's masks, 5 a run's length; auxmem.from the runs (SRC), auxmem.to (DST)
; the screen byte. Between rows of runs in the aux language card ALTZP goes off and interrupts
; are let in for a moment (auxcard.call runs it with them off), with bank 2 in meanwhile for
; runs in bank 1 (flags bit 6), where a program's entry may be.
(const HOME u16 0x1E00)
(const RUNS u16 0x1E59)
(const KIT_SIZE u16 307)
(let home_main (array u8 KIT_SIZE) :address 0x1E00 :reserve)
(let home_aux (array u8 KIT_SIZE) :address 0x1E00 :in aux :reserve)

(const KIT (array u8 307)
    (make (array u8 307)
        0xA5 0xFE 0x29 0x07 0x0A 0x0A      ; row_at: LDA ROWY / AND #$07 / ASL A / ASL A
        0x05 0xF8 0x85 0xFD 0xA5 0xFE      ; ORA PAGEHI / STA DST+1 / LDA ROWY
        0x29 0x38 0x4A 0x4A 0x4A 0x4A      ; AND #$38 / LSR A / LSR A / LSR A / LSR A
        0x05 0xFD 0x85 0xFD 0xA9 0x00 0x6A ; ORA DST+1 / STA DST+1 / LDA #0 / ROR A
        0x85 0xFC 0xA5 0xFE 0x0A 0x2A 0x2A ; STA DST / LDA ROWY / ASL A / ROL A / ROL A
        0x29 0x03 0xA8 0xA5 0xFC 0x18      ; AND #$03 / TAY / LDA DST / CLC
        0x79 0x2E 0x1E 0x65 0xF6 0x85 0xFC ; ADC THIRDS,Y / ADC COL / STA DST
        0x60                               ; RTS
        0x00 0x28 0x50                     ; THIRDS: .BYTE 0, 40, 80
        0xE6 0xFE 0xA5 0xFE 0x29 0x07      ; next_row: INC ROWY / LDA ROWY / AND #$07
        0xF0 0x0A 0xA5 0xFD 0x18 0x69 0x04 ; BEQ new_band / LDA DST+1 / CLC / ADC #4
        0x85 0xFD 0xC6 0xF9 0x60           ; STA DST+1 / DEC H / RTS
        0x20 0x00 0x1E 0xC6 0xF9 0x60      ; new_band: JSR row_at / DEC H / RTS
        0x9D 0x02 0xC0 0x9D 0x04 0xC0      ; start: STA $C002,X / STA $C004,X
        0x4C 0x00 0x1E                     ; JMP row_at
        0x8D 0x02 0xC0 0x8D 0x04 0xC0 0x60 ; done: STA $C002 / STA $C004 / RTS
        0x20 0x49 0x1E                     ; runs: JSR start
        0xA5 0xFC 0x85 0xF3 0xA0 0x00      ; row: LDA DST / STA ROWLO / LDY #0
        0xB1 0xFA 0xF0 0x77 0xE6 0xFA      ; seg: LDA (SRC),Y / BEQ row_end / INC SRC
        0xD0 0x02 0xE6 0xFB                ; BNE c1 / INC SRC+1
        0xC9 0x40 0xB0 0x06 0x65 0xFC      ; c1: CMP #$40 / BCS run / ADC DST
        0x85 0xFC 0x90 0xEC                ; STA DST / BCC seg
        0xAA 0x29 0x3F 0x85 0xF7 0xA8 0x88 ; run: TAX / AND #$3F / STA N / TAY / DEY
        0xE0 0x80 0xB0 0x1A 0xA5 0xF2 0x4A ; CPX #$80 / BCS masked / LDA FLAGS / LSR A
        0xB0 0x0C                          ; BCS o_white
        0xB1 0xFA 0x91 0xFC 0x88 0x10 0xF9 ; o_byte: LDA (SRC),Y / STA (DST),Y / DEY / BPL o_byte
        0xA5 0xF7 0x4C 0xC8 0x1E           ; o_done: LDA N / JMP advance
        0xA9 0x7F                          ; o_white: LDA #$7F
        0x91 0xFC 0x88 0x10 0xFB 0x30 0xF2 ; o_wbyte: STA (DST),Y / DEY / BPL o_wbyte / BMI o_done
        0x4C 0x11 0x1F 0x65 0xF7 0x85 0xF4 ; masked: JMP hi / m1: ADC N / STA MSK
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
        0xA0 0x00 0x4C 0x62 0x1E           ; LDY #0 / JMP seg
        0xE6 0xFA 0xD0 0x02 0xE6 0xFB      ; row_end: INC SRC / BNE r1 / INC SRC+1
        0xA5 0xF3 0x85 0xFC 0x24 0xF2      ; r1: LDA ROWLO / STA DST / BIT FLAGS
        0x10 0x1B 0x50 0x06 0x2C 0x83 0xC0 ; BPL r2 / BVC w1 / BIT $C083
        0x2C 0x83 0xC0                     ; BIT $C083
        0x8D 0x08 0xC0 0x58 0xEA 0x78      ; w1: STA $C008 / CLI / NOP / SEI
        0x8D 0x09 0xC0 0x24 0xF2 0x50 0x06 ; STA $C009 / BIT FLAGS / BVC r2
        0x2C 0x8B 0xC0 0x2C 0x8B 0xC0      ; BIT $C08B / BIT $C08B
        0x20 0x31 0x1E 0xF0 0x03           ; r2: JSR next_row / BEQ fin
        0x4C 0x5C 0x1E                     ; JMP row
        0x4C 0x52 0x1E                     ; fin: JMP done
        0xE0 0xC0 0xB0 0x06                ; hi: CPX #$C0 / BCS fill
        0xA5 0xFA 0x18 0x4C 0x9E 0x1E      ; LDA SRC / CLC / JMP m1
        0xA5 0xF2 0x4A 0xA0 0x00 0xB1 0xFA ; fill: LDA FLAGS / LSR A / LDY #0 / LDA (SRC),Y
        0x90 0x02 0xA9 0x7F                ; BCC f_go / LDA #$7F
        0xA4 0xF7 0x88                     ; f_go: LDY N / DEY
        0x91 0xFC 0x88 0x10 0xFB           ; f_byte: STA (DST),Y / DEY / BPL f_byte
        0xA9 0x01 0x4C 0xC8 0x1E))         ; LDA #1 / JMP advance

; the code to $1E00 of both memories, once
(fn setup () void
    (if ready
        (return))
    (let home (ptr u8) (as (ptr u8) HOME))
    (for i u16 0 KIT_SIZE
        (store (+ home i) (at KIT i)))
    (auxmem.write home (addr_of (at KIT 0)) KIT_SIZE)
    (set ready true))

; runs flags: the runs are in the aux language card (lib/auxcard reaches them), the shape
; drawn in white
(const FROM_CARD u8 0x80)  ; the card's bank 2 ($D000-$FFFF)
(const FROM_CARD1 u8 0xC0) ; its bank 1 ($D000-$DFFF)
(const AS_WHITE u8 0x01)

; a sprite half stored as runs onto the page's bank, its top-left at byte column col (even, as
; for masked), h rows: per row, control bytes up to a 0: 1-63 skip that many bytes, $40 + n n
; picture bytes copied (opaque), $80 + n n picture bytes then their n masks, each screen byte
; (screen & mask) | picture, $C0 + n one byte n times (opaque: a solid sprite's black); n is
; 1-63. The runs are in the bank's memory, or with FROM_CARD in the aux language card for
; either bank (FROM_CARD1: in the card's bank 1). AS_WHITE draws the shape in white instead (a
; fill's bytes too).
(fn runs ((page u8) (bank u8) (col u8) (top u8) (h u8) (stream (ptr u8)) (flags u8)) void
    (setup)
    (set (at auxmem.work 4) col)
    (set (at auxmem.work 5) 1)
    (set (at auxmem.work 6) page)
    (set auxmem.rest top)
    (set auxmem.pages h)
    (set (at auxmem.work 0) flags)
    (set auxmem.from stream)
    (if (!= (& flags FROM_CARD) 0)
        (auxcard.call RUNS bank (if (= (& flags 0x40) 0)
            auxcard.BANK2
            auxcard.BANK1))
        (jsr RUNS (x bank))))
