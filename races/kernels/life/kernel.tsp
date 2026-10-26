; one step of Life on 40 x 48 cells with a dead border, as programs/life: rows of 42
; cells, cur to next

(const STRIDE u16 42)
(const DEAD u8 0)
(const LIVE u8 1)

(let cur (array u8 2100) :address 0x2000)
(let next (array u8 2100) :address 0x4000)

; the first cell of row y; row 0 is the top border
(fn row_of ((world (ptr u8)) (y u8)) (ptr u8)
    (+ world (* (as u16 y) STRIDE)))

(fn main () void
    (let world (ptr u8) (addr_of (at cur 0)))
    (let out_world (ptr u8) (addr_of (at next 0)))
    (for y u8 1 49
        (let above (ptr u8) (row_of world (- y 1)))
        (let here (ptr u8) (row_of world y))
        (let below (ptr u8) (row_of world (+ y 1)))
        (let out (ptr u8) (row_of out_world y))
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
