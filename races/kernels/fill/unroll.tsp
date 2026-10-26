; fill 8 KB with a byte, the 32 pages unrolled with macros.unroll: one store a page for
; each value of the byte index, so every store is to a fixed address plus i

(import macros)

(let buf (array u8 8192) :address 0x2000)
(let value u8 :address 0x5F00)

(fn main () void
    (let v u8 value)
    (for i u8 0 256
        (macros.unroll page 0 32
            (set (at buf (+ (* page 256) (as u16 i))) v))))
