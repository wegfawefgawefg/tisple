; copy 4 KB, the 16 pages unrolled with macros.unroll: one copy a page for each value of
; the byte index, so every access is a fixed address plus i

(import macros)

(let src (array u8 4096) :address 0x2000)
(let dst (array u8 4096) :address 0x4000)

(fn main () void
    (for i u8 0 256
        (macros.unroll page 0 16
            (set (at dst (+ (* page 256) (as u16 i))) (at src (+ (* page 256) (as u16 i)))))))
