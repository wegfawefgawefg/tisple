; a fixed pool of sprites: no heap, just an array and a count

(const MAX_SPRITES u8 32)

(struct Sprite ((x u8) (y u8) (dx i8) (active bool)))

; stored one array per field, so (at sprites i x) is a single LDA sprites_x,X
(let sprites (array Sprite MAX_SPRITES))
(let sprite_count u8 0)

(fn spawn ((x u8) (y u8) (dx i8)) bool
    (if (= sprite_count MAX_SPRITES)
        (return false))
    (set (at sprites sprite_count) (make Sprite :x x :y y :dx dx :active true))
    (set sprite_count (+ sprite_count 1))
    true)

; move every live sprite sideways; x wraps at 256
(fn step_sprites () void
    (for i u8 0 sprite_count
        (if (at sprites i active)
            (set (at sprites i x) (+ (at sprites i x) (as u8 (at sprites i dx)))))))
