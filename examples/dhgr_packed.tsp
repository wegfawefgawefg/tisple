; a double hi-res picture worked out and packed while compiling, kept packed in aux memory,
; and unpacked onto page 1 when the program runs: the aux half straight into aux memory, the
; main half from aux memory into main. Each half is 8 KB; packed, both take 1.7 KB. Working
; out the 16384 bytes in macro code is most of the 0.7 s this takes to compile; packing them is
; 10 ms. A key ends it.

(import apple2 a2)
(import dhgr)
(import tables)
(import unpack)

; the picture: pixel x (0-139), row y. Hills of greens under a sky, a moon with a rim
(macro colour_at (x y)
    (let dx (- x 100))
    (let dy (- y 40))
    (let d (+ (* (* dx dx) 335) (* (* dy dy) 100)))
    (let hill (+ 110 (round (* 18 (sin (/ x (float 9)))))))
    (if (< d 22500)
        (if (> d 16900)
            dhgr.WHITE
            dhgr.YELLOW)
        (if (> y (+ hill (/ (- 140 x) 5)))
            (if (= (& (+ x y) 7) 0)
                dhgr.GREEN
                dhgr.DARK_GREEN)
            (if (> y hill)
                dhgr.GREEN
                (if (< y 60)
                    dhgr.DARK_BLUE
                    (if (= (& (+ x (* y 3)) 3) 0)
                        dhgr.PURPLE
                        dhgr.DARK_BLUE))))))

; byte i of a half (0 main, 1 aux) in the page's order: which row and column it is, then its 7
; dots (aux first in each column), dot d lit when bit d & 3 of its pixel's colour is
(macro page_byte (half i)
    (let third (/ (% (% i 0x400) 0x80) 40))
    (let col (% (% (% i 0x400) 0x80) 40))
    (if (= third 3)
        0
        (do
            (let y (+ (* third 64) (+ (* (/ (% i 0x400) 0x80) 8) (/ i 0x400))))
            (let b 0)
            (for k 0 7
                (let dot
                    (+ (* col 14)
                        (+ k (if (= half 1)
                            0
                            7))))
                (if (!= (& (>> (colour_at (/ dot 4) y) (& dot 3)) 1) 0)
                    (set b (| b (<< 1 k)))))
            b)))

(macro main_byte (i)
    (page_byte 0 i))

(macro aux_byte (i)
    (page_byte 1 i))

(tables.packed picture_main 8192 main_byte :in aux)
(tables.packed picture_aux 8192 aux_byte :in aux)

(fn main () void
    (dhgr.on false)
    (dhgr.clear dhgr.PAGE1 dhgr.BLACK)
    (unpack.aux_to_aux (as (ptr u8) 0x2000) (addr_of (at picture_aux 0)))
    (unpack.aux_to_main (as (ptr u8) 0x2000) (addr_of (at picture_main 0)))
    (while (< (io_read u8 a2.KBD) 0x80))
    (io_read u8 a2.KBDSTRB)
    (dhgr.off))
