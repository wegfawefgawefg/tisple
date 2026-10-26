; sieve of Eratosthenes over 8192 flags; result is the number of primes below 8192

(let flags (array u8 8192) :address 0x2000)
(let result u16 :address 0x5F00)

(fn main () void
    (let count u16 0)
    (for i u16 0 8192
        (set (at flags i) 1))
    (for i u16 2 8192
        (if (!= (at flags i) 0)
            (do
                (set count (+ count 1))
                (let k u16 (+ i i))
                (while (< k 8192)
                    (set (at flags k) 0)
                    (set k (+ k i))))))
    (set result count))
