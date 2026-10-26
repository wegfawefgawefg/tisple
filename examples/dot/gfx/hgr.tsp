; hi-res page 1: 280 x 192, 7 pixels per byte, rows interleaved in memory

(import apple2 a2)

; start address of every row, split into low and high bytes.
; :boundary keeps each table inside one 256-byte page, so (at row_lo y)
; is always a 4-cycle LDA abs,Y
(let row_lo (array u8 192) :boundary 256)
(let row_hi (array u8 192) :boundary 256)

; byte and bit mask of every column, since there is no divide
(let col_byte (array u8 280))
(let col_bit (array u8 280))

(fn init () void
    (for y u8 0 192
        (let start u16
            (+ (+ a2.HGR_PAGE1 (<< (as u16 (& y 7)) 10))
                (+ (<< (as u16 (& (>> y 3) 7)) 7) (* (as u16 (>> y 6)) 40))))
        (set (at row_lo y) (as u8 start))
        (set (at row_hi y) (as u8 (>> start 8))))
    (let byte u8 0)
    (let bit u8 0)
    (for x u16 0 280
        (set (at col_byte x) byte)
        (set (at col_bit x) (<< 1 bit))
        (set bit (+ bit 1))
        (if (= bit 7)
            (do
                (set bit 0)
                (set byte (+ byte 1))))))

; any access to these soft switches flips them
(fn show () void
    (io_read u8 a2.TXTCLR)
    (io_read u8 a2.MIXCLR)
    (io_read u8 a2.LOWSCR)
    (io_read u8 a2.HIRES))

; xor one pixel, so drawing twice erases it
(fn flip ((x u16) (y u8)) void
    (let row u16 (| (<< (as u16 (at row_hi y)) 8) (as u16 (at row_lo y))))
    (let p (ptr u8) (+ (as (ptr u8) row) (at col_byte x)))
    (store p (^ (load p) (at col_bit x))))
