; fill count bytes starting at dst with value

(fn fill ((dst (ptr u8)) (count u16) (value u8)) void
    (for i u16 0 count
        (store (+ dst i) value)))

; hi-res page 1 is 8 KB at $2000; zero bytes are black. :reserve keeps the program out of it
(let page1 (array u8 8192) :address 0x2000 :reserve)

(fn clear_hgr () void
    (fill (as (ptr u8) 0x2000) 0x2000 0))
