; keyboard command dispatch with an enum and match

(import apple2 a2)

(enum Command (none left right fire quit))

(let player_x u8 140)
(let running bool true)

(fn read_command () Command
    (let key u8 (io_read u8 a2.KBD))
    (if (< key 0x80)
        (return Command.none))
    (io_read u8 a2.KBDSTRB)
    (match (& key 0x7F)
        (a2.KEY_LEFT Command.left)
        (a2.KEY_RIGHT Command.right)
        (a2.KEY_SPACE Command.fire)
        (a2.KEY_ESC Command.quit)
        (_ Command.none)))

(fn shoot () void
    (io_read u8 a2.SPKR))

(fn run ((c Command)) void
    (match c
        (Command.none)
        (Command.left (set player_x (- player_x 1)))
        (Command.right (set player_x (+ player_x 1)))
        (Command.fire
            (shoot)
            (shoot))
        (Command.quit (set running false))))

(fn main () void
    (while running
        (run (read_command))))
