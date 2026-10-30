; pictures streamed from the disk while a ball keeps bouncing: four hi-res pictures live on
; the disk (tables :on disk), two as they are and two packed. Each is read onto the hidden
; page, or packed into aux memory and unpacked from there, while the step between sectors
; moves the ball on the page shown and grows a bar under it; then the pages flip. A key ends
; it. It needs its disk: tisple run examples/disk_stream.tsp --disk

(import apple2 a2)
(import disk)
(import tables)
(import unpack)

(let page1 (array u8 8192) :address a2.HGR_PAGE1 :reserve)
(let page2 (array u8 8192) :address a2.HGR_PAGE2 :reserve)
(let packed_room (array u8 8192) :address 0x6000 :in aux :reserve)

; the pictures, worked out while compiling. Colour k's bytes for an even and an odd column
; (violet, green, white, orange, blue); the colour of block b (4 byte columns) in band r (12
; rows) for each look; band r's 40 bytes; and a page's 8192 in the screen's order: each 1 KB
; eighth holds rows e, e + 8, ... e + 56, each 128 bytes rows y, y + 64, y + 128 and 8 bytes of
; screen hole. A row is band y / 12's, so 16 rows are worked out and copied
(macro colour_pair (k)
    (match k
        (1 (list 0x55 0x2A))
        (2 (list 0x2A 0x55))
        (3 (list 0x7F 0x7F))
        (4 (list 0xAA 0xD5))
        (_ (list 0xD5 0xAA))))

(macro block_colour (look r b)
    (match look
        (0 (+ 1 (% (/ r 2) 5))) ; bands across
        (1 (+ 1 (% b 5)))       ; bands down
        (2
            (if (= (% (+ b (/ r 2)) 2) 0)
                4
                5))               ; a checkerboard
        (_ (+ 1 (% (+ b r) 5))))) ; steps

(macro band_bytes (look r)
    (let out (list))
    (for b 0 10
        (let pair (colour_pair (block_colour look r b)))
        (set out (append out pair pair)))
    out)

(macro block_bytes (bands y)
    (append (nth bands (/ y 12))
        (nth bands (/ (+ y 64) 12))
        (nth bands (/ (+ y 128) 12))
        (repeat 8 0)))

(macro eighth_bytes (bands e)
    (append (block_bytes bands e)
        (block_bytes bands (+ e 8))
        (block_bytes bands (+ e 16))
        (block_bytes bands (+ e 24))
        (block_bytes bands (+ e 32))
        (block_bytes bands (+ e 40))
        (block_bytes bands (+ e 48))
        (block_bytes bands (+ e 56))))

(macro page_bytes (look)
    (let bands (list))
    (for r 0 16
        (set bands (append bands (list (band_bytes look r)))))
    (append (eighth_bytes bands 0)
        (eighth_bytes bands 1)
        (eighth_bytes bands 2)
        (eighth_bytes bands 3)
        (eighth_bytes bands 4)
        (eighth_bytes bands 5)
        (eighth_bytes bands 6)
        (eighth_bytes bands 7)))

(macro picture (name look)
    `(const ,name (array u8 8192) (make (array u8 8192) ,@(page_bytes look)) :on disk))

(macro packed_picture (name look)
    (let bytes (pack (page_bytes look)))
    `(const ,name (array u8 ,(len bytes)) (make (array u8 ,(len bytes)) ,@bytes) :on disk))

(picture ACROSS 0)
(packed_picture DOWN 1)
(picture CHECKS 2)
(packed_picture STEPS 3)

(const PICTURES (array (ptr u8) 4)
    (make (array (ptr u8) 4)
        (addr_of (at ACROSS 0))
        (addr_of (at DOWN 0))
        (addr_of (at CHECKS 0))
        (addr_of (at STEPS 0))))

(const PACKED (array bool 4) (make (array bool 4) false true false true))

; row y's offset in a page
(macro row_offset (y)
    (+ (* (% y 8) 0x400) (+ (* (% (/ y 8) 8) 0x80) (* (/ y 64) 40))))

(tables.table ROW u16 192 row_offset)

; the ball, 2 bytes by 8 rows, drawn with xor so a second draw takes it off
(const BALL (array u8 16)
    (make (array u8 16)
        0x78 0x07 0x7E 0x1F 0x7E 0x1F 0x7F 0x3F 0x7F 0x3F 0x7E 0x1F 0x7E 0x1F 0x78 0x07))

(let shown u16 a2.HGR_PAGE1)
(let col u8 3)
(let row u8 40)
(let right bool true)
(let downward bool true)
(let sectors u8 0)

(fn draw_ball () void
    (for k u8 0 8
        (let p (ptr u8) (as (ptr u8) (+ shown (+ (at ROW (+ row k)) (as u16 col)))))
        (store p (^ (load p) (at BALL (* k 2))))
        (store (+ p 1) (^ (load (+ p 1)) (at BALL (+ (* k 2) 1))))))

(fn move_ball () void
    (if right
        (set col (+ col 1))
        (set col (- col 1)))
    (if (or (= col 0) (= col 38))
        (set right (not right)))
    (if downward
        (set row (+ row 3))
        (set row (- row 3)))
    (if (or (< row 4) (> row 170))
        (set downward (not downward))))

; between two sectors: the ball a step on, the bar (the picture's bits flipped) a byte longer
(fn step () void
    (draw_ball)
    (move_ball)
    (draw_ball)
    (if (< sectors 40)
        (for y u8 186 190
            (let p (ptr u8) (as (ptr u8) (+ shown (+ (at ROW y) (as u16 sectors)))))
            (store p (^ (load p) 0x7F))))
    (set sectors (+ sectors 1)))

(fn show ((page u16)) void
    (if (= page a2.HGR_PAGE1)
        (io_read u8 a2.LOWSCR)
        (io_read u8 a2.HISCR))
    (set shown page))

; picture k onto the hidden page while the ball goes on, then that page shown
(fn load_picture ((k u8)) void
    (let hidden u16 (if (= shown a2.HGR_PAGE1)
        a2.HGR_PAGE2
        a2.HGR_PAGE1))
    (set sectors 0)
    (draw_ball)
    (if (at PACKED k)
        (do
            (disk.read_with (as (ptr u8) 0x6000) (at PICTURES k) disk.AUX step)
            (unpack.aux_to_main (as (ptr u8) hidden) (as (ptr u8) 0x6000)))
        (disk.read_with (as (ptr u8) hidden) (at PICTURES k) disk.MAIN step))
    (draw_ball)
    (show hidden))

(fn main () void
    (let page (ptr u8) (as (ptr u8) a2.HGR_PAGE1))
    (for i u16 0 8192
        (store (+ page i) 0))
    (io_read u8 a2.TXTCLR)
    (io_read u8 a2.MIXCLR)
    (io_read u8 a2.HIRES)
    (show a2.HGR_PAGE1)
    (io_read u8 a2.KBDSTRB)
    (let k u8 0)
    (while (< (io_read u8 a2.KBD) 0x80)
        (load_picture k)
        (set k (& (+ k 1) 3)))
    (io_read u8 a2.KBDSTRB)
    (io_read u8 a2.TXTSET))
