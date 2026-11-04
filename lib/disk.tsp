; disk tables read in while the program runs, on a disk tisple build --disk made. A table
; placed :on disk lives on the disk, not in memory; its name holds a record of where:
;   (tables.packed_bytes FOREST (...) :on disk)            ; packed, for lib/unpack
;   (const MAP (array u8 2048) (make ...) :on disk)        ; or as it is
; then (import disk) and
;   (disk.read dst (addr_of (at FOREST 0)) disk.AUX)      the table's bytes to aux memory at dst
;   (disk.read dst (addr_of (at MAP 0)) disk.MAIN)        or to main memory
;   (disk.read_card (addr_of (at room 0)) (addr_of (at MAP 0)) disk.BANK1)  or to a let :in
;                                                          bank1, the language card's bank 1
;   (disk.read_card_with dst table bank step)              (read_card and read_card_with take
;                                                          any bank; read and read_with have
;                                                          no code for BANK1)
;   (disk.read_with dst table disk.AUX step)              step, a (fn () void), after each
;                                                          sector but the last
;   (const LEVELS (array u8 9000) (make ...) :on disk2)   ; on the second disk: from drive 2,
;   (disk.disk2_in 1)                                      or drive 1 once it is swapped in
; each gives disk.DONE (0) when every byte is in, or an error: disk.NO_CARD, NO_DISK,
; BAD_SECTOR, BAD_TRACK. Exactly the table's bytes are written. 8.9 KB a second; step has
; about 8,700 cycles a sector before the read slows (docs/disks.md, Disk tables). Interrupts
; keep running, held off while a sector is read (about 12,000 cycles at a time, from the field
; before's end on: up to about 15,300). Not from step itself, and not with 80STORE on unless dst
; is clear of the screens.

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
; the language card's bank 1 ($D000-$DFFF: a let :in bank1), for read_card and read_card_with:
; the reader reads as to main memory with bank 1 switched in for each sector, bank 2 back after
; (code that calls them is never in bank 1). A tick meanwhile runs with bank 2 in (the build's
; interrupt entry puts it in and back), and step between sectors with bank 2 as always
(const BANK1 u8 4)
(const CARD_BANK1 u16 0xC08B) ; a read: the card's bank 1 at $D000, ram read and written
(const CARD_BANK2 u16 0xC083) ; bank 2

(const DONE u8 0)
(const MORE u8 1)
(const NO_CARD u8 2)    ; no disk ii card in the slot the disk booted from
(const NO_DISK u8 3)    ; no address field for three turns: no disk, or not ours
(const BAD_SECTOR u8 4) ; a wanted sector did not read in four turns
(const BAD_TRACK u8 5)  ; the head did not reach the track in five seeks

; where the second disk is: drive 2 (2 here), or drive 1 (0 here) on a machine with one drive,
; once the program has asked for it to be put there (disk2_in). A table :on disk2 is read from
; that drive, one :on disk from drive 1
(let disk2_bit u8 2)

(fn disk2_in ((drive u8)) void
    (set disk2_bit (if (= drive 2)
        2
        0)))

; motor on and the table's record read; MORE, or DONE for nothing to read, or NO_CARD (none
; without the reader: its first byte is the JMP to BEGIN's code). A table :on disk2 has $80 in
; its record's third byte (a track is 34 at most): bit 1 of X asks the reader for drive 2
(fn start ((dst (ptr u8)) (table (ptr u8)) (bank u8)) u8
    (if (!= (load (as (ptr u8) BEGIN)) 0x4C)
        (return NO_CARD))
    (set auxmem.from table)
    (set auxmem.to dst)
    (jsr BEGIN (x (| bank (& (>> (load (+ table 2)) 6) disk2_bit)))))

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

; the next sector, into bank 1 for BANK1 (the reader's main memory mode: X bit 2 is not its)
(macro next_card (bank)
    `(if (= ,bank BANK1)
        (do
            (io_read u8 CARD_BANK1)
            (let status u8 (jsr NEXT))
            (io_read u8 CARD_BANK2)
            status)
        (jsr NEXT)))

; read and read_with with bank BANK1 too
(fn read_card ((dst (ptr u8)) (table (ptr u8)) (bank u8)) u8
    (let status u8 (start dst table bank))
    (while (= status MORE)
        (set status (next_card bank)))
    status)

(fn read_card_with ((dst (ptr u8)) (table (ptr u8)) (bank u8) (step (fn () void))) u8
    (let status u8 (start dst table bank))
    (if (= status MORE)
        (set status (next_card bank)))
    (while (= status MORE)
        (step)
        (set status (next_card bank)))
    status)
