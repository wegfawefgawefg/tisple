; length of a zero-terminated string (3000 bytes)

(let str (array u8 4096) :address 0x2000)
(let result u16 :address 0x5F00)

(fn main () void
    (let n u16 0)
    (while (!= (at str n) 0)
        (set n (+ n 1)))
    (set result n))
