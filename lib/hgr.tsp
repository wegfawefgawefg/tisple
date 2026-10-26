; hi-res graphics, page 1: 280 x 192 pixels, 7 to a byte (bit 0 leftmost), rows interleaved
; (import hgr) then (hgr.show), (hgr.clear), (hgr.plot x y), (hgr.flip x y),
; (hgr.line x0 y0 x1 y1). Pixels are bits, so a lone one shows in colour on a colour screen.

(import apple2 a2)
(import tables)

; the address of row y: eighths of the page, then 8-row bands, then thirds. A plain
; function: the table below calls it while compiling
(fn row_start ((y u8)) u16
    (+ (+ a2.HGR_PAGE1 (<< (as u16 (& y 7)) 10))
        (+ (<< (as u16 (& (>> y 3) 7)) 7) (* (as u16 (>> y 6)) 40))))

; start address of every row, as a low-byte and a high-byte table, each in one page
(tables.table ROW u16 192 row_start :boundary 256)

; byte and bit mask of every column, since there is no divide
(macro col_byte (x)
    (/ x 7))

(macro col_bit (x)
    (<< 1 (% x 7)))

(tables.table COL_BYTE u8 280 col_byte)
(tables.table COL_BIT u8 280 col_bit)

; graphics, full screen, page 1, hi-res; any access to these soft switches flips them
(fn show () void
    (io_read u8 a2.TXTCLR)
    (io_read u8 a2.MIXCLR)
    (io_read u8 a2.LOWSCR)
    (io_read u8 a2.HIRES))

; every pixel of page 1 off
(fn clear () void
    (let page (ptr u8) (as (ptr u8) a2.HGR_PAGE1))
    (for i u16 0 8192
        (store (+ page i) 0)))

; the byte that holds pixel x of row y
(fn pixel_byte ((x u16) (y u8)) (ptr u8)
    (+ (as (ptr u8) (at ROW y)) (at COL_BYTE x)))

; one pixel on
(fn plot ((x u16) (y u8)) void
    (let p (ptr u8) (pixel_byte x y))
    (store p (| (load p) (at COL_BIT x))))

; xor one pixel, so drawing twice erases it
(fn flip ((x u16) (y u8)) void
    (let p (ptr u8) (pixel_byte x y))
    (store p (^ (load p) (at COL_BIT x))))

; every pixel of the line from x0, y0 to x1, y1 on, both ends included (Bresenham)
(fn line ((x0 u16) (y0 u8) (x1 u16) (y1 u8)) void
    (let x i16 (as i16 x0))
    (let y i16 (as i16 y0))
    (let x_end i16 (as i16 x1))
    (let y_end i16 (as i16 y1))
    (let dx i16 (- x_end x))
    (let step_x i16 1)
    (if (< dx 0)
        (do
            (set dx (- 0 dx))
            (set step_x -1)))
    (let dy i16 (- y y_end))
    (let step_y i16 1)
    (if (> dy 0)
        (do
            (set dy (- 0 dy))
            (set step_y -1)))
    ; dy is minus the height; err tracks how far the drawn line is off the real one
    (let err i16 (+ dx dy))
    (while true
        (plot (as u16 x) (as u8 y))
        (if (and (= x x_end) (= y y_end))
            (break))
        (let twice i16 (<< err 1))
        (if (>= twice dy)
            (do
                (set err (+ err dy))
                (set x (+ x step_x))))
        (if (<= twice dx)
            (do
                (set err (+ err dx))
                (set y (+ y step_y))))))
