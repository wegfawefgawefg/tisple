; BYTE's sieve (Jim Gilbreath, BYTE, September 1981): 10 passes over 8191 flags,
; counting 1899 primes; flags are the race's buffer, the count goes to checksum

(const SIZE u16 8190)

(let flags (array u8 8191) :address 0x2000)
(let checksum u16 :address 0x5F00)

(fn main () void
    (let count u16 0)
    (for iter u8 0 10
        (set count 0)
        (for i u16 0 (+ SIZE 1)
            (set (at flags i) 1))
        (for i u16 0 (+ SIZE 1)
            (if (!= (at flags i) 0)
                (do
                    (let prime u16 (+ (+ i i) 3))
                    (let k u16 (+ i prime))
                    (while (<= k SIZE)
                        (set (at flags k) 0)
                        (set k (+ k prime)))
                    (set count (+ count 1))))))
    (set checksum count))
