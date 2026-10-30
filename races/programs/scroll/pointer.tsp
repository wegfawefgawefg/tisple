; program.tsp with pointers into the map and the screen, as program.c has.
; A side-scroller's renderer: a 128 x 24 map of 8 x 8 tiles drawn into a 256 x 192
; bitmap (32 bytes a row) at a pixel scroll, every frame in full: each screen byte is the
; left tile's line shifted left by the fine scroll and the right tile's shifted right.
; Animated tiles (water) roll their lines with the frame. A player walks at a fixed
; place on the screen, falling, climbing and taking coins from the map. 8 frames,
; 3 pixels a frame; the checksum folds the screen, the player's row and the score.

(const MAP_W u16 128)
(const ROW u16 32)
(const FRAMES u8 8)
(const SPEED u16 3)
(const SOLID u8 1)
(const ANIMATED u8 2)
(const COIN u8 13)

(const TILES (array u8 128)
    (make (array u8 128)
        0x00 0x00 0x00 0x00 0x00 0x00 0x00 0x00
        0xFF 0x81 0x81 0x81 0xFF 0x18 0x18 0x18
        0xFF 0x18 0x18 0x18 0xFF 0x81 0x81 0x81
        0xFF 0x81 0x81 0x81 0xFF 0x18 0x18 0x18
        0xFF 0x18 0x18 0x18 0xFF 0x81 0x81 0x81
        0xBC 0xCE 0xDB 0xF2 0xB5 0x50 0xBB 0x86
        0xD8 0xE5 0xEC 0xCF 0x6A 0xD5 0xA0 0x2B
        0x04 0x00 0x05 0x1C 0x5F 0xDA 0x85 0xD0
        0x20 0x23 0x1E 0x09 0xD4 0x5F 0x6A 0x75
        0x44 0x4A 0x47 0x26 0xE9 0xA4 0xCF 0x1A
        0x78 0x69 0xA0 0xC3 0x3E 0xE9 0x34 0xBF
        0x9C 0xB4 0x81 0xE0 0x73 0xAE 0x19 0x64
        0xB0 0xDF 0xE2 0xBD 0x28 0x73 0xFE 0x09
        0x00 0x3C 0x7E 0x66 0x66 0x7E 0x3C 0x00
        0x08 0x0D 0x24 0x57 0xD2 0xFD 0xC8 0x53
        0x24 0x28 0x7D 0x34 0xA7 0x82 0xAD 0xF8))

(const ATTR (array u8 16) (make (array u8 16) 0 1 1 1 1 1 1 1 1 0 0 0 0 0 2 2))

(let screen (array u8 6144) :address 0x2000)
(let map (array u8 3072) :address 0x4000)
(let checksum u16 :address 0x5F00)

(let player_row u8 2)
(let score u8 0)

; line of tile t, rolled by the frame when it is animated
(fn tile_line ((t u8) (line u8) (frame u8)) u8
    (let l u8 line)
    (if (!= (& (at ATTR t) ANIMATED) 0)
        (set l (& (+ line frame) 7)))
    (at TILES (+ (* t 8) l)))

(fn draw ((sx u16) (frame u8)) void
    (let fine u8 (as u8 (& sx 7)))
    (let back u8 (- 8 fine))
    (for r u8 0 24
        (let tiles (ptr u8) (addr_of (at map (+ (* (as u16 r) MAP_W) (>> sx 3)))))
        (for c u8 0 32
            (let left u8 (load (+ tiles c)))
            (let right u8 (load (+ tiles (+ c 1))))
            (let p (ptr u8) (addr_of (at screen (+ (* (as u16 r) (* 8 ROW)) (as u16 c)))))
            (for line u8 0 8
                (store p
                    (| (<< (tile_line left line frame) fine)
                        (>> (tile_line right line frame) back)))
                (set p (+ p ROW))))))

(fn move_player ((sx u16)) void
    (let col u16 (>> (+ sx 100) 3))
    (let cell u16 (+ (* (as u16 player_row) MAP_W) col))
    (if (and (= (& (at ATTR (at map (+ cell MAP_W))) SOLID) 0) (< player_row 22))
        (set player_row (+ player_row 1))
        (if (and (!= (& (at ATTR (at map (+ cell 1))) SOLID) 0) (> player_row 1))
            (set player_row (- player_row 1))))
    (set cell (+ (* (as u16 player_row) MAP_W) col))
    (if (= (at map cell) COIN)
        (do
            (set (at map cell) 0)
            (set score (+ score 1)))))

(fn main () void
    (set player_row 2)
    (set score 0)
    (for frame u8 0 FRAMES
        (let sx u16 (* (as u16 frame) SPEED))
        (draw sx frame)
        (move_player sx))
    (let sum_a u16 0)
    (let sum_b u16 0)
    (for n u16 0 6144
        (set sum_a (+ sum_a (as u16 (at screen n))))
        (set sum_b (+ sum_b sum_a)))
    (set sum_a (+ sum_a (as u16 player_row)))
    (set sum_b (+ sum_b sum_a))
    (set sum_a (+ sum_a (as u16 score)))
    (set sum_b (+ sum_b sum_a))
    (set checksum (^ sum_a sum_b)))
