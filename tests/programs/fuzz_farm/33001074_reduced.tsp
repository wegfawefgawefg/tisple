; slice 6 (pointers.cpp): f1's store through p19 became a set of wb in the middle of a round,
; and collapse, going by the summary from the round's start (no write of wb), ran the loop
; around its call once
(let g0 u8 1)
(let wb (array u8 1024))
(fn peek2 ((p (ptr u8)) (k u8)) u8 (load (+ p (& k 3))))
(fn f1 () u16 (let p19 (ptr u8) (addr_of (at wb 0))) (store (+ p19 (% g0 16)) (+ g0 (peek2 p19 g0))) 0)
(fn main () i16 (for n40 u8 0 3 (f1)) 0)
