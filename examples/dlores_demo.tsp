; double lo-res, 80 x 40 blocks in 16 colours over four lines of 80-column text, with a
; MouseText frame round the text. ESC leaves.
;   tisple run examples/dlores_demo.tsp

(import apple2 a2)
(import dlores)
(import mousetext)
(import text80)
(import timing)

; colour bars down the screen, a diagonal stripe moving through them
(fn draw ((phase u8)) void
    (for y u8 0 40
        (for x u8 0 80
            (let c u8 (& (+ (>> x 2) (>> y 3)) 15))
            (if (= (& (+ x (+ y phase)) 15) 0)
                (set c dlores.WHITE))
            (dlores.plot x y c))))

(fn main () void
    (dlores.on true)
    (text80.window 20)
    (text80.clear)
    (mousetext.on)
    (mousetext.bar80 20)
    (text80.move 1 21)
    (text80.put_code mousetext.OPEN_APPLE)
    (text80.print " DOUBLE LO-RES: 80 X 48 BLOCKS, 16 COLOURS, THE EVEN COLUMNS IN AUX MEMORY ")
    (text80.put_code mousetext.CHECK)
    (text80.move 1 22)
    (text80.put_code mousetext.HOURGLASS)
    (text80.print " MIXED MODE: FOUR LINES OF 80-COLUMN TEXT UNDER IT. ESC LEAVES")
    (let phase u8 0)
    (while (!= (io_read u8 a2.KBD) (| a2.KEY_ESC 0x80))
        (draw phase)
        (set phase (+ phase 1)))
    (io_read u8 a2.KBDSTRB)
    (dlores.off))
