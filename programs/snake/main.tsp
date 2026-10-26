; Snake on the lo-res screen: arrows steer, eat the orange food, miss the walls and yourself
; a crash starts a new game; Esc quits

(import apple2 a2)
(import keys)
(import lores)
(import random)
(import speaker)
(import timing)

(enum Dir (up down left right))

(const WALL u8 lores.GREY)
(const BODY u8 lores.GREEN)
(const HEAD u8 lores.YELLOW)
(const FOOD u8 lores.ORANGE)

; the body as a ring of positions, head and tail indexes wrapping at 256 on their own
(let xs (array u8 256))
(let ys (array u8 256))
(let head u8 0)
(let tail u8 0)
(let grow u8 0) ; segments still to add
(let dir Dir Dir.right)
(let quit bool false)
(let score u16 0)

(fn draw_walls () void
    (for x u8 0 40
        (lores.plot x 0 WALL)
        (lores.plot x 47 WALL))
    (for y u8 1 47
        (lores.plot 0 y WALL)
        (lores.plot 39 y WALL)))

; somewhere empty inside the walls
(fn place_food () void
    (while true
        (let x u8 (+ (random.below 38) 1))
        (let y u8 (+ (random.below 46) 1))
        (if (= (lores.get x y) lores.BLACK)
            (do
                (lores.plot x y FOOD)
                (return)))))

(fn new_game () void
    (lores.clear)
    (draw_walls)
    (set head 0)
    (set tail 0)
    (set (at xs 0) 20)
    (set (at ys 0) 24)
    (lores.plot 20 24 HEAD)
    (set grow 4)
    (set dir Dir.right)
    (set score 0)
    (place_food))

; no turning back on yourself
(fn read_input () void
    (match (keys.read)
        (a2.KEY_UP
            (if (!= dir Dir.down)
                (set dir Dir.up)))
        (a2.KEY_DOWN
            (if (!= dir Dir.up)
                (set dir Dir.down)))
        (a2.KEY_LEFT
            (if (!= dir Dir.right)
                (set dir Dir.left)))
        (a2.KEY_RIGHT
            (if (!= dir Dir.left)
                (set dir Dir.right)))
        (a2.KEY_ESC (set quit true))
        (_)))

; one move; false on a crash
(fn step () bool
    (let x u8 (at xs head))
    (let y u8 (at ys head))
    (match dir
        (Dir.up (set y (- y 1)))
        (Dir.down (set y (+ y 1)))
        (Dir.left (set x (- x 1)))
        (Dir.right (set x (+ x 1))))
    (let hit u8 (lores.get x y))
    (if (= hit FOOD)
        (do
            (set grow (+ grow 3))
            (set score (+ score 1))
            (speaker.buzz 20 4)
            (place_food))
        (if (!= hit lores.BLACK)
            (return false)))
    (lores.plot (at xs head) (at ys head) BODY)
    (set head (+ head 1))
    (set (at xs head) x)
    (set (at ys head) y)
    (lores.plot x y HEAD)
    (if (> grow 0)
        (set grow (- grow 1))
        (do
            (lores.plot (at xs tail) (at ys tail) lores.BLACK)
            (set tail (+ tail 1))))
    true)

(fn main () void
    (lores.show)
    (new_game)
    (while (not quit)
        (timing.frames 4)
        (read_input)
        (if (not (step))
            (do
                (speaker.buzz 120 40)
                (timing.frames 60)
                (new_game)))))
