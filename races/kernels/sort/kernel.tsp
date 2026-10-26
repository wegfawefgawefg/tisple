; insertion sort of 256 bytes in place

(let arr (array u8 256) :address 0x4000)

(fn main () void
    (for i u8 1 256
        (let v u8 (at arr i))
        (let j u8 i)
        (while (and (!= j 0) (> (at arr (- j 1)) v))
            (set (at arr j) (at arr (- j 1)))
            (set j (- j 1)))
        (set (at arr j) v)))
