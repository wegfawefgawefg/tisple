; A text adventure's parser and verbs, run over a script of 300 typed commands (the
; race's script buffer: lines ending in a return, a 0 at the end). Each line is
; upper-cased and split into words of at most 6 letters, each word looked up in the
; vocabulary, the noise words dropped, and the verb dispatched to its handler, which
; moves the player, takes and drops things, opens the door and the box. Every answer is
; a byte appended to the race's log buffer; the checksum folds the log and the world.

(const WORD_LEN u8 6)
(const MAX_WORDS u8 8)
(const LINE_MAX u8 64)
(const NONE u8 255)
(const CARRIED u8 254)
(const IN_BOX u8 253)
(const OBJECTS u8 8)

(enum Kind (unknown verb noun dir prep noise))

(const NORTH u8 0)
(const SOUTH u8 1)
(const EAST u8 2)
(const WEST u8 3)
(const UP u8 4)
(const DOWN u8 5)

(const LAMP u8 0)
(const KEY u8 1)
(const COIN u8 2)
(const BOX u8 3)
(const DOOR u8 4)
(const SWORD u8 5)
(const BOOK u8 6)
(const ROPE u8 7)

(const P_IN u8 1)
(const P_WITH u8 2)

(const M_EMPTY u8 1)
(const M_UNKNOWN u8 2)
(const M_NO_VERB u8 3)
(const M_WHERE u8 4)
(const M_NO_WAY u8 5)
(const M_DOOR_SHUT u8 6)
(const M_ROOM u8 7)
(const M_CARRYING u8 8)
(const M_NOTHING u8 9)
(const M_WHAT u8 10)
(const M_FIXED u8 11)
(const M_HAVE_IT u8 12)
(const M_OK u8 13)
(const M_NOT_HERE u8 14)
(const M_NOT_CARRIED u8 15)
(const M_OBJECT u8 16)
(const M_LOCKED u8 40)
(const M_ALREADY u8 41)
(const M_CANT u8 42)
(const M_NEED_KEY u8 43)
(const M_BOX_SHUT u8 44)
(const M_DESCRIBE u8 45)
(const M_END u8 255)

(struct Word ((text (array u8 6)) (id u8) (kind Kind)))

; a vocabulary entry: the text padded with spaces to WORD_LEN
(macro word (text id word_kind)
    (let letters (chars text))
    `(make Word
        :text (make (array u8 6) ,@(append letters (repeat (- 6 (len letters)) 32)))
        :id ,id
        :kind ,word_kind))

(const VOCAB_COUNT u8 46)

(const VOCAB (array Word 46)
    (make (array Word 46)
        (word "GO" 1 Kind.verb)
        (word "WALK" 1 Kind.verb)
        (word "TAKE" 2 Kind.verb)
        (word "GET" 2 Kind.verb)
        (word "DROP" 3 Kind.verb)
        (word "OPEN" 4 Kind.verb)
        (word "CLOSE" 5 Kind.verb)
        (word "SHUT" 5 Kind.verb)
        (word "UNLOCK" 6 Kind.verb)
        (word "PUT" 7 Kind.verb)
        (word "LOOK" 8 Kind.verb)
        (word "L" 8 Kind.verb)
        (word "INVENT" 9 Kind.verb)
        (word "I" 9 Kind.verb)
        (word "EXAMIN" 10 Kind.verb)
        (word "X" 10 Kind.verb)
        (word "NORTH" NORTH Kind.dir)
        (word "N" NORTH Kind.dir)
        (word "SOUTH" SOUTH Kind.dir)
        (word "S" SOUTH Kind.dir)
        (word "EAST" EAST Kind.dir)
        (word "E" EAST Kind.dir)
        (word "WEST" WEST Kind.dir)
        (word "W" WEST Kind.dir)
        (word "UP" UP Kind.dir)
        (word "U" UP Kind.dir)
        (word "DOWN" DOWN Kind.dir)
        (word "D" DOWN Kind.dir)
        (word "LAMP" LAMP Kind.noun)
        (word "LANTER" LAMP Kind.noun)
        (word "KEY" KEY Kind.noun)
        (word "COIN" COIN Kind.noun)
        (word "BOX" BOX Kind.noun)
        (word "DOOR" DOOR Kind.noun)
        (word "SWORD" SWORD Kind.noun)
        (word "BOOK" BOOK Kind.noun)
        (word "ROPE" ROPE Kind.noun)
        (word "IN" P_IN Kind.prep)
        (word "INTO" P_IN Kind.prep)
        (word "WITH" P_WITH Kind.prep)
        (word "THE" 0 Kind.noise)
        (word "A" 0 Kind.noise)
        (word "AN" 0 Kind.noise)
        (word "BRASS" 0 Kind.noise)
        (word "GOLD" 0 Kind.noise)
        (word "OLD" 0 Kind.noise)))

; exits by room and direction: north, south, east, west, up, down
(const EXITS (array u8 36)
    (make (array u8 36)
        1 NONE 3 NONE NONE NONE
        NONE 0 2 NONE 4 NONE
        NONE 5 NONE 1 NONE NONE
        NONE NONE NONE 0 NONE 4
        NONE NONE NONE NONE 3 1
        2 NONE NONE NONE NONE NONE))

(const START (array u8 8) (make (array u8 8) 0 3 4 2 1 5 0 4))

(let script (array u8 4096) :address 0x2000)
(let log_buf (array u8 4096) :address 0x3000)
(let checksum u16 :address 0x5F00)

(let line (array u8 64))
(let line_len u8 0)
(let words (array (array u8 6) 8))
(let word_ids (array u8 8))
(let word_kinds (array Kind 8))
(let word_count u8 0)

(let room u8 0)
(let loc (array u8 8))
(let door_open bool false)
(let door_locked bool true)
(let box_open bool false)
(let moves u16 0)
(let log_len u16 0)
(let script_at u16 0)

(let noun1 u8 NONE)
(let noun2 u8 NONE)
(let prep u8 NONE)
(let dir u8 NONE)

(fn say ((code u8)) void
    (set (at log_buf log_len) code)
    (set log_len (+ log_len 1)))

; the next line of the script, upper-cased; false at the end of the script
(fn read_line () bool
    (set line_len 0)
    (if (= (at script script_at) 0)
        (return false))
    (while true
        (let c u8 (at script script_at))
        (if (= c 0)
            (break))
        (set script_at (+ script_at 1))
        (if (= c '\r')
            (break))
        (if (and (>= c 'a') (<= c 'z'))
            (set c (- c (- 'a' 'A'))))
        (if (< line_len LINE_MAX)
            (do
                (set (at line line_len) c)
                (set line_len (+ line_len 1)))))
    true)

(fn is_letter ((c u8)) bool
    (and (>= c 'A') (<= c 'Z')))

; words of letters, each cut to 6 and padded with spaces; at most 8
(fn split_words () void
    (let i u8 0)
    (set word_count 0)
    (while (and (< i line_len) (< word_count MAX_WORDS))
        (while (and (< i line_len) (not (is_letter (at line i))))
            (set i (+ i 1)))
        (if (= i line_len)
            (break))
        (let n u8 0)
        (while (and (< i line_len) (is_letter (at line i)))
            (if (< n WORD_LEN)
                (do
                    (set (at (at words word_count) n) (at line i))
                    (set n (+ n 1))))
            (set i (+ i 1)))
        (while (< n WORD_LEN)
            (set (at (at words word_count) n) ' ')
            (set n (+ n 1)))
        (set word_count (+ word_count 1))))

(fn same_text ((v u8) (w u8)) bool
    (for k u8 0 WORD_LEN
        (if (!= (at (at VOCAB v text) k) (at (at words w) k))
            (return false)))
    true)

(fn look_up ((w u8)) void
    (for v u8 0 VOCAB_COUNT
        (if (same_text v w)
            (do
                (set (at word_ids w) (at VOCAB v id))
                (set (at word_kinds w) (at VOCAB v kind))
                (return))))
    (set (at word_ids w) NONE)
    (set (at word_kinds w) Kind.unknown))

; look every word up and drop the noise words
(fn look_up_words () void
    (let kept u8 0)
    (for w u8 0 word_count
        (look_up w)
        (if (!= (at word_kinds w) Kind.noise)
            (do
                (set (at word_ids kept) (at word_ids w))
                (set (at word_kinds kept) (at word_kinds w))
                (set kept (+ kept 1)))))
    (set word_count kept))

(fn list_objects_at ((where u8)) void
    (for o u8 0 OBJECTS
        (if (= (at loc o) where)
            (say (+ M_OBJECT o)))))

(fn describe () void
    (say M_ROOM)
    (say room)
    (list_objects_at room))

(fn here ((o u8)) bool
    (or (= (at loc o) room) (= (at loc o) CARRIED)))

(fn at_door () bool
    (or (= room 1) (= room 2)))

; the handlers give their answer, or 0 when they have said it themselves
(fn do_go () u8
    (if (= dir NONE)
        (return M_WHERE))
    (let to u8 (at EXITS (+ (* room 6) dir)))
    (if (= to NONE)
        (return M_NO_WAY))
    (if (and (or (and (= room 1) (= dir EAST)) (and (= room 2) (= dir WEST))) (not door_open))
        (return M_DOOR_SHUT))
    (set room to)
    (describe)
    0)

(fn do_inventory () u8
    (let any bool false)
    (say M_CARRYING)
    (for o u8 0 OBJECTS
        (if (= (at loc o) CARRIED)
            (do
                (say (+ M_OBJECT o))
                (set any true))))
    (if any
        0
        M_NOTHING))

(fn do_take () u8
    (if (= noun1 NONE)
        (return M_WHAT))
    (if (= noun1 DOOR)
        (return M_FIXED))
    (if (= (at loc noun1) CARRIED)
        (return M_HAVE_IT))
    (if (!= (at loc noun1) room)
        (return M_NOT_HERE))
    (set (at loc noun1) CARRIED)
    M_OK)

(fn do_drop () u8
    (if (= noun1 NONE)
        (return M_WHAT))
    (if (!= (at loc noun1) CARRIED)
        (return M_NOT_CARRIED))
    (set (at loc noun1) room)
    M_OK)

; open is true for open, false for close
(fn do_open ((open bool)) u8
    (if (= noun1 NONE)
        (return M_WHAT))
    (if (= noun1 DOOR)
        (do
            (if (not (at_door))
                (return M_NOT_HERE))
            (if (and open door_locked)
                (return M_LOCKED))
            (if (= door_open open)
                (return M_ALREADY))
            (set door_open open)
            (return M_OK)))
    (if (= noun1 BOX)
        (do
            (if (not (here BOX))
                (return M_NOT_HERE))
            (if (= box_open open)
                (return M_ALREADY))
            (set box_open open)
            (return M_OK)))
    M_CANT)

(fn do_unlock () u8
    (if (!= noun1 DOOR)
        (return M_CANT))
    (if (not (at_door))
        (return M_NOT_HERE))
    (if (or (!= noun2 KEY) (!= (at loc KEY) CARRIED))
        (return M_NEED_KEY))
    (if (not door_locked)
        (return M_ALREADY))
    (set door_locked false)
    M_OK)

(fn do_put () u8
    (if (= noun1 NONE)
        (return M_WHAT))
    (if (or (!= prep P_IN) (!= noun2 BOX))
        (return M_CANT))
    (if (!= (at loc noun1) CARRIED)
        (return M_NOT_CARRIED))
    (if (not (here BOX))
        (return M_NOT_HERE))
    (if (not box_open)
        (return M_BOX_SHUT))
    (set (at loc noun1) IN_BOX)
    M_OK)

(fn do_examine () u8
    (if (= noun1 NONE)
        (return M_WHAT))
    (if (not (here noun1))
        (return M_NOT_HERE))
    (say M_DESCRIBE)
    (say noun1)
    (if (and (= noun1 BOX) box_open)
        (list_objects_at IN_BOX))
    0)

; the verb's noun, a preposition and its noun, and a direction, from the other words
(fn find_objects () void
    (set noun1 NONE)
    (set noun2 NONE)
    (set prep NONE)
    (set dir NONE)
    (for w u8 1 word_count
        (let kind Kind (at word_kinds w))
        (if (and (= kind Kind.prep) (= prep NONE))
            (set prep (at word_ids w))
            (if (= kind Kind.noun)
                (if (and (= prep NONE) (= noun1 NONE))
                    (set noun1 (at word_ids w))
                    (if (and (!= prep NONE) (= noun2 NONE))
                        (set noun2 (at word_ids w))))
                (if (and (= kind Kind.dir) (= dir NONE))
                    (set dir (at word_ids w)))))))

(fn run_command () u8
    (if (= word_count 0)
        (return M_EMPTY))
    (for w u8 0 word_count
        (if (= (at word_kinds w) Kind.unknown)
            (do
                (say M_UNKNOWN)
                (say w)
                (return 0))))
    (find_objects)
    (if (= (at word_kinds 0) Kind.dir)
        (do
            (set dir (at word_ids 0))
            (return (do_go))))
    (if (!= (at word_kinds 0) Kind.verb)
        (return M_NO_VERB))
    (match (at word_ids 0)
        (1 (do_go))
        (2 (do_take))
        (3 (do_drop))
        (4 (do_open true))
        (5 (do_open false))
        (6 (do_unlock))
        (7 (do_put))
        (8
            (describe)
            0)
        (9 (do_inventory))
        (10 (do_examine))
        (_ 0)))

(fn main () void
    (set room 0)
    (for o u8 0 OBJECTS
        (set (at loc o) (at START o)))
    (set door_open false)
    (set door_locked true)
    (set box_open false)
    (set moves 0)
    (set log_len 0)
    (set script_at 0)
    (while (read_line)
        (split_words)
        (look_up_words)
        (let answer u8 (run_command))
        (if (!= answer 0)
            (say answer))
        (say M_END)
        (set moves (+ moves 1)))
    (let sum_a u16 0)
    (let sum_b u16 0)
    (for i u16 0 log_len
        (set sum_a (+ sum_a (as u16 (at log_buf i))))
        (set sum_b (+ sum_b sum_a)))
    (set sum_a (+ sum_a (as u16 room)))
    (set sum_b (+ sum_b sum_a))
    (for o u8 0 OBJECTS
        (set sum_a (+ sum_a (as u16 (at loc o))))
        (set sum_b (+ sum_b sum_a)))
    (let flags u16 0)
    (if door_open
        (set flags (+ flags 1)))
    (if door_locked
        (set flags (+ flags 2)))
    (if box_open
        (set flags (+ flags 4)))
    (set sum_a (+ sum_a flags))
    (set sum_b (+ sum_b sum_a))
    (set sum_a (+ sum_a moves))
    (set sum_b (+ sum_b sum_a))
    (set sum_a (+ sum_a log_len))
    (set sum_b (+ sum_b sum_a))
    (set checksum (^ sum_a sum_b)))
