; saves on the disk the program booted from: high scores, a saved game, settings, written
; back to sectors the build sets aside. (import save) then, at the top level,
;   (save.area GAME 300)                 room on the disk for a save of up to 300 bytes
; and
;   (save.write (addr_of (at GAME 0)) (addr_of (at state 0)) 300)  the bytes to the disk
;   (save.read (addr_of (at GAME 0)) (addr_of (at state 0)) 300)   and back
; each gives save.OK (0) or what went wrong: save.EMPTY (read: nothing saved yet, or no copy
; that checks), save.PROTECTED (the disk's write protect is on), save.TOO_BIG (more than the
; area holds), or lib/disk's NO_CARD, NO_DISK, BAD_SECTOR, BAD_TRACK.
;
; A save area is a table :on disk (docs/disks.md, Disk tables): two copies, each a header
; sector and the data's sectors, on the last tracks, far from the loader and the program. A
; write puts the data in the older copy, then its header (a generation number, the length,
; a checksum of the data); a read takes the newest copy whose header and data check. So a
; write cut short, by a power cut or a reset, leaves the copy before it to read. Our own
; writer (src/disk/disk_writer.cpp) refuses any track below the lowest table, so nothing it
; does can reach the tracks the disk boots from. Builds without --disk have no writer and no
; tables: every call gives NO_CARD.
;
; A write of n bytes writes (n + 255) / 256 + 1 sectors and reads the two headers first: well
; under half a second for a page. Interrupts are off for about 12,000 cycles a sector, as for
; lib/disk's reads. The bytes come from main memory, a page at a time: the last sector also
; takes the bytes after the save's end, up to its page's end (they are never read back).
; read's dst is left with an unchecked copy's bytes when it gives EMPTY. Up to 4096 bytes.

(import auxmem)
(import disk)

(const OK u8 0)
(const NO_CARD u8 2)
(const NO_DISK u8 3)
(const BAD_SECTOR u8 4)
(const BAD_TRACK u8 5)
(const PROTECTED u8 6) ; the write protect is on
(const REFUSED u8 7)   ; the writer would not write there: not a save area
(const EMPTY u8 8)     ; nothing saved, or no copy that checks
(const TOO_BIG u8 9)   ; more bytes than the area holds

(const MORE u8 1)
(const MAX_PAGES u8 16)

; the writer, our own 6502 code (src/disk/disk_writer.cpp): tisple build --disk writes it here
; for the disk, so it costs the program no bytes of its own. Its entries take the record in
; auxmem.from and the bytes in auxmem.to, as the reader's
(const BEGIN u16 0x0E00)
(const NEXT u16 0x0E03)
(let writer (array u8 1536) :address 0x0E00 :reserve)

; room for a save of up to bytes: two copies of a header sector and the data's sectors, every
; third sector round a track (time to work out the next one's nibbles)
(macro area (name bytes)
    (if (> bytes 4096)
        (error "a save area holds up to 4096 bytes, not " bytes))
    (let n (* 2 (* 256 (+ 1 (/ (+ bytes 255) 256)))))
    `(const ,name (array u8 ,n)
        (make (array u8 ,n)
            ,@(map
                (fn (i)
                    0)
                (range 0 n)))
        :on disk
        :interleave 3))

; a header: "TSAV", the generation, the length, the data's two check bytes, a check of these
(const HEAD_SIZE u8 16)
(let head (array u8 16))
(let other (array u8 16))

; a record for some of an area's pieces: its length, the area's first track, the entries
(let part (array u8 24))

(fn pieces_of ((count u16)) u8
    (as u8 (>> (+ count 255) 8)))

; the area's pieces copy (0 or 1) holds from first, n of them, count bytes long
(fn make_part ((area (ptr u8)) (first u8) (n u8) (count u16)) (ptr u8)
    (set (at part 0) (as u8 count))
    (set (at part 1) (as u8 (>> count 8)))
    (set (at part 2) (load (+ area 2)))
    (for i u8 0 n
        (set (at part (+ 3 i)) (load (+ area (+ 3 (+ first i))))))
    (addr_of (at part 0)))

; the area's length in pieces for one copy
(fn copy_pieces ((area (ptr u8))) u8
    (let length u16 (| (as u16 (load area)) (<< (as u16 (load (+ area 1))) 8)))
    (as u8 (>> length 9)))

(fn header_part ((area (ptr u8)) (copy u8)) (ptr u8)
    (make_part area (* copy (copy_pieces area)) 1 (as u16 HEAD_SIZE)))

(fn data_part ((area (ptr u8)) (copy u8) (count u16)) (ptr u8)
    (make_part area (+ (* copy (copy_pieces area)) 1) (pieces_of count) count))

; the data's two check bytes: a running sum, and a sum of the sums (Fletcher's, mod 256)
(let sum_a u8 0)
(let sum_b u8 0)

(fn check_bytes ((p (ptr u8)) (count u16)) void
    (set sum_a 0)
    (set sum_b 0)
    (for i u16 0 count
        (set sum_a (+ sum_a (load (+ p i))))
        (set sum_b (+ sum_b sum_a))))

(fn head_check ((h (ptr u8))) u8
    (let x u8 0x5A)
    (for i u8 0 10
        (set x (^ x (load (+ h i)))))
    x)

(fn head_good ((h (ptr u8)) (count u16)) bool
    (and (= (load h) 'T')
        (and (= (load (+ h 1)) 'S')
            (and (= (load (+ h 2)) 'A')
                (and (= (load (+ h 3)) 'V')
                    (and (= (load (+ h 10)) (head_check h))
                        (and (= (load (+ h 6)) (as u8 count))
                            (= (load (+ h 7)) (as u8 (>> count 8))))))))))

; copy's header to h; false when it is not there, or does not check (a sector that does not
; read is a copy that is not there); a disk error in status
(let status u8 0)

(fn read_head ((area (ptr u8)) (copy u8) (h (ptr u8)) (count u16)) bool
    (for i u8 0 HEAD_SIZE
        (store (+ h i) 0))
    (let got u8 (disk.read h (header_part area copy) disk.MAIN))
    (if (= got disk.BAD_SECTOR)
        (return false))
    (if (!= got disk.DONE)
        (do
            (set status got)
            (return false)))
    (head_good h count))

; whether generation a is newer than b, round the wrap
(fn newer ((a u8) (b u8)) bool
    (let d u8 (- a b))
    (and (!= d 0) (< d 0x80)))

; the writer through its record and bytes
(fn write_part ((part_record (ptr u8)) (src (ptr u8))) u8
    (if (!= (load (as (ptr u8) BEGIN)) 0x4C)
        (return NO_CARD))
    (set auxmem.from part_record)
    (set auxmem.to src)
    (let got u8 (jsr BEGIN))
    (while (= got MORE)
        (set got (jsr NEXT)))
    got)

(fn write ((area (ptr u8)) (src (ptr u8)) (count u16)) u8
    (if (or (= count 0) (> (pieces_of count) (- (copy_pieces area) 1)))
        (return TOO_BIG))
    (set status OK)
    (let has_a bool (read_head area 0 (addr_of (at head 0)) count))
    (let gen_a u8 (at head 5))
    (let has_b bool (read_head area 1 (addr_of (at other 0)) count))
    (let gen_b u8 (at other 5))
    (if (!= status OK)
        (return status))
    ; the copy to write: one that is not there, else the older
    (let copy u8 0)
    (let gen u8 1)
    (if (and has_a (not has_b))
        (do
            (set copy 1)
            (set gen (+ gen_a 1))))
    (if (and has_b (not has_a))
        (set gen (+ gen_b 1)))
    (if (and has_a has_b)
        (if (newer gen_a gen_b)
            (do
                (set copy 1)
                (set gen (+ gen_a 1)))
            (set gen (+ gen_b 1))))
    (let got u8 (write_part (data_part area copy count) src))
    (if (!= got disk.DONE)
        (return got))
    (check_bytes src count)
    (set (at head 0) 'T')
    (set (at head 1) 'S')
    (set (at head 2) 'A')
    (set (at head 3) 'V')
    (set (at head 4) 1)
    (set (at head 5) gen)
    (set (at head 6) (as u8 count))
    (set (at head 7) (as u8 (>> count 8)))
    (set (at head 8) sum_a)
    (set (at head 9) sum_b)
    (set (at head 10) (head_check (addr_of (at head 0))))
    (write_part (header_part area copy) (addr_of (at head 0))))

; copy's data to dst, checked against its header h
(fn read_copy ((area (ptr u8)) (copy u8) (h (ptr u8)) (dst (ptr u8)) (count u16)) bool
    (let got u8 (disk.read dst (data_part area copy count) disk.MAIN))
    (if (= got disk.BAD_SECTOR)
        (return false))
    (if (!= got disk.DONE)
        (do
            (set status got)
            (return false)))
    (check_bytes dst count)
    (and (= sum_a (load (+ h 8))) (= sum_b (load (+ h 9)))))

(fn read ((area (ptr u8)) (dst (ptr u8)) (count u16)) u8
    (if (or (= count 0) (> (pieces_of count) (- (copy_pieces area) 1)))
        (return TOO_BIG))
    (set status OK)
    (let has_a bool (read_head area 0 (addr_of (at head 0)) count))
    (let has_b bool (read_head area 1 (addr_of (at other 0)) count))
    (if (!= status OK)
        (return status))
    (let first u8 0)
    (if (or (not has_a) (and has_b (newer (at other 5) (at head 5))))
        (set first 1))
    (for k u8 0 2
        (let copy u8 (^ first k))
        (let there bool (if (= copy 0)
            has_a
            has_b))
        (if there
            (do
                (let h (ptr u8) (if (= copy 0)
                    (addr_of (at head 0))
                    (addr_of (at other 0))))
                (if (read_copy area copy h dst count)
                    (return OK))
                (if (!= status OK)
                    (return status)))))
    EMPTY)
