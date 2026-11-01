; 40-column text on text page 1. (import text40) then
;   (text40.on)                   text, page 1, cleared
;   (text40.print "Hello")        text at the cursor, a put call a character
;   (text40.put_str "Hello")      the same from a zero-terminated string, in a loop
;   (text40.put c)                one ASCII character; RETURN starts the next line
;   (text40.put_code c)           one screen code as it is
;   (text40.move col row)         the cursor; text40.col and text40.row read it
;   (text40.inverse true)         what follows in inverse, until (text40.inverse false)
;   (text40.clear)                spaces everywhere, the cursor home
;   (text40.poke col row code)    one screen code at a place; the cursor stays
;   (text40.window 20)            the text from row 20 down only: clear, the cursor's home and
;                                 scrolling keep to rows 20-23 (the mixed modes' four lines,
;                                 lib/lores and lib/hgr); (text40.window 0) the whole screen
; Past the last row the text scrolls up a line. Inverse lower case needs the alternate
; set, (io_write u8 a2.SETALTCHAR 0); the primary one flashes those codes.

(import apple2 a2)
(import tables)

(const COLUMNS u8 40)
(const ROWS u8 24)
(const SPACE u8 0xA0)

(macro row_start (r)
    (+ (+ 0x0400 (* (% r 8) 0x80)) (* (/ r 8) 40)))

(tables.table ROW u16 24 row_start :boundary 256)

(let col u8 0)
(let row u8 0)
(let inverse_on bool false)
(let top u8 0) ; the window's first row

(fn row_at ((r u8)) (ptr u8)
    (as (ptr u8) (at ROW r)))

(fn on () void
    (io_read u8 a2.TXTSET)
    (io_read u8 a2.LOWSCR)
    (clear))

(fn clear () void
    (for r u8 top ROWS
        (let p (ptr u8) (row_at r))
        (for i u8 0 COLUMNS
            (store (+ p i) SPACE)))
    (set col 0)
    (set row top))

(fn window ((first u8)) void
    (set top first)
    (if (< row top)
        (set row top)))

(fn poke ((c u8) (r u8) (code u8)) void
    (store (+ (row_at r) c) code))

(fn move ((c u8) (r u8)) void
    (set col c)
    (set row r))

(fn inverse ((on_now bool)) void
    (set inverse_on on_now))

(fn scroll () void
    (for r u8 (+ top 1) ROWS
        (let from (ptr u8) (row_at r))
        (let to (ptr u8) (row_at (- r 1)))
        (for i u8 0 COLUMNS
            (store (+ to i) (load (+ from i)))))
    (let last (ptr u8) (row_at (- ROWS 1)))
    (for i u8 0 COLUMNS
        (store (+ last i) SPACE)))

(fn newline () void
    (set col 0)
    (if (< row (- ROWS 1))
        (set row (+ row 1))
        (scroll)))

(fn put_code ((code u8)) void
    (store (+ (row_at row) col) code)
    (set col (+ col 1))
    (if (= col COLUMNS)
        (newline)))

; an ASCII character at the cursor, normal or inverse: inverse letters are $00-$1F, inverse
; symbols $20-$3F. All in one body, so a print is a call a character
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
    (store (+ (row_at row) col) code)
    (set col (+ col 1))
    (if (= col COLUMNS)
        (newline)))

(macro print (text)
    `(do
        ,@(map
            (fn (c)
                `(put ,c))
            (chars text))))

; the characters of a string literal, or any zero-terminated ASCII text up to 255 long
(fn put_str ((s (ptr u8))) void
    (let i u8 0)
    (while (!= (load (+ s i)) 0)
        (put (load (+ s i)))
        (set i (+ i 1))))
