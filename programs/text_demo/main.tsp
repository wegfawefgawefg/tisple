; a scrolling viewer for a long built-in book, 80 columns wide: the bigger-program demo.
; About 72 KB of text across main memory above and below $6000, the language card and aux
; memory (chapters.tsp); `tisple run programs/text_demo/main.tsp`, or with --disk to boot it.
; Arrows scroll, RETURN goes to the next chapter, T to the top, ESC leaves.

(import apple2 a2)
(import keys)
(import text80)
(import auxmem)
(import chapters ch)

(const CHAPTERS u8 6)
(const WIDTH u16 80)
(const FIRST_ROW u8 1) ; row 0 is the title, row 23 the status line
(const PAGE u8 22)
(const STATUS_ROW u8 23)
(const INVERSE_SPACE u8 0x20)

; each chapter's first line, length, and whether it is in aux memory
(let chapter_start (array (ptr u8) 6))
(let chapter_lines (array u16 6))
(let chapter_aux (array bool 6))
(let total u16 0)
(let top u16 0)          ; the book's line at the top of the page
(let top_chapter u8 0)   ; and its chapter
(let line (array u8 80)) ; an aux line, copied to main memory to show
(let blank (array u8 80))

(fn add ((c u8) (start (ptr u8)) (lines u16) (aux bool)) void
    (set (at chapter_start c) start)
    (set (at chapter_lines c) lines)
    (set (at chapter_aux c) aux)
    (set total (+ total lines)))

(fn setup () void
    (add 0 (addr_of (at ch.intro 0)) ch.INTRO_LINES false)
    (add 1 (addr_of (at ch.built 0)) ch.BUILT_LINES false)
    (add 2 (addr_of (at ch.charset 0)) ch.CHARSET_LINES false)
    (add 3 (addr_of (at ch.sines 0)) ch.SINES_LINES false)
    (add 4 (addr_of (at ch.primes 0)) ch.PRIMES_LINES false)
    (add 5 (addr_of (at ch.numbers 0)) ch.NUMBERS_LINES true)
    (for i u8 0 80
        (set (at blank i) 0xA0)))

; the chapter that line n of the book is in, and the line within it
(let found_line u16 0)

(fn chapter_of ((n u16)) u8
    (let c u8 0)
    (let rest u16 n)
    (while (and (< c (- CHAPTERS 1)) (>= rest (at chapter_lines c)))
        (set rest (- rest (at chapter_lines c)))
        (set c (+ c 1)))
    (set found_line rest)
    c)

; the 80 screen codes of line n, in main memory
(fn line_codes ((n u16)) (ptr u8)
    (let c u8 (chapter_of n))
    (let p (ptr u8) (+ (at chapter_start c) (* found_line WIDTH)))
    (if (at chapter_aux c)
        (do
            (auxmem.read (addr_of (at line 0)) p 80)
            (return (addr_of (at line 0)))))
    p)

; the first line of chapter c in the book
(fn chapter_first ((c u8)) u16
    (let n u16 0)
    (for i u8 0 c
        (set n (+ n (at chapter_lines i))))
    n)

; n in decimal at the cursor
(let digit_buf (array u8 5))

(fn print_number ((n u16)) void
    (let count u8 0)
    (let rest u16 n)
    (while (do
            (set (at digit_buf count) (+ 48 (as u8 (% rest 10))))
            (set rest (/ rest 10))
            (set count (+ count 1))
            (> rest 0)))
    (while (> count 0)
        (set count (- count 1))
        (text80.put (at digit_buf count))))

(fn print_name ((c u8)) void
    (match c
        (0 (text80.print "Tisple ][ text viewer"))
        (1 (text80.print "How it is put together"))
        (2 (text80.print "The character set"))
        (3 (text80.print "A sine wave"))
        (4 (text80.print "Primes"))
        (_ (text80.print "Numbers"))))

; where chapter c's bytes are
(fn print_place ((c u8)) void
    (let address u16 (as u16 (at chapter_start c)))
    (if (at chapter_aux c)
        (text80.print "aux memory")
        (if (>= address 0xD000)
            (text80.print "the language card")
            (if (< address 0x6000)
                (text80.print "main memory below $6000")
                (text80.print "main memory from $6000")))))

; inverse spaces to just before column end, on the cursor's row
(fn fill_to ((end u8)) void
    (while (< text80.col end)
        (text80.put_code INVERSE_SPACE)))

(fn title () void
    (text80.move 0 0)
    (text80.inverse true)
    (text80.put_code 0x40) ; the closed apple
    (text80.print " ")
    (print_name top_chapter)
    (fill_to 56)
    (text80.print "line ")
    (print_number (+ top 1))
    (text80.print " of ")
    (print_number total)
    (fill_to 79)
    (text80.poke 79 0 INVERSE_SPACE)
    (text80.inverse false))

(fn status () void
    (text80.move 0 STATUS_ROW)
    (text80.inverse true)
    (text80.print " chapter ")
    (print_number (as u16 (+ top_chapter 1)))
    (text80.print " is in ")
    (print_place top_chapter)
    (fill_to 46)
    (text80.print "arrows, RETURN, T, ESC to leave")
    (fill_to 79)
    (text80.poke 79 STATUS_ROW INVERSE_SPACE)
    (text80.inverse false))

(fn draw () void
    (set top_chapter (chapter_of top))
    (title)
    (for r u8 0 PAGE
        (let n u16 (+ top (as u16 r)))
        (if (< n total)
            (text80.put_row (+ r FIRST_ROW) (line_codes n))
            (text80.put_row (+ r FIRST_ROW) (addr_of (at blank 0)))))
    (status))

(fn last_top () u16
    (- total (as u16 PAGE)))

(fn down ((lines u16)) void
    (set top (+ top lines))
    (if (> top (last_top))
        (set top (last_top))))

(fn up ((lines u16)) void
    (if (> top lines)
        (set top (- top lines))
        (set top 0)))

(fn next_chapter () void
    (let c u8 (+ (chapter_of top) 1))
    (if (= c CHAPTERS)
        (set c 0))
    (set top (chapter_first c))
    (if (> top (last_top))
        (set top (last_top))))

(fn main () void
    (setup)
    (text80.on)
    (draw)
    (while true
        (let k u8 (keys.wait))
        (if (= k a2.KEY_ESC)
            (break))
        (match k
            (a2.KEY_DOWN (down 1))
            (a2.KEY_UP (up 1))
            (a2.KEY_RIGHT (down (as u16 PAGE)))
            (a2.KEY_SPACE (down (as u16 PAGE)))
            (a2.KEY_LEFT (up (as u16 PAGE)))
            (a2.KEY_RETURN (next_chapter))
            (0x54 (set top 0)) ; T
            (0x74 (set top 0)) ; t
            (_))
        (draw))
    (text80.clear)
    (text80.off))
