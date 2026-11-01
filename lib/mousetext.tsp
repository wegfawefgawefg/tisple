; MouseText: the 32 pictures the alternate character set has at screen codes $40-$5F on an
; enhanced IIe, a IIc and a IIgs (lib/machine says: machine.mousetext). An unenhanced IIe shows
; inverse capitals there. (import mousetext) then
;   (mousetext.on)                  the alternate set in 40 columns (lib/text80 turns it on)
;   (text40.put_code mousetext.CHECK) or text80.put_code, text40.poke, text80.poke: one picture
;   (mousetext.frame80 col row w h) a window frame round the w x h text at col, row, in 80
;                                   columns: underscores along its top, MouseText bars down its
;                                   sides and along its bottom, outside the text (so col >= 1,
;                                   row >= 1, col + w <= 79, row + h <= 23)
;   (mousetext.frame40 col row w h) the same in 40 columns
;   (mousetext.bar80 row) and bar40 a line of MouseText's top bar across a row: under a menu
; The names say what each picture is; $46 and $47 are the IIc's and enhanced IIe's running man
; (inverse, then normal), which a IIgs draws as other shapes.

(import apple2 a2)
(import text40)
(import text80)

(const CLOSED_APPLE u8 0x40)
(const OPEN_APPLE u8 0x41)
(const POINTER u8 0x42)
(const HOURGLASS u8 0x43)
(const CHECK u8 0x44)
(const CHECK_INVERSE u8 0x45)
(const RUNNER_INVERSE u8 0x46)
(const RUNNER u8 0x47)
(const LEFT_ARROW u8 0x48)
(const ELLIPSIS u8 0x49)
(const DOWN_ARROW u8 0x4A)
(const UP_ARROW u8 0x4B)
(const TOP_BAR u8 0x4C) ; a line along the cell's top
(const RETURN u8 0x4D)
(const BLOCK u8 0x4E)
(const SCROLL_LEFT u8 0x4F)
(const SCROLL_RIGHT u8 0x50)
(const SCROLL_DOWN u8 0x51)
(const SCROLL_UP u8 0x52)
(const MIDDLE_BAR u8 0x53) ; a line across the middle
(const CORNER u8 0x54)     ; lines along the left and the bottom
(const RIGHT_ARROW u8 0x55)
(const CHECKER_1 u8 0x56)
(const CHECKER_2 u8 0x57)
(const FOLDER_LEFT u8 0x58)
(const FOLDER_RIGHT u8 0x59)
(const RIGHT_BAR u8 0x5A) ; a line down the cell's right side
(const DIAMOND u8 0x5B)
(const TOP_BOTTOM u8 0x5C) ; lines along the top and the bottom
(const CROSS u8 0x5D)
(const SQUARE_DOT u8 0x5E)
(const LEFT_BAR u8 0x5F) ; a line down the cell's left side

(const UNDERSCORE u8 0xDF) ; '_' in the normal set: a line along the cell's bottom

(fn on () void
    (io_write u8 a2.SETALTCHAR 0))

(fn frame40 ((col u8) (row u8) (w u8) (h u8)) void
    (for i u8 0 w
        (text40.poke (+ col i) (- row 1) UNDERSCORE)
        (text40.poke (+ col i) (+ row h) TOP_BAR))
    (for i u8 0 h
        (text40.poke (- col 1) (+ row i) RIGHT_BAR)
        (text40.poke (+ col w) (+ row i) LEFT_BAR)))

(fn frame80 ((col u8) (row u8) (w u8) (h u8)) void
    (for i u8 0 w
        (text80.poke (+ col i) (- row 1) UNDERSCORE)
        (text80.poke (+ col i) (+ row h) TOP_BAR))
    (for i u8 0 h
        (text80.poke (- col 1) (+ row i) RIGHT_BAR)
        (text80.poke (+ col w) (+ row i) LEFT_BAR)))

(fn bar40 ((row u8)) void
    (for i u8 0 40
        (text40.poke i row TOP_BAR)))

(fn bar80 ((row u8)) void
    (for i u8 0 80
        (text80.poke i row TOP_BAR)))
