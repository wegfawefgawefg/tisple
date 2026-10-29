; 80-column text on an extended 80-column card: 80 x 24 characters, the even columns in aux
; memory. (import text80) then
;   (text80.on)                   80 columns, the alternate set (MouseText), cleared
;   (text80.print "Hello")        text at the cursor; a string is a macro argument only
;   (text80.put c)                one ASCII character; RETURN starts the next line
;   (text80.put_code c)           one screen code as it is, MouseText ($40-$5F) included
;   (text80.move col row)         the cursor; text80.col and text80.row read it
;   (text80.inverse true)         what follows in inverse, until (text80.inverse false)
;   (text80.clear)                spaces everywhere, the cursor home
;   (text80.put_row r p)          80 screen codes from p across row r, a line at once
;   (text80.off)                  back to 40 columns
; Past the last row the screen scrolls up a line. The screen is reached with io_read and
; io_write, so nothing moves across the PAGE2 switch that picks aux or main memory.

(import apple2 a2)
(import tables)

(const COLUMNS u8 80)
(const ROWS u8 24)
(const SPACE u8 0xA0)

; start of each of the 24 rows in each memory: rows are interleaved in thirds of 40 bytes
(macro row_start (r)
    (+ (+ 0x0400 (* (% r 8) 0x80)) (* (/ r 8) 40)))

(tables.table ROW u16 24 row_start :boundary 256)

(let col u8 0)
(let row u8 0)
(let inverse_on bool false)

(fn on () void
    (io_write u8 a2.SET80VID 0)
    (io_write u8 a2.SETALTCHAR 0)
    (io_write u8 a2.SET80STORE 0)
    (io_read u8 a2.TXTSET)
    (io_read u8 a2.LOWSCR)
    (clear))

(fn off () void
    (io_write u8 a2.CLR80VID 0)
    (io_write u8 a2.CLRALTCHAR 0)
    (io_write u8 a2.CLR80STORE 0)
    (io_read u8 a2.LOWSCR))

; 40 bytes of one memory's half of every row
(fn fill_half ((code u8)) void
    (for r u8 0 ROWS
        (let base u16 (at ROW r))
        (for i u16 0 40
            (io_write u8 (+ base i) code))))

(fn clear () void
    (io_read u8 a2.HISCR)
    (fill_half SPACE)
    (io_read u8 a2.LOWSCR)
    (fill_half SPACE)
    (set col 0)
    (set row 0))

(fn move ((c u8) (r u8)) void
    (set col c)
    (set row r))

(fn inverse ((on_now bool)) void
    (set inverse_on on_now))

; the byte at column c of row r: even columns in aux memory (PAGE2 on), odd ones in main
(fn poke ((c u8) (r u8) (code u8)) void
    (let at_addr u16 (+ (at ROW r) (as u16 (>> c 1))))
    (if (= (& c 1) 0)
        (do
            (io_read u8 a2.HISCR)
            (io_write u8 at_addr code)
            (io_read u8 a2.LOWSCR))
        (io_write u8 at_addr code)))

; every row up one, a blank row at the bottom
(fn scroll () void
    (for half u8 0 2
        (if (= half 0)
            (io_read u8 a2.HISCR)
            (io_read u8 a2.LOWSCR))
        (for r u8 1 ROWS
            (let from u16 (at ROW r))
            (let to u16 (at ROW (- r 1)))
            (for i u16 0 40
                (io_write u8 (+ to i) (io_read u8 (+ from i)))))
        (let last u16 (at ROW (- ROWS 1)))
        (for i u16 0 40
            (io_write u8 (+ last i) SPACE)))
    (io_read u8 a2.LOWSCR))

(fn newline () void
    (set col 0)
    (if (< row (- ROWS 1))
        (set row (+ row 1))
        (scroll)))

; a screen code at the cursor, then on
(fn put_code ((code u8)) void
    (poke col row code)
    (set col (+ col 1))
    (if (= col COLUMNS)
        (newline)))

; an ASCII character at the cursor, normal or inverse: inverse letters are $00-$1F, inverse
; lower case (the alternate set) $60-$7F. All in one body, so a print is a call a character
(fn put ((c u8)) void
    (if (= c a2.KEY_RETURN)
        (do
            (newline)
            (return)))
    (let ascii u8 (& c 0x7F))
    (let code u8 (| ascii 0x80))
    (if (and inverse_on (and (>= ascii 0x40) (< ascii 0x60)))
        (set code (- ascii 0x40))
        (if inverse_on
            (set code ascii)))
    (let at_addr u16 (+ (at ROW row) (as u16 (>> col 1))))
    (if (= (& col 1) 0)
        (do
            (io_read u8 a2.HISCR)
            (io_write u8 at_addr code)
            (io_read u8 a2.LOWSCR))
        (io_write u8 at_addr code))
    (set col (+ col 1))
    (if (= col COLUMNS)
        (newline)))

; the text's characters one after another; strings exist only while compiling
(macro print (text)
    `(do
        ,@(map
            (fn (c)
                `(put ,c))
            (chars text))))

; 80 screen codes from p across row r: the 40 even ones into aux memory, then the odd ones
(fn put_row ((r u8) (p (ptr u8))) void
    (let base u16 (at ROW r))
    (io_read u8 a2.HISCR)
    (for i u8 0 40
        (io_write u8 (+ base (as u16 i)) (load (+ p (<< i 1)))))
    (io_read u8 a2.LOWSCR)
    (for i u8 0 40
        (io_write u8 (+ base (as u16 i)) (load (+ p (+ (<< i 1) 1))))))
