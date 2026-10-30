; Pathfinding as a game does it: breadth-first search on a 64 x 48 grid of floor and
; wall (walls all round the edge, so no bounds checks), a queue of cell numbers, each
; cell's direction back toward the start in came_from; then the path is walked back from
; the goal and marked in the grid. Six searches, each over the grid the last one marked;
; lengths gets each path's length (65535 for none), checksum folds them and the grid.

(const W u16 64)
(const CELLS u16 3072)
(const SEARCHES u8 6)
(const WALL u8 1)
(const PATH u8 2)
(const UNSEEN u8 255)
(const START u8 4)

; north, east, south, west, as amounts to add (wrapping)
(const STEP (array u16 4) (make (array u16 4) 65472 1 64 65535))

(let grid (array u8 3072) :address 0x2000)
(let came_from (array u8 3072) :address 0x3000)
(let queue (array u16 3072) :address 0x4000)
(let starts (array u16 6) :address 0x5E00)
(let goals (array u16 6) :address 0x5E20)
(let lengths (array u16 6) :address 0x5E40)
(let checksum u16 :address 0x5F00)

(let tail u16 0)

(fn visit ((next u16) (d u8)) void
    (if (and (!= (at grid next) WALL) (= (at came_from next) UNSEEN))
        (do
            (set (at came_from next) d)
            (set (at queue tail) next)
            (set tail (+ tail 1)))))

(fn find_path ((start u16) (goal u16)) u16
    (for i u16 0 CELLS
        (set (at came_from i) UNSEEN))
    (set (at came_from start) START)
    (let head u16 0)
    (set tail 0)
    (set (at queue tail) start)
    (set tail (+ tail 1))
    (while (!= head tail)
        (let cell u16 (at queue head))
        (set head (+ head 1))
        (if (= cell goal)
            (break))
        (visit (- cell W) 0)
        (visit (+ cell 1) 1)
        (visit (+ cell W) 2)
        (visit (- cell 1) 3))
    (if (= (at came_from goal) UNSEEN)
        (return 65535))
    (let len u16 0)
    (let cell u16 goal)
    (while (!= cell start)
        (set (at grid cell) PATH)
        (set cell (- cell (at STEP (at came_from cell))))
        (set len (+ len 1)))
    len)

(fn main () void
    (let sum_a u16 0)
    (let sum_b u16 0)
    (for s u8 0 SEARCHES
        (set (at lengths s) (find_path (at starts s) (at goals s)))
        (set sum_a (+ sum_a (at lengths s)))
        (set sum_b (+ sum_b sum_a)))
    (for i u16 0 CELLS
        (set sum_a (+ sum_a (as u16 (at grid i))))
        (set sum_b (+ sum_b sum_a)))
    (set checksum (^ sum_a sum_b)))
