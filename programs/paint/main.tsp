; Paint on the hi-res screen with the mouse: hold the button to draw, any key clears, Esc
; quits. Needs an AppleMouse card (tisple run puts one in slot 4) or a IIc; without one it
; quits at once.

(import apple2 a2)
(import hgr)
(import keys)
(import mouse)

; the cursor: a dot with four arms, xor-drawn so drawing it again takes it away
(fn cursor ((x u16) (y u8)) void
    (hgr.flip x y)
    (if (>= x 2)
        (hgr.flip (- x 2) y))
    (if (< x 278)
        (hgr.flip (+ x 2) y))
    (if (>= y 2)
        (hgr.flip x (- y 2)))
    (if (< y 190)
        (hgr.flip x (+ y 2))))

(fn main () void
    (mouse.init)
    (if (not mouse.found)
        (return))
    (hgr.clear)
    (hgr.show)
    (mouse.read)
    (let x u16 mouse.x)
    (let y u8 mouse.y)
    (let down bool mouse.button)
    (cursor x y)
    (while true
        (let key u8 (keys.read))
        (if (= key a2.KEY_ESC)
            (break))
        (if (!= key 0)
            (do
                (hgr.clear)
                (cursor x y)))
        (mouse.read)
        (if (or (or (!= mouse.x x) (!= mouse.y y)) (!= mouse.button down))
            (do
                (cursor x y)
                ; a stroke joins the points it read, so a fast move leaves no gaps
                (if (and mouse.button down)
                    (hgr.line x y mouse.x mouse.y))
                (if (and mouse.button (not down))
                    (hgr.plot mouse.x mouse.y))
                (set x mouse.x)
                (set y mouse.y)
                (set down mouse.button)
                (cursor x y))))
    (io_read u8 a2.TXTSET))
