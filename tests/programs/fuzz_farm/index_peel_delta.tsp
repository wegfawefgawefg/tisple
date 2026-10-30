; gen_place.cpp: (at a (+ K (as u16 (+ x c)))) lost K. The constant peeled off the u16 index was
; overwritten when the byte delta c moved into the address: buf+3 for buf+12, and t+65534 for
; t+298 with (- x 2)
(let buf (array u8 512))
(fn main () u16
    (for x u16 0 512 (set (at buf x) (as u8 (+ (* x 3) (>> x 4)))))
    (let s u16 0)
    (for x u8 1 40 (set s (+ s (as u16 (at buf (+ 9 (as u16 (+ x 3))))))))
    (for x u8 0 60 (if (> x 1) (set s (+ s (as u16 (at buf (+ 300 (as u16 (- x 2)))))))))
    (for y u8 2 50 (set s (+ s (* 3 (as u16 (at buf (- (+ 7 (as u16 (+ y 5))) 4)))))))
    s)
