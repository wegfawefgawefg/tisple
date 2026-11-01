; a save on the boot disk: how many times this disk has booted, and the best of a little
; reaction game, kept between runs. Build a disk and boot it again and again:
;   tisple run examples/save_count.tsp --disk build/save_count.dsk --write-back
;   tisple machine --disk build/save_count.dsk --write-back --window

(import apple2 a2)
(import save)
(import text40)

(struct Record ((boots u16) (best u16)))

(save.area RECORD 4)

(let record Record)
(let bytes (array u8 4))
(let digits (array u8 5))

(fn put_number ((n u16)) void
    (let count u8 0)
    (while true
        (set (at digits count) (as u8 (% n 10)))
        (set count (+ count 1))
        (set n (/ n 10))
        (if (= n 0)
            (break)))
    (while (> count 0)
        (set count (- count 1))
        (text40.put (+ '0' (at digits count)))))

(fn put_status ((status u8)) void
    (match status
        (save.OK (text40.print "OK"))
        (save.EMPTY (text40.print "NOTHING SAVED YET"))
        (save.PROTECTED (text40.print "THE DISK IS WRITE PROTECTED"))
        (save.NO_CARD (text40.print "NO DISK TO SAVE ON (BUILD WITH --DISK)"))
        (_
            (text40.print "DISK ERROR ")
            (put_number (as u16 status)))))

(fn load_record () u8
    (let status u8 (save.read (addr_of (at RECORD 0)) (addr_of (at bytes 0)) 4))
    (if (= status save.OK)
        (do
            (set record.boots (| (as u16 (at bytes 0)) (<< (as u16 (at bytes 1)) 8)))
            (set record.best (| (as u16 (at bytes 2)) (<< (as u16 (at bytes 3)) 8)))))
    (if (!= status save.OK)
        (do
            (set record.boots 0)
            (set record.best 0xFFFF)))
    status)

(fn save_record () u8
    (set (at bytes 0) (as u8 record.boots))
    (set (at bytes 1) (as u8 (>> record.boots 8)))
    (set (at bytes 2) (as u8 record.best))
    (set (at bytes 3) (as u8 (>> record.best 8)))
    (save.write (addr_of (at RECORD 0)) (addr_of (at bytes 0)) 4))

; frames from GO until a key, about 60 a second
(fn react () u16
    (text40.move 0 10)
    (text40.print "WAIT FOR IT...")
    (for i u8 0 90
        (while (< (io_read u8 a2.RDVBLBAR) 0x80))
        (while (>= (io_read u8 a2.RDVBLBAR) 0x80)))
    (io_read u8 a2.KBDSTRB)
    (text40.move 0 10)
    (text40.print "GO! PRESS A KEY  ")
    (let frames u16 0)
    (while (< (io_read u8 a2.KBD) 0x80)
        (while (< (io_read u8 a2.RDVBLBAR) 0x80))
        (while (>= (io_read u8 a2.RDVBLBAR) 0x80))
        (set frames (+ frames 1)))
    (io_read u8 a2.KBDSTRB)
    frames)

(fn main () void
    (text40.on)
    (text40.print "LOAD: ")
    (put_status (load_record))
    (set record.boots (+ record.boots 1))
    (text40.move 0 2)
    (text40.print "THIS DISK HAS BOOTED ")
    (put_number record.boots)
    (text40.print " TIMES")
    (text40.move 0 3)
    (text40.print "BEST: ")
    (if (= record.best 0xFFFF)
        (text40.print "-")
        (do
            (put_number record.best)
            (text40.print " FRAMES")))
    (text40.move 0 5)
    (text40.print "SAVE: ")
    (put_status (save_record))
    (text40.move 0 7)
    (text40.print "PRESS A KEY TO PLAY, ESC TO LEAVE")
    (while true
        (while (< (io_read u8 a2.KBD) 0x80))
        (let key u8 (& (io_read u8 a2.KBD) 0x7F))
        (io_read u8 a2.KBDSTRB)
        (if (= key a2.KEY_ESC)
            (return))
        (let frames u16 (react))
        (text40.move 0 12)
        (text40.print "YOU TOOK ")
        (put_number frames)
        (text40.print " FRAMES     ")
        (if (< frames record.best)
            (do
                (set record.best frames)
                (text40.move 0 13)
                (text40.print "A NEW BEST! SAVE: ")
                (put_status (save_record))))))
