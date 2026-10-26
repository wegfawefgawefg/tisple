; plasma: three moving sine waves summed for every block, through a color ramp
; Esc quits

(import apple2 a2)
(import keys)
(import lores)
(import macros)
(import tables)

; 128 + 127 sin(2 pi i / 256), rounded
(macro sine_byte (i)
    (round (+ 128 (* 127 (sin (/ (* (* 2 pi) i) 256))))))

(tables.table SINE u8 256 sine_byte)

; a loop through the lo-res colors, indexed by the whole sum (its top 4 bits pick the
; color), as a low nibble and as a high one: 512 bytes to save 8 shifts per screen byte
(macro ramp_color (sum)
    (nth (list 0 2 6 7 14 12 13 15 15 13 9 11 1 3 2 0) (>> sum 4)))

(macro ramp_color_high (sum)
    (<< (ramp_color sum) 4))

(tables.table RAMP_LO u8 256 ramp_color)
(tables.table RAMP_HI u8 256 ramp_color_high)

; this frame's part of the sum for each column and each block row
(let col (array u8 40))
(let row (array u8 48))
(let t u8 0)
(let frame u16 0)

(fn draw () void
    (for x u8 0 40
        (set (at col x) (+ (at SINE (+ (<< x 2) t)) (at SINE (- (* x 3) (<< t 1))))))
    (for y u8 0 48
        (set (at row y) (+ (at SINE (+ (* y 5) (<< t 1))) (at SINE (+ y t)))))
    ; two block rows per screen byte: the upper one in the low nibble
    (for r u8 0 24
        (let screen (ptr u8) (lores.row r))
        (let top u8 (at row (<< r 1)))
        (let bottom u8 (at row (+ (<< r 1) 1)))
        ; 40 straight copies: (at col 5) is a fixed address and there is no loop counter.
        ; 1 KB more code for 10% more frames than the loop (docs/benchmarks.md)
        (macros.unroll x 0 40
            (store (+ screen x)
                (| (at RAMP_LO (+ (at col x) top)) (at RAMP_HI (+ (at col x) bottom)))))))

(fn main () void
    (lores.show)
    (while (!= (keys.read) a2.KEY_ESC)
        (draw)
        (set t (+ t 1))
        (set frame (+ frame 1))))
