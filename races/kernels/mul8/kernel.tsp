; 8-bit multiply: the sum of a[i] * b[i] over 256 byte pairs, in 16 bits

(let a (array u8 256) :address 0x4000)
(let b (array u8 256) :address 0x4100)
(let result u16 :address 0x5F00)

(fn main () void
    (let sum u16 0)
    (for i u8 0 256
        (set sum (+ sum (* (as u16 (at a i)) (as u16 (at b i))))))
    (set result sum))
