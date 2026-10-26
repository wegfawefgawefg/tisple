; move a dot with the arrow keys; click the speaker on every step

(import gfx/hgr)
(import apple2 a2)

(enum Action (none left right up down))

(struct Dot ((x u16) (y u8)))

(let dot Dot (make Dot :x 140 :y 96))

(macro inc (place)
    `(set ,place (+ ,place 1)))

(macro dec (place)
    `(set ,place (- ,place 1)))

(fn read_action () Action
    (let key u8 (io_read u8 a2.KBD))
    (if (< key 0x80)
        (return Action.none))
    (io_read u8 a2.KBDSTRB)
    (match (& key 0x7F)
        (a2.KEY_LEFT Action.left)
        (a2.KEY_RIGHT Action.right)
        (a2.KEY_UP Action.up)
        (a2.KEY_DOWN Action.down)
        (_ Action.none)))

(fn click () void
    (io_read u8 a2.SPKR))

(fn main () void
    (hgr.init)
    (hgr.show)
    (hgr.flip dot.x dot.y)
    (while true
        (let action Action (read_action))
        (if (!= action Action.none)
            (do
                (hgr.flip dot.x dot.y)
                (match action
                    (Action.left
                        (if (> dot.x 0)
                            (dec dot.x)))
                    (Action.right
                        (if (< dot.x 279)
                            (inc dot.x)))
                    (Action.up
                        (if (> dot.y 0)
                            (dec dot.y)))
                    (Action.down
                        (if (< dot.y 191)
                            (inc dot.y)))
                    (_))
                (hgr.flip dot.x dot.y)
                (click)))))
