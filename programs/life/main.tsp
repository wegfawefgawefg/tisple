; Conway's Life on the lo-res screen, 40 x 48 cells
; any key starts a new random world; Esc quits

(import apple2 a2)
(import keys)
(import lores)
(import random)

(const STRIDE u16 42) ; 40 cells and a dead border cell on each side
(const DEAD u8 0)
(const LIVE u8 1)

; two worlds with a dead border all round, (48 + 2) rows of STRIDE cells
(let world_a (array u8 2100))
(let world_b (array u8 2100))
(let cur (ptr u8))
(let next (ptr u8))

(let generation u16 0)

; block colors for a cell in the top and the bottom half of a screen byte
(const TOP_COLOR (array u8 2) (make (array u8 2) 0 lores.GREEN))
(const BOTTOM_COLOR (array u8 2) (make (array u8 2) 0 0xC0))

; the first cell of row y; row 0 is the top border
(fn row_of ((world (ptr u8)) (y u8)) (ptr u8)
    (+ world (* (as u16 y) STRIDE)))

; about a third of the cells alive
(fn scatter () void
    (for y u8 1 49
        (let r (ptr u8) (row_of cur y))
        (for x u8 1 41
            (store (+ r x) (if (< (random.byte) 80)
                LIVE
                DEAD)))))

(fn step () void
    (for y u8 1 49
        (let above (ptr u8) (row_of cur (- y 1)))
        (let here (ptr u8) (row_of cur y))
        (let below (ptr u8) (row_of cur (+ y 1)))
        (let out (ptr u8) (row_of next y))
        (for x u8 1 41
            (let n u8
                (+
                    (+ (+ (load (+ above (- x 1))) (load (+ above x)))
                        (+ (load (+ above (+ x 1))) (load (+ here (- x 1)))))
                    (+ (+ (load (+ here (+ x 1))) (load (+ below (- x 1))))
                        (+ (load (+ below x)) (load (+ below (+ x 1)))))))
            (let alive u8 (load (+ here x)))
            (store (+ out x) (if (or (= n 3) (and (= n 2) (= alive LIVE)))
                LIVE
                DEAD)))))

; two cell rows per screen byte: the upper one in the low nibble
(fn draw () void
    (for r u8 0 24
        (let screen (ptr u8) (lores.row r))
        (let top (ptr u8) (row_of cur (+ (<< r 1) 1)))
        (let bottom (ptr u8) (row_of cur (+ (<< r 1) 2)))
        (for x u8 0 40
            (store (+ screen x)
                (| (at TOP_COLOR (load (+ top (+ x 1))))
                    (at BOTTOM_COLOR (load (+ bottom (+ x 1)))))))))

(fn swap () void
    (let t (ptr u8) cur)
    (set cur next)
    (set next t))

(fn main () void
    (set cur (addr_of (at world_a 0)))
    (set next (addr_of (at world_b 0)))
    (lores.clear)
    (lores.show)
    (scatter)
    (while true
        (draw)
        (step)
        (swap)
        (set generation (+ generation 1))
        (let key u8 (keys.read))
        (if (= key a2.KEY_ESC)
            (return))
        (if (!= key 0)
            (scatter))))
