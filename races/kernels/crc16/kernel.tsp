; CRC-16-CCITT (polynomial $1021, start $FFFF), bit by bit, over 1 KB

(let data (array u8 1024) :address 0x4000)
(let result u16 :address 0x5F00)

(fn main () void
    (let crc u16 0xFFFF)
    (for i u16 0 1024
        (set crc (^ crc (<< (as u16 (at data i)) 8)))
        (for k u8 0 8
            (if (!= (& crc 0x8000) 0)
                (set crc (^ (<< crc 1) 0x1021))
                (set crc (<< crc 1)))))
    (set result crc))
