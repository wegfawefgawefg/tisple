; per-object update functions from a dispatch table: each kind of thing on the text screen
; moves its own way, its step picked once when it is spawned

(import text40)
(import timing)

(struct Thing ((x u8) (y u8) (step (fn (u8) void))))

(const MAX_THINGS u8 8)

(let things (array Thing MAX_THINGS))
(let thing_count u8 0)

(fn walk ((i u8)) void
    (set (at things i x) (% (+ (at things i x) 1) 40)))

(fn fall ((i u8)) void
    (set (at things i y) (% (+ (at things i y) 1) 24)))

(fn hop ((i u8)) void
    (set (at things i x) (% (+ (at things i x) 2) 40))
    (set (at things i y) (^ (at things i y) 1)))

; by kind: a const table, two byte tables of the functions' addresses
(const STEPS (array (fn (u8) void) 3) (make (array (fn (u8) void) 3) walk fall hop))

(fn spawn ((kind u8) (x u8) (y u8)) void
    (set (at things thing_count) (make Thing :x x :y y :step (at STEPS kind)))
    (set thing_count (+ thing_count 1)))

(fn draw ((i u8) (c u8)) void
    (text40.move (at things i x) (at things i y))
    (text40.put c))

(fn main () void
    (text40.on)
    (spawn 0 0 2)
    (spawn 1 5 0)
    (spawn 2 10 8)
    (spawn 0 20 12)
    (for frame u8 0 120
        (for i u8 0 thing_count
            (draw i ' ')
            (let step (fn (u8) void) (at things i step))
            (step i)
            (draw i '*'))
        (timing.frame)))
