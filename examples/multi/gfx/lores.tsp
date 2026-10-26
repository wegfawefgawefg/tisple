; lo-res graphics: 40 x 48 blocks in 16 colors, two blocks per byte of text page 1

(const TEXT_PAGE1 u16 0x0400)

; text rows are interleaved: 8 blocks of 128 bytes, each holding rows n, n+8, n+16
(fn row_base (args (row u8))
    (+ (+ TEXT_PAGE1 (<< (u16 (& row 7)) 7)) (* (u16 (>> row 3)) 40)))

(fn show
    (io_read u8 0xC050)  ; graphics
    (io_read u8 0xC052)  ; full screen
    (io_read u8 0xC054)  ; page 1
    (io_read u8 0xC056)) ; lo-res

; even y is the low nibble, odd y the high one
(fn plot (args (x u8) (y u8) (color u8))
    (let p (as (ptr u8) (+ (row_base (>> y 1)) (u16 x))))
    (let old (load p))
    (if (= (& y 1) 0)
        (store p (| (& old 0xF0) color))
        (store p (| (& old 0x0F) (<< color 4)))))
