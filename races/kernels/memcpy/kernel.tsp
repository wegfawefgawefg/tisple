; copy 4 KB

(let src (array u8 4096) :address 0x2000)
(let dst (array u8 4096) :address 0x4000)

(fn main () void
    (for i u16 0 4096
        (set (at dst i) (at src i))))
