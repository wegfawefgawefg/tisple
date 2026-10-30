; disk tables read in while the program runs, on a disk tisple build --disk made. A table
; placed :on disk lives on the disk, not in memory; its name holds a record of where:
;   (tables.packed_bytes FOREST (...) :on disk)            ; packed, for lib/unpack
;   (const MAP (array u8 2048) (make ...) :on disk)        ; or as it is
; then (import disk) and
;   (disk.read dst (addr_of (at FOREST 0)) disk.AUX)      the table's bytes to aux memory at dst
;   (disk.read dst (addr_of (at MAP 0)) disk.MAIN)        or to main memory
;   (disk.read_with dst table disk.AUX step)              step, a (fn () void), after each
;                                                          sector but the last
; each gives disk.DONE (0) when every byte is in, or an error: disk.NO_CARD, NO_DISK,
; BAD_SECTOR, BAD_TRACK. Exactly the table's bytes are written. About 9 KB a second; step has
; about 9,000 cycles a sector before the read slows (docs/disks.md, Disk tables). Interrupts
; keep running, held off while a sector is read (about 12,000 cycles at a time). Not from
; step itself, and not with 80STORE on unless dst is clear of the screens.

(import auxmem)

; the reader, our own 6502 code (src/disk/disk_reader.cpp). tisple build --disk writes it into
; reader's bytes at boot, with the slot the disk booted from, so it costs the program no bytes
; of its own; it runs there, with its bounce page and tables in work. It takes its arguments
; in auxmem.from, auxmem.to and X, reads them at BEGIN, and keeps no zero page of its own. A
; build without a disk has no reader: every read gives NO_CARD.
(const BEGIN u16 0x1700)
(const NEXT u16 0x1703)
(let work (array u8 768) :address 0x1400 :reserve)
(let reader (array u8 1280) :address 0x1700 :reserve)

(const MAIN u8 0)
(const AUX u8 1)

(const DONE u8 0)
(const MORE u8 1)
(const NO_CARD u8 2)    ; no disk ii card in the slot the disk booted from
(const NO_DISK u8 3)    ; no address field for three turns: no disk, or not ours
(const BAD_SECTOR u8 4) ; a wanted sector did not read in four turns
(const BAD_TRACK u8 5)  ; the head did not reach the track in five seeks

; motor on and the table's record read; MORE, or DONE for nothing to read, or NO_CARD (none
; without the reader: its first byte is the JMP to BEGIN's code)
(fn start ((dst (ptr u8)) (table (ptr u8)) (bank u8)) u8
    (if (!= (load (as (ptr u8) BEGIN)) 0x4C)
        (return NO_CARD))
    (set auxmem.from table)
    (set auxmem.to dst)
    (jsr BEGIN (x bank)))

(fn read ((dst (ptr u8)) (table (ptr u8)) (bank u8)) u8
    (let status u8 (start dst table bank))
    (while (= status MORE)
        (set status (jsr NEXT)))
    status)

(fn read_with ((dst (ptr u8)) (table (ptr u8)) (bank u8) (step (fn () void))) u8
    (let status u8 (start dst table bank))
    (if (= status MORE)
        (set status (jsr NEXT)))
    (while (= status MORE)
        (step)
        (set status (jsr NEXT)))
    status)
