; double hi-res drawn from colour pixels, a row at a time: (import dhgr_pixels) then
;   (dhgr_pixels.row page col top groups pixels)  groups 7-pixel groups (1-20) of row top of
;                                   page from byte column col (even) on: each pixel a byte of
;                                   main memory at pixels, its colour 0-15; whole bytes written
;                                   in both memories, what was there gone
;   (dhgr_pixels.read page col top groups pixels)  the other way: the row's bytes back to
;                                   pixels at pixels
;   (dhgr_pixels.cells page col top blocks cells)  a row of 4-pixel cells in white on black,
;                                   each a byte at cells (bit 0 the leftmost pixel; 4 bits),
;                                   7 cells a block (4 groups): a 35-column font's row
; For a hud drawn in whole pixels (text in colour pixels, badges, cards): the program builds a
; row of pixels, then this writes it. Per group the 4 bytes (aux, main, aux, main) are worked
; out with shifts and three small tables (a colour's low bits moved up into the next byte),
; main memory's two written, then aux memory's two in a with_aux that only writes (RAMWRT): the
; code reads main memory throughout, so it runs where the layout puts it. With an interrupt
; handler, (auxmem.guard) first, as for any with_aux.

(import dhgr)
(import tables)

; a pixel's colour bits that go into the byte before the next one: the low 3, 2 and 1 bits,
; moved up to the top of a byte's 7 dots
(macro lo4 (p)
    (<< (& p 7) 4))

(macro lo5 (p)
    (<< (& p 3) 5))

(macro lo6 (p)
    (<< (& p 1) 6))

(tables.table LO4 u8 16 lo4)
(tables.table LO5 u8 16 lo5)
(tables.table LO6 u8 16 lo6)

; row top's byte column col on page
(fn row_start ((page u8) (col u8) (top u8)) (ptr u8)
    (dhgr.row_at top (dhgr.start page col)))

(fn row ((page u8) (col u8) (top u8) (groups u8) (pixels (ptr u8))) void
    (let dst (ptr u8) (row_start page col top))
    (let c u8 0) ; the group's first byte column, from col
    (for g u8 0 groups
        (let p1 u8 (load (+ pixels 1)))
        (let b0 u8 (| (load pixels) (at LO4 p1)))
        (let p3 u8 (load (+ pixels 3)))
        (let b1 u8 (| (| (>> p1 3) (<< (load (+ pixels 2)) 1)) (at LO5 p3)))
        (let p5 u8 (load (+ pixels 5)))
        (let b2 u8 (| (| (>> p3 2) (<< (load (+ pixels 4)) 2)) (at LO6 p5)))
        (let b3 u8 (| (>> p5 1) (<< (load (+ pixels 6)) 3)))
        (store (+ dst c) b1)
        (store (+ dst (+ c 1)) b3)
        (with_aux false true
            (store (+ dst (+ c 1)) b2)
            (store (+ dst c) b0))
        (set pixels (+ pixels 7))
        (set c (+ c 2))))

(let aux_bytes (array u8 40))
(let main_bytes (array u8 40))

(fn read ((page u8) (col u8) (top u8) (groups u8) (pixels (ptr u8))) void
    (let count u8 (<< groups 1))
    (let p (ptr u8) (row_start page col top))
    (for i u8 0 count
        (set (at main_bytes i) (load (+ p i))))
    (with_aux true false
        (for i u8 0 count
            (set (at aux_bytes i) (load (+ p i)))))
    ; a group's bytes aux, main, aux, main hold its 7 pixels' 4 dots each, bit 0 first
    (let c u8 0)
    (for g u8 0 groups
        (let b0 u8 (at aux_bytes c))
        (let b1 u8 (at main_bytes c))
        (let b2 u8 (at aux_bytes (+ c 1)))
        (let b3 u8 (at main_bytes (+ c 1)))
        (store pixels (& b0 0x0F))
        (store (+ pixels 1) (| (<< (& b1 0x01) 3) (>> b0 4)))
        (store (+ pixels 2) (& (>> b1 1) 0x0F))
        (store (+ pixels 3) (| (<< (& b2 0x03) 2) (>> b1 5)))
        (store (+ pixels 4) (& (>> b2 2) 0x0F))
        (store (+ pixels 5) (| (<< (& b3 0x07) 1) (& (>> b2 6) 0x01)))
        (store (+ pixels 6) (>> b3 3))
        (set pixels (+ pixels 7))
        (set c (+ c 2))))

; a group of 7 pixels, lit where its bit is (bit 0 the leftmost), white on black: its 4 bytes,
; each pixel 4 dots, a byte 7 of the 28: MONO0 aux, MONO1 main, MONO2 aux, MONO3 main
(macro mono (pattern byte)
    (let b 0)
    (for k 0 7
        (let dot (+ (* byte 7) k))
        (if (!= (& (>> pattern (/ dot 4)) 1) 0)
            (set b (| b (<< 1 k)))))
    b)

(macro mono0 (pattern)
    (mono pattern 0))

(macro mono1 (pattern)
    (mono pattern 1))

(macro mono2 (pattern)
    (mono pattern 2))

(macro mono3 (pattern)
    (mono pattern 3))

(tables.table MONO0 u8 128 mono0)
(tables.table MONO1 u8 128 mono1)
(tables.table MONO2 u8 128 mono2)
(tables.table MONO3 u8 128 mono3)

(fn cells ((page u8) (col u8) (top u8) (blocks u8) (nibbles (ptr u8))) void
    (let dst (ptr u8) (row_start page col top))
    (for b u8 0 blocks
        ; the block's 7 cells' bits, 4 a cell, as 4 groups of 7 pixels
        (let c1 u8 (load (+ nibbles 1)))
        (let g0 u8 (| (load nibbles) (<< (& c1 7) 4)))
        (let c3 u8 (load (+ nibbles 3)))
        (let g1 u8 (| (| (>> c1 3) (<< (load (+ nibbles 2)) 1)) (<< (& c3 3) 5)))
        (let c5 u8 (load (+ nibbles 5)))
        (let g2 u8 (| (| (>> c3 2) (<< (load (+ nibbles 4)) 2)) (<< (& c5 1) 6)))
        (let g3 u8 (| (>> c5 1) (<< (load (+ nibbles 6)) 3)))
        ; main memory's bytes of the 4 groups, then aux memory's
        (store dst (at MONO1 g0))
        (store (+ dst 1) (at MONO3 g0))
        (store (+ dst 2) (at MONO1 g1))
        (store (+ dst 3) (at MONO3 g1))
        (store (+ dst 4) (at MONO1 g2))
        (store (+ dst 5) (at MONO3 g2))
        (store (+ dst 6) (at MONO1 g3))
        (store (+ dst 7) (at MONO3 g3))
        (with_aux false true
            (store dst (at MONO0 g0))
            (store (+ dst 1) (at MONO2 g0))
            (store (+ dst 2) (at MONO0 g1))
            (store (+ dst 3) (at MONO2 g1))
            (store (+ dst 4) (at MONO0 g2))
            (store (+ dst 5) (at MONO2 g2))
            (store (+ dst 6) (at MONO0 g3))
            (store (+ dst 7) (at MONO2 g3)))
        (set nibbles (+ nibbles 7))
        (set dst (+ dst 8))))
