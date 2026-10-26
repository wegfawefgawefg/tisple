; hi-res page 1: 280 x 192, 7 pixels per byte, rows interleaved in memory

(import apple2 a2)
(import tables)

; the address of row y: eighths of the page, then 8-row bands, then thirds. A plain
; function: the table below calls it while compiling
(fn row_start ((y u8)) u16
    (+ (+ a2.HGR_PAGE1 (<< (as u16 (& y 7)) 10))
        (+ (<< (as u16 (& (>> y 3) 7)) 7) (* (as u16 (>> y 6)) 40))))

; start address of every row, stored as a low-byte and a high-byte table.
; :boundary keeps each inside one 256-byte page, so (at ROW y) is two 4-cycle LDA abs,Y
(tables.table ROW u16 192 row_start :boundary 256)

; byte and bit mask of every column, since there is no divide
(macro col_byte (x)
    (/ x 7))

(macro col_bit (x)
    (<< 1 (% x 7)))

(tables.table COL_BYTE u8 280 col_byte)
(tables.table COL_BIT u8 280 col_bit)

; any access to these soft switches flips them
(fn show () void
    (io_read u8 a2.TXTCLR)
    (io_read u8 a2.MIXCLR)
    (io_read u8 a2.LOWSCR)
    (io_read u8 a2.HIRES))

; xor one pixel, so drawing twice erases it
(fn flip ((x u16) (y u8)) void
    (let p (ptr u8) (+ (as (ptr u8) (at ROW y)) (at COL_BYTE x)))
    (store p (^ (load p) (at COL_BIT x))))
