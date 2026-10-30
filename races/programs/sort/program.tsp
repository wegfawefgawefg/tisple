; Sorting as a program does it. keys (1024 words) by quicksort with an explicit stack of
; ranges, leaving runs under CUTOFF for one insertion sort pass at the end; table (64
; records of score and id) by a stable insertion sort, high scores first; then the best
; 16 as a high-score table that 64 new scores are entered into. A checksum of all three
; goes to checksum.

(const KEYS u16 1024)
(const RECORDS u8 64)
(const TOP u8 16)
(const CUTOFF u16 8)
(const STACK u8 24)

(struct Score ((score u16) (id u8)))

(let keys (array u16 1024) :address 0x2000)
(let table (array Score 64) :address 0x2800)
(let news (array u16 64) :address 0x2900)
(let top (array Score 16) :address 0x2A00)
(let checksum u16 :address 0x5F00)

(let lo_stack (array u16 STACK))
(let hi_stack (array u16 STACK))
(let sp u8 0)

(fn swap_keys ((i u16) (j u16)) void
    (let t u16 (at keys i))
    (set (at keys i) (at keys j))
    (set (at keys j) t))

(fn push ((lo u16) (hi u16)) void
    (set (at lo_stack sp) lo)
    (set (at hi_stack sp) hi)
    (set sp (+ sp 1)))

; Hoare's partition around the median of the first, middle and last keys; every key in
; lo..split is at most every key in split+1..hi
(fn partition ((lo u16) (hi u16)) u16
    (let mid u16 (+ lo (>> (- hi lo) 1)))
    (if (< (at keys mid) (at keys lo)) (swap_keys mid lo))
    (if (< (at keys hi) (at keys lo)) (swap_keys hi lo))
    (if (< (at keys hi) (at keys mid)) (swap_keys hi mid))
    (let pivot u16 (at keys mid))
    (let i u16 lo)
    (let j u16 hi)
    (while true
        (while (< (at keys i) pivot)
            (set i (+ i 1)))
        (while (> (at keys j) pivot)
            (set j (- j 1)))
        (if (>= i j)
            (return j))
        (swap_keys i j)
        (set i (+ i 1))
        (set j (- j 1)))
    j)

(fn insertion_sort_keys ((n u16)) void
    (for i u16 1 n
        (let v u16 (at keys i))
        (let j u16 i)
        (while (and (> j 0) (> (at keys (- j 1)) v))
            (set (at keys j) (at keys (- j 1)))
            (set j (- j 1)))
        (set (at keys j) v)))

(fn quicksort_keys ((n u16)) void
    (set sp 0)
    (push 0 (- n 1))
    (while (!= sp 0)
        (set sp (- sp 1))
        (let lo u16 (at lo_stack sp))
        (let hi u16 (at hi_stack sp))
        (while (>= (- hi lo) CUTOFF)
            (let split u16 (partition lo hi))
            ; push the bigger side and go on with the smaller: the stack stays short
            (if (< (- split lo) (- hi split))
                (do
                    (push (+ split 1) hi)
                    (set hi split))
                (do
                    (push lo split)
                    (set lo (+ split 1))))))
    (insertion_sort_keys n))

; high scores first; equal scores keep their order
(fn sort_table () void
    (for i u8 1 RECORDS
        (let score u16 (at table i score))
        (let id u8 (at table i id))
        (let j u8 i)
        (while (and (> j 0) (< (at table (- j 1) score) score))
            (set (at table j score) (at table (- j 1) score))
            (set (at table j id) (at table (- j 1) id))
            (set j (- j 1)))
        (set (at table j score) score)
        (set (at table j id) id)))

; a new score goes in below every score at least as high, the last one drops off
(fn enter_score ((score u16) (id u8)) void
    (if (<= score (at top (- TOP 1) score))
        (return))
    (let pos u8 0)
    (while (>= (at top pos score) score)
        (set pos (+ pos 1)))
    (let i u8 (- TOP 1))
    (while (> i pos)
        (set (at top i score) (at top (- i 1) score))
        (set (at top i id) (at top (- i 1) id))
        (set i (- i 1)))
    (set (at top pos score) score)
    (set (at top pos id) id))

(let sum_a u16 0)
(let sum_b u16 0)

(fn fold ((v u16)) void
    (set sum_a (+ sum_a v))
    (set sum_b (+ sum_b sum_a)))

(fn main () void
    (quicksort_keys KEYS)
    (sort_table)
    (for k u8 0 TOP
        (set (at top k score) (at table k score))
        (set (at top k id) (at table k id)))
    (for k u8 0 RECORDS
        (enter_score (at news k) (+ 100 k)))
    (set sum_a 0)
    (set sum_b 0)
    (for i u16 0 KEYS
        (fold (at keys i)))
    (for k u8 0 RECORDS
        (fold (at table k score))
        (fold (as u16 (at table k id))))
    (for k u8 0 TOP
        (fold (at top k score))
        (fold (as u16 (at top k id))))
    (set checksum (^ sum_a sum_b)))
