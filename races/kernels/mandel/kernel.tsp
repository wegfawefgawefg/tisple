; Mandelbrot, 32 x 24 pixels, up to 15 iterations, 16-bit fixed point with 6 fraction
; bits. A point escapes when |x| or |y| reaches 2 or x^2 + y^2 reaches 4, tested before
; the squares can overflow 16 bits.

(let iters (array u8 768) :address 0x4000)
(let x0 i16 :address 0x5F00)
(let y0 i16 :address 0x5F02)
(let step i16 :address 0x5F04)

(fn main () void
    (let cy i16 y0)
    (let k u16 0)
    (for py u8 0 24
        (let cx i16 x0)
        (for px u8 0 32
            (let zx i16 0)
            (let zy i16 0)
            (let n u8 0)
            (while (< n 15)
                (if (or (or (>= zx 128) (<= zx -128)) (or (>= zy 128) (<= zy -128)))
                    (break))
                (let zx2 i16 (>> (* zx zx) 6))
                (let zy2 i16 (>> (* zy zy) 6))
                (if (>= (+ zx2 zy2) 256)
                    (break))
                (set zy (+ (>> (* zx zy) 5) cy))
                (set zx (+ (- zx2 zy2) cx))
                (set n (+ n 1)))
            (set (at iters k) n)
            (set k (+ k 1))
            (set cx (+ cx step)))
        (set cy (+ cy step))))
