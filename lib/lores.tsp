; lo-res graphics, page 1: 40 x 48 blocks in 16 colors, two blocks per byte of text page 1
; (import lores) then (lores.show), (lores.plot x y color), (lores.clear)

(import apple2 a2)

; start of each of the 24 text rows: rows are interleaved in thirds of 40 bytes
(const ROW_START (array u16 24)
    (make (array u16 24)
        0x0400 0x0480 0x0500 0x0580 0x0600 0x0680 0x0700 0x0780
        0x0428 0x04A8 0x0528 0x05A8 0x0628 0x06A8 0x0728 0x07A8
        0x0450 0x04D0 0x0550 0x05D0 0x0650 0x06D0 0x0750 0x07D0)
    :boundary 256)

(const BLACK u8 0)
(const MAGENTA u8 1)
(const DARK_BLUE u8 2)
(const PURPLE u8 3)
(const DARK_GREEN u8 4)
(const GREY u8 5)
(const MEDIUM_BLUE u8 6)
(const LIGHT_BLUE u8 7)
(const BROWN u8 8)
(const ORANGE u8 9)
(const GREY2 u8 10)
(const PINK u8 11)
(const GREEN u8 12)
(const YELLOW u8 13)
(const AQUA u8 14)
(const WHITE u8 15)

; graphics, full screen, page 1, lo-res
(fn show () void
    (io_read u8 a2.TXTCLR)
    (io_read u8 a2.MIXCLR)
    (io_read u8 a2.LOWSCR)
    (io_read u8 a2.LORES))

; a pointer to the start of text row r, which holds block rows 2r (low nibble) and 2r + 1
(fn row ((r u8)) (ptr u8)
    (as (ptr u8) (at ROW_START r)))

; every block black; leaves the 8 hidden bytes after each row alone
(fn clear () void
    (for r u8 0 24
        (let p (ptr u8) (row r))
        (for x u8 0 40
            (store (+ p x) BLACK))))

(fn plot ((x u8) (y u8) (color u8)) void
    (let p (ptr u8) (+ (row (>> y 1)) x))
    (let old u8 (load p))
    (if (= (& y 1) 0)
        (store p (| (& old 0xF0) color))
        (store p (| (& old 0x0F) (<< color 4)))))

; the color of block x, y
(fn get ((x u8) (y u8)) u8
    (let b u8 (load (+ (row (>> y 1)) x)))
    (if (= (& y 1) 0)
        (& b 0x0F)
        (>> b 4)))
