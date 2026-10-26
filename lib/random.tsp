; 16-bit xorshift: (import random) then (random.byte), (random.below n)
; the same seed gives the same sequence; set random.seed to change it

(let seed u16 0xACE1)

(fn byte () u8
    (set seed (^ seed (<< seed 7)))
    (set seed (^ seed (>> seed 9)))
    (set seed (^ seed (<< seed 8)))
    (as u8 seed))

; 0 to n - 1
(fn below ((n u8)) u8
    (% (byte) n))
