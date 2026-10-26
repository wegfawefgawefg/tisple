; 16-bit multiply: the sum of x[i] * y[i] over 128 word pairs, wrapping at 16 bits

(let x (array u16 128) :address 0x4000)
(let y (array u16 128) :address 0x4100)
(let result u16 :address 0x5F00)

(fn main () void
    (let sum u16 0)
    (for i u8 0 128
        (set sum (+ sum (* (at x i) (at y i)))))
    (set result sum))
