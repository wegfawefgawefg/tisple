; keyboard command dispatch with an enum and match

(import apple2 a2)

(enum Command none left right fire quit)

(let player_x u8 140)
(let running bool true)

(fn read_command (returns Command)
    (let key (io_read u8 a2.KBD))
    (if (< key 0x80)
        (return Command.none))
    (io_read u8 a2.KBDSTRB)
    (match (& key 0x7F)
        (0x08 Command.left)  ; left arrow
        (0x15 Command.right) ; right arrow
        (0x20 Command.fire)  ; space
        (0x1B Command.quit)  ; escape
        (_ Command.none)))

(fn shoot
    (io_read u8 a2.SPKR))

(fn run (args (c Command))
    (match c
        (Command.none (do))
        (Command.left (set player_x (- player_x 1)))
        (Command.right (set player_x (+ player_x 1)))
        (Command.fire (do
            (shoot)
            (shoot)))
        (Command.quit (set running false))))

(fn main
    (while running
        (run (read_command))))
