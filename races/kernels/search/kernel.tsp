; naive search for the 6-byte needle in 4 KB of text; result is the first index

(const TEXT_LEN u16 4096)
(const NEEDLE_LEN u8 6)

(let text (array u8 4096) :address 0x2000)
(let needle (array u8 6) :address 0x5E00)
(let result u16 :address 0x5F00)

(fn main () void
    (for i u16 0 (+ (- TEXT_LEN (as u16 NEEDLE_LEN)) 1)
        (let p (ptr u8) (addr_of (at text i)))
        (let j u8 0)
        (while (and (< j NEEDLE_LEN) (= (load (+ p j)) (at needle j)))
            (set j (+ j 1)))
        (if (= j NEEDLE_LEN)
            (do
                (set result i)
                (return))))
    (set result 0xFFFF))
