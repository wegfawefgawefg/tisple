; the macros that turn text into the book: each chapter is one let of 80-byte lines of
; screen codes, made while compiling. Strings exist only here.
;   (book.chapter name lines (options) "row" ...)  rows of text; "# x" is an inverse heading
;   (book.numbers name lines (options))            n, its square and cube, hex, binary, factors
;   (book.sines name lines (options))              a sine wave, a row a step
;   (book.primes name lines (options))             primes, ten a row
; lines is the chapter's length, which the viewer needs too; a wrong one is an error that
; gives the right one. options are the let's placement options, () or (:in aux).

(const WIDTH u16 80)

; ---- character codes in macro code: lists of ASCII codes

(macro digits (n)
    (if (< n 10)
        (return (list (+ 48 n))))
    (append (digits (/ n 10)) (list (+ 48 (% n 10)))))

(macro pad_left (codes width)
    (if (>= (len codes) width)
        (return codes))
    (append (repeat (- width (len codes)) 32) codes))

(macro pad_right (codes width)
    (if (>= (len codes) width)
        (return codes))
    (append codes (repeat (- width (len codes)) 32)))

(macro hex_digits (n count)
    (let out (list))
    (for i 0 count
        (let d (% (>> n (* 4 (- (- count 1) i))) 16))
        (set out
            (append out
                (list (if (< d 10)
                    (+ 48 d)
                    (+ 55 d))))))
    out)

(macro bin_digits (n count)
    (let out (list))
    (for i 0 count
        (set out (append out (list (+ 48 (% (>> n (- (- count 1) i)) 2))))))
    out)

; ---- rows to screen codes

; normal text has bit 7 set; inverse letters are $00-$1F, inverse lower case (the alternate
; set) $60-$7F, other inverse symbols $20-$3F
(macro screen_code (c inverse)
    (if (not inverse)
        (return (+ c 128)))
    (if (and (>= c 64) (< c 96))
        (- c 64)
        c))

; a row of text, a heading, or generated codes, to 80 screen codes. A generated code of 256
; or more is a screen code plus 256, as it is: MouseText ($40-$5F) on the alternate set
(macro row_codes (row)
    (let codes row)
    (let inverse false)
    (if (= (kind row) 'string)
        (do
            (set codes (chars row))
            (if (and (>= (len codes) 2) (and (= (nth codes 0) 35) (= (nth codes 1) 32)))
                (do
                    (set codes (tail (tail codes)))
                    (set inverse true)))))
    (if (> (len codes) 80)
        (error "a row is " (len codes) " characters, more than 80: " row))
    (let n (len codes))
    (map
        (fn (i)
            (let
                c
                (if (< i n)
                    (nth codes i)
                    32))
            (if (>= c 256)
                (- c 256)
                (screen_code c inverse)))
        (range 0 80)))

; lists joined pairwise, so a long book takes n log n steps, not n squared
(macro flatten (lists)
    (let n (len lists))
    (if (= n 0)
        (return (list)))
    (if (= n 1)
        (return (head lists)))
    (let half (/ n 2))
    (append
        (flatten
            (map
                (fn (i)
                    (nth lists i))
                (range 0 half)))
        (flatten
            (map
                (fn (i)
                    (nth lists i))
                (range half n)))))

(macro build (name lines options rows)
    (if (!= (len rows) lines)
        (error "chapter " name " has " (len rows) " rows; write " (len rows) " for its lines"))
    (let size (* 80 lines))
    `(let ,name (array u8 ,size)
        (make (array u8 ,size) ,@(flatten (map row_codes rows)))
        ,@options))

(macro chapter (name lines options & rows)
    (build name lines options rows))

; ---- generated chapters

(macro factors (n)
    (let out (list))
    (let rest n)
    (let p 2)
    (while (<= (* p p) rest)
        (let power 0)
        (while (= (% rest p) 0)
            (set rest (/ rest p))
            (set power (+ power 1)))
        (if (> power 0)
            (do
                (if (> (len out) 0)
                    (set out (append out (chars " x "))))
                (set out (append out (digits p)))
                (if (> power 1)
                    (set out (append out (list 94) (digits power))))))
        (set p (+ p 1)))
    (if (> rest 1)
        (do
            (if (> (len out) 0)
                (set out (append out (chars " x "))))
            (set out (append out (digits rest)))))
    out)

(macro number_row (n)
    (let fact (if (prime n)
        (chars "prime")
        (factors n)))
    (if (= n 1)
        (set fact (chars "one: no factors")))
    (append (pad_left (digits n) 6)
        (pad_left (digits (* n n)) 9)
        (pad_left (digits (* n (* n n))) 11)
        (chars "   $")
        (hex_digits n 4)
        (chars "   %")
        (bin_digits n 12)
        (chars "   ")
        fact))

(macro numbers (name lines options)
    (let rows
        (list "# Numbers" "" "     n   squared        cubed     hex          binary   factors" ""))
    (set rows (append rows (map number_row (range 1 (+ (- lines (len rows)) 1)))))
    (build name lines options rows))

; a step of 6 degrees a row: the angle, sin as a number, and a star on a 61-column axis
(macro sine_row (i)
    (let degrees (* i 6))
    (let s (sin (/ (* degrees pi) 180)))
    (let thousandths (round (* s 1000)))
    (let sign (if (< thousandths 0)
        45
        43))
    (let size (if (< thousandths 0)
        (- 0 thousandths)
        thousandths))
    (let fraction (digits (% size 1000)))
    (let shown
        (append (list sign (+ 48 (/ size 1000)) 46) (repeat (- 3 (len fraction)) 48) fraction))
    (let star (+ 49 (round (* s 30))))
    (let plot (list))
    (for col 19 80
        (set plot
            (append plot
                (list (if (= col star)
                    42
                    (if (= col 49)
                        124
                        (if (= (% (- col 19) 10) 0)
                            46
                            32)))))))
    (append (pad_left (digits degrees) 6) (chars " deg  ") shown (repeat 1 32) plot))

(macro sines (name lines options)
    (let rows (list "# A sine wave, 6 degrees a line" ""))
    (set rows (append rows (map sine_row (range 0 (- lines (len rows))))))
    (build name lines options rows))

(macro prime (n)
    (if (< n 2)
        (return false))
    (let d 2)
    (while (<= (* d d) n)
        (if (= (% n d) 0)
            (return false))
        (set d (+ d 1)))
    true)

(macro primes (name lines options)
    (let rows (list "# Primes, ten a line" ""))
    (let count (* 10 (- lines (len rows))))
    (let found (list))
    (let n 2)
    (while (< (len found) count)
        (if (prime n)
            (set found (append found (list n))))
        (set n (+ n 1)))
    (for r 0 (- lines 2)
        (let row (list))
        (for k 0 10
            (set row (append row (pad_left (digits (nth found (+ (* r 10) k))) 8))))
        (set rows (append rows (list row))))
    (build name lines options rows))

; 32 screen codes from first, each with a space after, behind a label 14 wide
(macro code_row (label first)
    (let row (pad_right (chars label) 14))
    (for i 0 32
        (set row (append row (list (+ 256 (+ first i)) 32))))
    row)

(macro charset (name lines options)
    (let rows
        (list "# The character set"
            ""
            "Our own font, drawn for Tisple ][, shown here at half width; the alternate set"
            "is on, so $40-$5F are MouseText and $60-$7F inverse lower case."
            ""
            (code_row "normal" 0xA0)
            (code_row "" 0xC0)
            (code_row "" 0xE0)
            ""
            (code_row "inverse" 0x00)
            (code_row "" 0x20)
            (code_row "" 0x60)
            ""
            (code_row "MouseText" 0x40)
            ""
            (append (chars "a window:     ") (list (+ 256 0x5E)) (repeat 34 (+ 256 0x4C)))
            (append (chars "              ")
                (list (+ 256 0x5F))
                (chars " closed ")
                (list (+ 256 0x40))
                (chars ", open ")
                (list (+ 256 0x41))
                (chars ", check ")
                (list (+ 256 0x44))
                (chars ", folder ")
                (list (+ 256 0x58) (+ 256 0x59)))
            (append (chars "              ") (list (+ 256 0x54)) (repeat 34 95))))
    (build name lines options rows))
