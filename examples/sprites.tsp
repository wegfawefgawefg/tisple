; a fixed pool of sprites: no heap, just an array and a count

(const MAX_SPRITES u8 32)

(struct Sprite (x u8) (y u8) (dx i8) (active bool))

; stored one array per field, so (. (at sprites i) x) is a single LDA sprites_x,X
(let sprites (array Sprite MAX_SPRITES))
(let sprite_count u8 0)

(fn spawn (args (x u8) (y u8) (dx i8)) (returns bool)
    (if (= sprite_count MAX_SPRITES)
        (return false))
    (set (at sprites sprite_count) (Sprite :x x :y y :dx dx :active true))
    (set sprite_count (+ sprite_count 1))
    true)

; move every live sprite sideways; x wraps at 256
(fn step_sprites
    (for i 0 sprite_count
        (if (. (at sprites i) active)
            (set (. (at sprites i) x) (+ (. (at sprites i) x) (u8 (. (at sprites i) dx)))))))
