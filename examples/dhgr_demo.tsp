; double hi-res with page flipping: a sky of colour bands, a sun and stars plotted a pixel at a
; time, and a ball bouncing over them, drawn on the hidden page while the other one shows, then
; flipped at the next vertical blank (30 times a second: a frame's drawing takes a little more
; than one). The four text lines under the picture are 80 columns, and each page has its own
; (text page 1 or 2), so the count of flips changes with the page. ESC stops it.

(import apple2 a2)
(import dhgr)
(import dhgr_text2) ; the flip count on text page 2
(import tables)
(import timing)

; ---- the ball: 14 pixels (two 7-pixel groups, 4 bytes in each memory) by 22 rows

(const BALL_W u8 4)
(const BALL_H u8 22)

; a hex digit is a colour (lo-res numbers), . is clear
(macro ball_rows ()
    (list "....FFFFFF...." "...FDDDDDDF..." "..FDDDDDDDDF.." ".FDDDDDDDDDDF." ".FDDDDDDDDDDF."
        "FDDD99DDDDDDDF" "FDD9999DDDDDDF" "FDD9999DDDDDDF" "FDDD99DDDDDDDF" "FDDDDDDDDDDDDF"
        "FDDDDDDDDDDDDF" "FDDDDDDDDDDDDF" "FDDDDDDDDDDD9F" "FDDDDDDDDDDD9F" "FDDDDDDDDDD99F"
        ".FDDDDDDDDD9F." ".FDDDDDDDD99F." "..FDDDDDD99F.." "...F999999F..." "....FFFFFF...."
        ".............." ".............."))

(macro hex_digit (ch)
    (if (<= ch 57)
        (- ch 48)
        (- ch 55)))

; byte i of one memory's half: w * h picture bytes, then as many mask bytes (bits of clear
; pixels set, and bit 7, which the screen does not show). Byte k of a row is byte (k % 2) * 2
; (aux) or (k % 2) * 2 + 1 (main) of group k / 2; that byte holds the group's dots 7b to 7b + 6,
; dot d of pixel d / 4, lit when bit d & 3 of its colour is
(macro ball_byte (half i)
    (let rows (ball_rows))
    (let w 4)
    (let mask (>= i (* w 22)))
    (let j (% i (* w 22)))
    (let row (chars (nth rows (/ j w))))
    (let k (% j w))
    (let b
        (+ (* (% k 2) 2) (if (= half 1)
            0
            1)))
    (let out (if mask
        128
        0))
    (for bit 0 7
        (let dot (+ (* 7 b) bit))
        (let ch (nth row (+ (* (/ k 2) 7) (/ dot 4))))
        (if (= ch 46)
            (if mask
                (set out (| out (<< 1 bit))))
            (if (and (not mask) (!= (& (>> (hex_digit ch) (& dot 3)) 1) 0))
                (set out (| out (<< 1 bit))))))
    out)

(macro ball_half (name half & options)
    `(let ,name (array u8 176)
        (make (array u8 176)
            ,@(map
                (fn (i)
                    (ball_byte half i))
                (range 0 176)))
        ,@options))

(ball_half ball_aux 1 :in aux)
(ball_half ball_main 0)

; ---- the sky, a band at a time down to the ground, and the text window under it

(const BANDS u8 7)
(const BAND_TOP (array u8 8) (make (array u8 8) 0 26 50 72 92 110 126 160))

(const BAND_COLOUR (array u8 7)
    (make (array u8 7)
        dhgr.DARK_BLUE dhgr.PURPLE dhgr.MEDIUM_BLUE dhgr.LIGHT_BLUE dhgr.PINK
        dhgr.GREEN dhgr.DARK_GREEN))

; the bands over the box of byte columns col to col + w - 1, rows top to top + h - 1
(fn sky ((page u8) (col u8) (top u8) (w u8) (h u8)) void
    (let bottom u8 (+ top h))
    (for b u8 0 BANDS
        (let t u8 (at BAND_TOP b))
        (let e u8 (at BAND_TOP (+ b 1)))
        (if (< t top)
            (set t top))
        (if (> e bottom)
            (set e bottom))
        (if (< t e)
            (dhgr.box page col t w (- e t) (at BAND_COLOUR b)))))

; the sun: an ellipse of plotted pixels, wide pixels being 1.83 rows across
(macro sun_half_width (dy)
    (round (/ (* 100 (sqrt (- 196 (* dy dy)))) 183)))

(tables.table SUN_W u8 29 (fn (i)
    (sun_half_width (- i 14))))

(fn sun ((page u8)) void
    (for r u8 0 29
        (let half u8 (at SUN_W r))
        (for x u8 (- 116 half) (+ 117 half)
            (dhgr.plot page x (+ r 4) dhgr.YELLOW))))

(const STARS u8 24)

(macro star_x (i)
    (% (* i 37) 100))

(macro star_y (i)
    (% (* i 11) 24))

(tables.table STAR_X u8 24 star_x)
(tables.table STAR_Y u8 24 star_y)

(fn star ((page u8) (i u8) (colour u8)) void
    (dhgr.plot page (at STAR_X i) (at STAR_Y i) colour))

; three stars a frame twinkle: one goes grey, the one before it white again
(fn twinkle ((page u8) (frame u8)) void
    (let i u8 (% frame STARS))
    (star page i dhgr.GREY)
    (star page (% (+ i 23) STARS) dhgr.WHITE)
    (star page (% (+ i 17) STARS) dhgr.WHITE))

(fn scene ((page u8)) void
    (sky page 0 0 40 160)
    (sun page)
    (for i u8 0 STARS
        (star page i dhgr.WHITE)))

; ---- 80-column text: screen codes, normal characters with bit 7 set

; a row of each page's text, for the frame count
(let lines (array u8 160))

(fn say ((page u8) (row u8) (text (ptr u8))) void
    (let line (ptr u8)
        (addr_of
            (at lines (if (= page dhgr.PAGE1)
                0
                80))))
    (let i u8 0)
    (while (and (< i 80) (!= (load (+ text i)) 0))
        (store (+ line i) (| (load (+ text i)) 0x80))
        (set i (+ i 1)))
    (while (< i 80)
        (store (+ line i) 0xA0)
        (set i (+ i 1)))
    (dhgr.text_row page row line))

; the count of flips in decimal at the end of the row say put on row 23 last
(fn say_frame ((page u8) (n u16)) void
    (let line (ptr u8)
        (addr_of
            (at lines (if (= page dhgr.PAGE1)
                0
                80))))
    (let v u16 n)
    (for k u8 0 5
        (store (+ line (- 74 k)) (+ 0xB0 (as u8 (% v 10))))
        (set v (/ v 10)))
    (dhgr.text_row page 23 line))

; ---- the ball's flight

(let col u8 4) ; even byte columns: the ball's bytes are for a group's start
(let top u8 60)
(let dy i8 0)
(let dcol u8 2)
(let old_col (array u8 2))
(let old_top (array u8 2))

(fn step ((frame u16)) void
    (set dy (+ dy 1)) ; falling
    (let next i16 (+ (as i16 top) (as i16 (>> dy 1))))
    (if (> next 104)
        (do
            (set next 104)
            (set dy -14)))
    (set top (as u8 next))
    (if (= (& (as u8 frame) 3) 0)
        (do
            (if (and (= dcol 2) (>= col 36))
                (set dcol 254))
            (if (and (= dcol 254) (= col 0))
                (set dcol 2))
            (set col (+ col dcol)))))

; ESC pressed (examples/ has a keys.tsp of its own, so the keyboard is read here)
(fn escape () bool
    (let k u8 (io_read u8 a2.KBD))
    (if (< k 0x80)
        (return false))
    (io_read u8 a2.KBDSTRB)
    (= (& k 0x7F) a2.KEY_ESC))

(fn main () void
    (for p u8 0 2
        (let page u8 (if (= p 0)
            dhgr.PAGE1
            dhgr.PAGE2))
        (scene page)
        (say page 20 "DOUBLE HI-RES: 140 x 192, 15 colours, drawn on the hidden page and flipped.")
        (say page 21
            "The sky is boxes, the sun and stars are plotted, the ball is a masked sprite.")
        (say page 22 "Press ESC to stop.")
        (say page 23 "Each page has its own text page; this one counts the flips:")
        (set (at old_col p) col)
        (set (at old_top p) top))
    (dhgr.on true)
    (let frame u16 0)
    (while (not (escape))
        (let page u8 (dhgr.hidden))
        (let k u8 (if (= page dhgr.PAGE1)
            0
            1))
        (sky page (at old_col k) (at old_top k) BALL_W BALL_H)
        (twinkle page (as u8 frame))
        (dhgr.masked page dhgr.AUX col top BALL_W BALL_H
            (addr_of (at ball_aux 0))
            (addr_of (at ball_aux 88)))
        (dhgr.masked page dhgr.MAIN col top BALL_W BALL_H
            (addr_of (at ball_main 0))
            (addr_of (at ball_main 88)))
        (set (at old_col k) col)
        (set (at old_top k) top)
        (say_frame page frame)
        (timing.frame)
        (dhgr.flip)
        (set frame (+ frame 1))
        (step frame))
    (dhgr.off))
