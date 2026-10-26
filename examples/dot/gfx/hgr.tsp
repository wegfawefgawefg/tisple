; hi-res page 1: 280 x 192, 7 pixels per byte, rows interleaved in memory

(const HGR_PAGE1 u16 0x2000)

; start address of every row, split into low and high bytes.
; :boundary keeps each table inside one 256-byte page, so (at row_lo y)
; is always a 4-cycle LDA abs,Y
(let row_lo (array u8 192) :boundary 256)
(let row_hi (array u8 192) :boundary 256)

; byte and bit mask of every column, since there is no divide
(let col_byte (array u8 280))
(let col_bit (array u8 280))

(fn init
    (for y 0 192
        (let addr
            (+ (+ HGR_PAGE1 (shl (u16 (bit_and y 7)) 10))
                (+ (shl (u16 (bit_and (shr y 3) 7)) 7) (* (u16 (shr y 6)) 40))))
        (set (at row_lo y) (u8 addr))
        (set (at row_hi y) (u8 (shr addr 8))))
    (let byte u8 0)
    (let bit u8 0)
    (for x 0 280
        (set (at col_byte x) byte)
        (set (at col_bit x) (shl 1 bit))
        (set bit (+ bit 1))
        (if (= bit 7)
            (do
                (set bit 0)
                (set byte (+ byte 1))))))

; any access to these soft switches flips them: graphics, full screen, page 1, hi-res
(fn show
    (io_read u8 0xC050)
    (io_read u8 0xC052)
    (io_read u8 0xC054)
    (io_read u8 0xC057))

; xor one pixel, so drawing twice erases it
(fn flip (args (x u16) (y u8))
    (let row (bit_or (shl (u16 (at row_hi y)) 8) (u16 (at row_lo y))))
    (let p (ptr u8 (+ row (u16 (at col_byte x)))))
    (store p (bit_xor (load p) (at col_bit x))))
