; backend/fold.cpp folded (& (fp) 0) to 0 and dropped the call: HasEffects did not know a call
; through a pointer (constant folding runs before LowerIndirectCalls); also (* (fp) 0) and
; (| (fp) 255)
(let g u8 0)
(let h u8 0)
(fn bump () u8 (set h (+ h 1)) h)
(fn main () u8
    (let fp (fn () u8) bump)
    (set g (& (fp) 0))
    (set g (& (fp) 0))
    (set g (* (fp) 0))
    (set g (| 255 (fp)))
    (set g (& (+ (fp) 1) 0))
    h)
