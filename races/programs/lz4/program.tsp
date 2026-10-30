; LZ4 block decompression (the format of LZ4's block API: a token of literal and match
; lengths, 255-byte length extensions, little-endian offsets) of packed into out, 8 KB of
; text and a tile map, as a game unpacks a level; then a checksum of what came out.
; u16 indexes into the two buffers, as Tisple arrays are reached.

(const SIZE u16 8192)

(let out (array u8 8192) :address 0x2000)
(let packed (array u8 7680) :address 0x4000)
(let checksum u16 :address 0x5F00)
(let packed_len u16 :address 0x5F02)

(let src u16 0)

(fn read_byte () u8
    (let b u8 (at packed src))
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
    (let o u16 0)
    (set src 0)
    (while true
        (let token u8 (read_byte))
        (let len u16 (extend_length (as u16 (>> token 4))))
        (while (!= len 0)
            (set (at out o) (at packed src))
            (set o (+ o 1))
            (set src (+ src 1))
            (set len (- len 1)))
        (if (>= src src_len)
            (break))
        (let offset u16 (as u16 (read_byte)))
        (set offset (| offset (<< (as u16 (read_byte)) 8)))
        (let m u16 (- o offset))
        (set len (+ (extend_length (as u16 (& token 15))) 4))
        (while (!= len 0)
            (set (at out o) (at out m))
            (set o (+ o 1))
            (set m (+ m 1))
            (set len (- len 1))))
    o)

(fn main () void
    (let n u16 (lz4_decode packed_len))
    (let sum_a u16 0)
    (let sum_b u16 0)
    (for i u16 0 n
        (set sum_a (+ sum_a (as u16 (at out i))))
        (set sum_b (+ sum_b sum_a)))
    (set checksum (^ (^ sum_a sum_b) n)))
