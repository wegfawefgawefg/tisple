; keyboard command dispatch with an enum and match

(const KEYBOARD u16 0xC000)
(const KEY_STROBE u16 0xC010)

(enum Command none left right fire quit)

(let player_x u8 140)
(let running bool true)

(fn read_command
    (let key (io_read u8 KEYBOARD))
    (if (< key 0x80)
        (return Command.none))
    (io_read u8 KEY_STROBE)
    (match (& key 0x7F)
        (0x08 Command.left)  ; left arrow
        (0x15 Command.right) ; right arrow
        (0x20 Command.fire)  ; space
        (0x1B Command.quit)  ; escape
        (_ Command.none)))

(fn shoot
    (io_read u8 0xC030))

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
