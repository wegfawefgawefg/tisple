; program.tsp with pointers walking the buffers, as program.c does

(let out (array u8 8192) :address 0x2000)
(let packed (array u8 7680) :address 0x4000)
(let checksum u16 :address 0x5F00)
(let packed_len u16 :address 0x5F02)

(let src (ptr u8) (as (ptr u8) 0))

(fn read_byte () u8
    (let b u8 (load src))
    (set src (+ src 1))
    b)

; a length of 15 goes on in the bytes after it, while they are 255
(fn extend_length ((len u16)) u16
    (if (!= len 15)
        (return len))
    (while true
        (let b u8 (read_byte))
        (set len (+ len (as u16 b)))
        (if (!= b 255)
            (break)))
    len)

(fn lz4_decode ((src_len u16)) u16
    (set src (addr_of (at packed 0)))
    (let end (ptr u8) (+ src src_len))
    (let dst (ptr u8) (addr_of (at out 0)))
    (while true
        (let token u8 (read_byte))
        (let len u16 (extend_length (as u16 (>> token 4))))
        (while (!= len 0)
            (store dst (load src))
            (set dst (+ dst 1))
            (set src (+ src 1))
            (set len (- len 1)))
        (if (>= (as u16 src) (as u16 end))
            (break))
        (let offset u16 (as u16 (read_byte)))
        (set offset (| offset (<< (as u16 (read_byte)) 8)))
        (let m (ptr u8) (as (ptr u8) (- (as u16 dst) offset)))
        (set len (+ (extend_length (as u16 (& token 15))) 4))
        (while (!= len 0)
            (store dst (load m))
            (set dst (+ dst 1))
            (set m (+ m 1))
            (set len (- len 1))))
    (- (as u16 dst) (as u16 (addr_of (at out 0)))))

(fn main () void
    (let n u16 (lz4_decode packed_len))
    (let sum_a u16 0)
    (let sum_b u16 0)
    (for i u16 0 n
        (set sum_a (+ sum_a (as u16 (at out i))))
        (set sum_b (+ sum_b sum_a)))
    (set checksum (^ (^ sum_a sum_b) n)))
