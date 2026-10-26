; fill count bytes starting at dst with value

(fn fill ((dst (ptr u8)) (count u16) (value u8)) void
    (for i u16 0 count
        (store (+ dst i) value)))

; hi-res page 1 is 8 KB at $2000; zero bytes are black
(fn clear_hgr () void
    (fill (as (ptr u8) 0x2000) 0x2000 0))
