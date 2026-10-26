; lo-res graphics: 40 x 48 blocks in 16 colors, two blocks per byte of text page 1

(import apple2 a2)

; text rows are interleaved: 8 blocks of 128 bytes, each holding rows n, n+8, n+16
(fn row_base ((row u8)) u16
    (+ (+ a2.TEXT_PAGE1 (<< (as u16 (& row 7)) 7)) (* (as u16 (>> row 3)) 40)))

(fn show () void
    (io_read u8 a2.TXTCLR)
    (io_read u8 a2.MIXCLR)
    (io_read u8 a2.LOWSCR)
    (io_read u8 a2.LORES))

; even y is the low nibble, odd y the high one
(fn plot ((x u8) (y u8) (color u8)) void
    (let p (ptr u8) (as (ptr u8) (+ (row_base (>> y 1)) (as u16 x))))
    (let old u8 (load p))
    (if (= (& y 1) 0)
        (store p (| (& old 0xF0) color))
        (store p (| (& old 0x0F) (<< color 4)))))
