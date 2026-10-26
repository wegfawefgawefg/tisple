; fill 8 KB with a byte

(let buf (array u8 8192) :address 0x2000)
(let value u8 :address 0x5F00)

(fn main () void
    (let v u8 value)
    (for i u16 0 8192
        (set (at buf i) v)))
