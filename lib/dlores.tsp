; double lo-res: 80 x 48 blocks in 16 colours, the even columns from aux memory, on a 128K IIe
; (Rev B board, extended 80-column card), IIc or IIgs. (import dlores) then
;   (dlores.on false)          double lo-res, the whole screen; (dlores.on true) 80 x 40 over
;                              four lines of 80-column text, rows 20-23 (lib/text80 with
;                              (text80.window 20) writes them)
;   (dlores.plot x y colour)   one block, x 0-79, y 0-47, colour dlores.BLACK to WHITE (the
;                              lo-res colours)
;   (dlores.get x y)           its colour
;   (dlores.clear colour)      every block (in mixed mode not the text rows)
;   (dlores.off)               back to 40-column text
; The screen is text page 1's memory in both memories: block column x is in aux memory when x
; is even, main when odd, at row (y / 2)'s byte x / 2, the low nibble for even y. The hardware
; shows an aux nibble rotated right a bit, so plot stores aux colours rotated left. Aux memory
; is reached as lib/text80 reaches it: 80STORE on, PAGE2 picking aux memory for $0400-$07FF,
; through io_read and io_write so nothing moves across the switch. 80STORE stays on (as text80
; leaves it), so text page 2 can't be shown meanwhile.

(import apple2 a2)
(import tables)

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

(macro row_start (r)
    (+ (+ 0x0400 (* (% r 8) 0x80)) (* (/ r 8) 40)))

(tables.table ROW u16 24 row_start :boundary 256)

; a colour as an aux nibble must hold it: rotated left one bit, since it shows rotated right
(macro aux_colour (c)
    (& (| (<< c 1) (>> c 3)) 15))

(tables.table AUX_COLOUR u8 16 aux_colour)

; and back, for get
(macro main_colour (c)
    (& (| (>> c 1) (<< c 3)) 15))

(tables.table FROM_AUX u8 16 main_colour)

(let rows u8 24)

(fn on ((mixed bool)) void
    (io_write u8 a2.SET80VID 0)
    (io_write u8 a2.SET80STORE 0)
    (io_read u8 a2.TXTCLR)
    (io_read u8 a2.LOWSCR)
    (io_read u8 a2.LORES)
    (io_read u8 a2.AN3OFF) ; double res
    (if mixed
        (do
            (io_read u8 a2.MIXSET)
            (set rows 20))
        (do
            (io_read u8 a2.MIXCLR)
            (set rows 24))))

(fn off () void
    (io_read u8 a2.AN3ON)
    (io_read u8 a2.TXTSET)
    (io_read u8 a2.MIXCLR)
    (io_write u8 a2.CLR80VID 0)
    (io_write u8 a2.CLR80STORE 0)
    (io_read u8 a2.LOWSCR))

; the byte holding block x, y, and the memory: PAGE2 on for aux (even x)
(fn byte_at ((x u8) (y u8)) u16
    (+ (at ROW (>> y 1)) (as u16 (>> x 1))))

(fn plot ((x u8) (y u8) (colour u8)) void
    (let addr u16 (byte_at x y))
    (let c u8 colour)
    (let aux bool (= (& x 1) 0))
    (if aux
        (do
            (set c (at AUX_COLOUR colour))
            (io_read u8 a2.HISCR)))
    (let old u8 (io_read u8 addr))
    (if (= (& y 1) 0)
        (io_write u8 addr (| (& old 0xF0) c))
        (io_write u8 addr (| (& old 0x0F) (<< c 4))))
    (if aux
        (io_read u8 a2.LOWSCR)))

(fn get ((x u8) (y u8)) u8
    (let addr u16 (byte_at x y))
    (let aux bool (= (& x 1) 0))
    (if aux
        (io_read u8 a2.HISCR))
    (let b u8 (io_read u8 addr))
    (if aux
        (io_read u8 a2.LOWSCR))
    (let c u8 (if (= (& y 1) 0)
        (& b 0x0F)
        (>> b 4)))
    (if aux
        (at FROM_AUX c)
        c))

; 40 bytes of one memory in each graphics row
(fn fill_half ((value u8)) void
    (for r u8 0 rows
        (let base u16 (at ROW r))
        (for i u16 0 40
            (io_write u8 (+ base i) value))))

(fn clear ((colour u8)) void
    (let a u8 (at AUX_COLOUR colour))
    (io_read u8 a2.HISCR)
    (fill_half (| a (<< a 4)))
    (io_read u8 a2.LOWSCR)
    (fill_half (| colour (<< colour 4))))
