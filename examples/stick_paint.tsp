; the joystick on lo-res with four lines of text: a block follows the stick, button 0 paints,
; button 1 clears, and the text window shows the values. ESC leaves.
;   tisple run examples/stick_paint.tsp            (a game controller in the window, or Alt keys)
;   tisple run examples/stick_paint.tsp --frames 120 --joystick "0:20,200,0;60f:250,30,1"

(import apple2 a2)
(import joystick)
(import lores)
(import text40)
(import timing)

(let x u8 20)
(let y u8 20)
(let under u8 0) ; the colour under the cursor

(fn put_number ((n u8)) void
    (text40.put (+ '0' (/ n 100)))
    (text40.put (+ '0' (% (/ n 10) 10)))
    (text40.put (+ '0' (% n 10))))

(fn show_values () void
    (text40.move 0 21)
    (text40.print "X ")
    (put_number joystick.x)
    (text40.print "  Y ")
    (put_number joystick.y)
    (text40.put_str (if joystick.button0
        "  PAINT"
        "       "))
    (text40.put_str (if joystick.button1
        "  CLEAR"
        "       ")))

(fn main () void
    (lores.mixed)
    (lores.clear)
    (text40.window 20)
    (text40.clear)
    (text40.print "STICK MOVES, BUTTON 0 PAINTS, 1 CLEARS")
    (while (!= (io_read u8 a2.KBD) (| a2.KEY_ESC 0x80))
        (timing.frame)
        (joystick.read)
        (lores.plot x y under)
        ; 0-255 across 40 blocks and down 40
        (set x (as u8 (/ (* (as u16 joystick.x) 40) 256)))
        (set y (as u8 (/ (* (as u16 joystick.y) 40) 256)))
        (if joystick.button1
            (lores.clear))
        (if joystick.button0
            (lores.plot x y lores.ORANGE))
        (set under (lores.get x y))
        (lores.plot x y lores.WHITE)
        (show_values)))
