; hi-res plotting with a table of row start addresses, since rows are interleaved in memory

(import tables)

(const HGR_PAGE1 u16 0x2000)

; ROW_ADDR[y] = $2000 + (y mod 8) * $400 + ((y / 8) mod 8) * $80 + (y / 64) * $28
; stored as a low-byte and a high-byte table; :boundary keeps each inside one page
(macro row_addr (y)
    (+ (+ HGR_PAGE1 (* (% y 8) 0x400)) (+ (* (% (/ y 8) 8) 0x80) (* (/ y 64) 0x28))))

(tables.table ROW_ADDR u16 192 row_addr :boundary 256)

; x is 0..279; seven pixels per byte, low bit leftmost, bit 7 picks the color group
(fn plot ((x u16) (y u8)) void
    (let p (ptr u8) (as (ptr u8) (+ (at ROW_ADDR y) (/ x 7))))
    (let mask u8 (<< 1 (as u8 (% x 7))))
    (store p (| (load p) mask)))

(fn hline ((x0 u16) (x1 u16) (y u8)) void
    (for x u16 x0 x1
        (plot x y)))
