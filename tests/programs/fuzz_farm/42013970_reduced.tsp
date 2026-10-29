(let g4 u16)
(let arr13 (array u8 256))
(let wb (array u8 1024))
(fn f0 ((a16 u16) (a17 u16) (a18 u8)) i8 0)
(fn f2 () u8 (let p25 (ptr u8) (addr_of (at wb 2))) (set (at arr13 3) (load (+ p25 254))) 0)
(fn main () u8
    (for n26 u8 3 5 (do (let c27 u8 0) (while (< c27 4) (set c27 (+ c27 1)) (f0 0 (io_read u16 49219) 0) (f2))))
    0)
