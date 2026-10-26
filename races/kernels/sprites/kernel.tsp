; move 64 sprites for 16 frames, bouncing off the edges; an array of structs, which
; Tisple stores one array per field

(struct Sprite ((x u8) (y u8) (dx i8) (dy i8) (frame u8)))

(let sprites (array Sprite 64) :address 0x4000)

(fn main () void
    (for f u8 0 16
        (for i u8 0 64
            (set (at sprites i x) (+ (at sprites i x) (as u8 (at sprites i dx))))
            (if (or (< (at sprites i x) 8) (>= (at sprites i x) 240))
                (set (at sprites i dx) (- 0 (at sprites i dx))))
            (set (at sprites i y) (+ (at sprites i y) (as u8 (at sprites i dy))))
            (if (or (< (at sprites i y) 8) (>= (at sprites i y) 184))
                (set (at sprites i dy) (- 0 (at sprites i dy))))
            (set (at sprites i frame) (& (+ (at sprites i frame) 1) 7)))))
