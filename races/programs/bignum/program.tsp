; Decimal arithmetic as games and calculators do it. A score is 8 decimal digits, most
; significant first; each award (a u16) is split into digits by subtracting powers of
; ten and added digit by digit with a carry. 8 games of 50 awards; after each game the
; score goes into the high-score table (8 scores, best first) if it beats one. Then a
; bignum: 100! built in little-endian bytes by multiplying by 2..100, and printed in
; decimal into dec by dividing it by 10 again and again. The checksum folds the table,
; the last score and the digits.

(const DIGITS u8 8)
(const GAMES u8 8)
(const AWARDS u8 50)
(const HIGH u8 8)
(const BIG u8 80)

(const POWERS (array u16 4) (make (array u16 4) 10000 1000 100 10))

(let points (array u16 400) :address 0x2000)
(let high (array u8 64) :address 0x3100)
(let dec (array u8 200) :address 0x3000)
(let checksum u16 :address 0x5F00)

(let score (array u8 8))
(let add (array u8 5))
(let big (array u8 80))
(let big_len u8 0)
(let dec_len u8 0)

(fn add_points ((points u16)) void
    (let left u16 points)
    (for k u8 0 4
        (set (at add k) 0)
        (while (>= left (at POWERS k))
            (set left (- left (at POWERS k)))
            (set (at add k) (+ (at add k) 1))))
    (set (at add 4) (as u8 left))
    (let carry u8 0)
    (for i u8 0 DIGITS
        (let digit u8 (- (- DIGITS 1) i))
        (let v u8 (+ (at score digit) carry))
        (if (< i 5)
            (set v (+ v (at add (- 4 i)))))
        (set carry 0)
        (if (>= v 10)
            (do
                (set v (- v 10))
                (set carry 1)))
        (set (at score digit) v)))

; is the score above high-score entry e
(fn beats ((e u8)) bool
    (let h u8 (* e DIGITS))
    (for i u8 0 DIGITS
        (if (!= (at score i) (at high (+ h i)))
            (return (> (at score i) (at high (+ h i))))))
    false)

(fn enter_high_score () void
    (let e u8 0)
    (while (and (< e HIGH) (not (beats e)))
        (set e (+ e 1)))
    (if (= e HIGH)
        (return))
    (let i u8 (- (* HIGH DIGITS) 1))
    (while (>= i (* (+ e 1) DIGITS))
        (set (at high i) (at high (- i DIGITS)))
        (set i (- i 1)))
    (for k u8 0 DIGITS
        (set (at high (+ (* e DIGITS) k)) (at score k))))

(fn factorial ((n u8)) void
    (set (at big 0) 1)
    (set big_len 1)
    (for m u8 2 (+ n 1)
        (let carry u16 0)
        (for i u8 0 big_len
            (let v u16 (+ (* (as u16 (at big i)) (as u16 m)) carry))
            (set (at big i) (as u8 v))
            (set carry (>> v 8)))
        (while (!= carry 0)
            (set (at big big_len) (as u8 carry))
            (set big_len (+ big_len 1))
            (set carry (>> carry 8)))))

; the digits come out last first, so they are turned round at the end
(fn print_big () void
    (set dec_len 0)
    (while (> big_len 0)
        (let rem u16 0)
        (let i u8 big_len)
        (while (!= i 0)
            (set i (- i 1))
            (let v u16 (| (<< rem 8) (as u16 (at big i))))
            (set (at big i) (as u8 (/ v 10)))
            (set rem (% v 10)))
        (set (at dec dec_len) (+ '0' (as u8 rem)))
        (set dec_len (+ dec_len 1))
        (while (and (> big_len 0) (= (at big (- big_len 1)) 0))
            (set big_len (- big_len 1))))
    (for i u8 0 (/ dec_len 2)
        (let t u8 (at dec i))
        (set (at dec i) (at dec (- (- dec_len 1) i)))
        (set (at dec (- (- dec_len 1) i)) t)))

(fn main () void
    (for i u8 0 (* HIGH DIGITS)
        (set (at high i) 0))
    (for g u8 0 GAMES
        (for i u8 0 DIGITS
            (set (at score i) 0))
        (for k u8 0 AWARDS
            (add_points (at points (+ (* (as u16 g) (as u16 AWARDS)) (as u16 k)))))
        (enter_high_score))
    (factorial 100)
    (print_big)
    (let sum_a u16 0)
    (let sum_b u16 0)
    (for i u8 0 (* HIGH DIGITS)
        (set sum_a (+ sum_a (as u16 (at high i))))
        (set sum_b (+ sum_b sum_a)))
    (for i u8 0 DIGITS
        (set sum_a (+ sum_a (as u16 (at score i))))
        (set sum_b (+ sum_b sum_a)))
    (for i u8 0 dec_len
        (set sum_a (+ sum_a (as u16 (at dec i))))
        (set sum_b (+ sum_b sum_a)))
    (set sum_a (+ sum_a (as u16 dec_len)))
    (set sum_b (+ sum_b sum_a))
    (set checksum (^ sum_a sum_b)))
