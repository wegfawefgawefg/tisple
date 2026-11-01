; what machine this is, on the 40-column screen: lib/machine's answers, then ESC
;   tisple run examples/machine_info.tsp                 the built-in IIe (an enhanced IIe)
;   tisple run examples/machine_info.tsp --model iic --mockingboard 5

(import apple2 a2)
(import machine)
(import text40)

(fn put_number ((n u8)) void
    (if (>= n 100)
        (text40.put (+ '0' (/ n 100))))
    (if (>= n 10)
        (text40.put (+ '0' (% (/ n 10) 10))))
    (text40.put (+ '0' (% n 10))))

(fn model_name () (ptr u8)
    (match machine.model
        (machine.II "APPLE ][")
        (machine.II_PLUS "APPLE ][+")
        (machine.IIE "APPLE //E")
        (machine.IIE_ENHANCED "ENHANCED APPLE //E")
        (machine.IIC "APPLE //C")
        (machine.IIC_PLUS "APPLE //C PLUS")
        (machine.IIGS "APPLE IIGS (AS A //E)")
        (_ "UNKNOWN")))

(fn card_name ((kind u8)) (ptr u8)
    (match kind
        (machine.DISK2 "DISK II")
        (machine.STORAGE "HARD DISK / FLASH")
        (machine.MOUSE "MOUSE")
        (machine.MOCKINGBOARD "MOCKINGBOARD")
        (machine.SERIAL "SERIAL")
        (machine.COL80 "80 COLUMNS")
        (machine.OTHER "SOME CARD")
        (_ "-")))

(fn yes_no ((b bool)) (ptr u8)
    (if b
        "YES"
        "NO"))

(fn main () void
    (machine.detect)
    (text40.on)
    (text40.print "MACHINE:   ")
    (text40.put_str (model_name))
    (text40.put a2.KEY_RETURN)
    (text40.print "MEMORY:    ")
    (put_number machine.kb)
    (text40.print "K")
    (text40.put a2.KEY_RETURN)
    (text40.print "65C02:     ")
    (text40.put_str (yes_no machine.cmos))
    (text40.put a2.KEY_RETURN)
    (text40.print "MOUSETEXT: ")
    (text40.put_str (yes_no machine.mousetext))
    (text40.put a2.KEY_RETURN)
    (text40.put a2.KEY_RETURN)
    (for n u8 1 8
        (text40.print "SLOT ")
        (put_number n)
        (text40.print ": ")
        (text40.put_str (card_name (machine.card n)))
        (text40.put a2.KEY_RETURN))
    (text40.move 0 22)
    (text40.print "ESC TO LEAVE")
    (while (!= (io_read u8 a2.KBD) (| a2.KEY_ESC 0x80))))
