; a smaller lib/mockingboard_bends: vibrato (w), glide (p) and detune (t), no slide (s), for a
; program whose handler calls (mockingboard.service_bends_small) or
; (mockingboard.service_bends_small_bank1). It plays w, p and t exactly as the bends' player does
; (the same periods on the same ticks: tests/test_mockingboard_bends_small.cpp); an s is read and
; not played (the note holds its pitch). Made for Catwiz's rest song (2026-11-04). A program calls
; one of service, service_bends or this.
;
; A slot's pitch is goal + off: off, signed, goes a stride nearer 0 each tick (a glide from the
; note before), and the vibrato goes on top. What it leaves out of the bends: the slide (no goal
; at the end of the periods), the detune as a word (t's range is a signed byte), one shift loop
; for both of a note's steps (a loop each), the period's own register writes (player.out writes
; the same registers: the volume and mixer it also looks at have not changed). At the tightest
; size budget its handler is about 730 bytes more than the plain player's, the bends' about 815
; (docs/mockingboard.md, The smaller player).

(import mockingboard_player player)
(import mockingboard_song tune)

(let vib_shift (array u8 7))  ; the depth as a shift of the note's period; 0 none
(let vib_rate (array u8 7))   ; the wobble's phase step a tick
(let vib_delay (array u8 7))  ; ticks of each note before it wobbles
(let vib_amount (array u8 7)) ; the note's: period >> vib_shift, at least 1; 0 for none
(let vib_phase (array u8 7))
(let vib_wait (array u8 7))
(let glide (array u8 7))    ; a glide's shift (8 - speed); 0 none (a slide is none here)
(let detune (array u8 7))   ; added to each note's period: t's -64 to 64 as a signed byte
(let goal (array u16 7))    ; the note's period, detuned
(let off (array u16 7))     ; the pitch less goal, two's complement; 0 there
(let stride (array u16 7))  ; added to off each tick, toward 0 (it is 0 once it would pass)
(let moving (array bool 7)) ; off is not 0, or the note has vibrato

; a song's or an effect's first byte: no vibrato, glide or detune, no note to glide from
(fn fresh ((s u8)) void
    (set (at vib_shift s) 0)
    (set (at glide s) 0)
    (set (at detune s) 0)
    (set (at player.period s) 0)
    (set (at moving s) false))

; p shifted right k times
(fn shifted ((p u16) (k u8)) u16
    (while (!= k 0)
        (set p (>> p 1))
        (set k (- k 1)))
    p)

; a note, a rest or a tie, then where the note's pitch goes
(fn sound ((s u8) (b u8)) void
    (let last u16 (at player.period s)) ; what sounded last: a glide's start
    (player.sound s b)
    (if (>= b tune.REST)
        (do
            (if (= b tune.REST)
                (set (at moving s) false))
            (return)))
    (let p u16 (+ (at player.period s) (as u16 (as i8 (at detune s)))))
    (let f u16 p)
    (set (at moving s) false)
    (let k u8 (at glide s))
    (let kv u8 (at vib_shift s))
    (if (!= (| k kv) 0)
        (do
            (let d u16 (shifted p k))
            (if (= d 0)
                (set d 1))
            (let a u8 0)
            (if (!= kv 0)
                (do
                    (set a (as u8 (shifted p kv)))
                    (if (= a 0)
                        (set a 1))))
            (set (at vib_amount s) a)
            (if (and (!= k 0) (!= last 0))
                (set f last))
            (set (at goal s) p)
            (let o u16 (- f p))
            (set (at off s) o)
            ; toward 0: up from below, down from above
            (set (at stride s) (if (>= o 0x8000)
                d
                (- 0 d)))
            (set (at vib_phase s) 0)
            (set (at vib_wait s) (at vib_delay s))
            (set (at moving s) (or (!= a 0) (!= o 0)))))
    (if (!= f (at player.period s))
        (do
            (set (at player.period s) f)
            (player.out s))))

; one tick of a note after its first: off a stride nearer 0 (0 once it would pass it), then the vibrato, a triangle of
; two heights (half the amount, the amount) a quarter of its phase each, above the pitch then
; below (the bends' move)
(fn move ((s u8)) void
    (let o u16 (at off s))
    (if (!= o 0)
        (do
            (let n u16 (+ o (at stride s)))
            (if (>= (^ n o) 0x8000)
                (set n 0))
            (set o n)
            (set (at off s) o)))
    (let r u16 (+ (at goal s) o))
    (let f u16 r)
    (let a u8 (at vib_amount s))
    (if (= a 0)
        (do
            (if (= o 0)
                (set (at moving s) false)))
        (if (!= (at vib_wait s) 0)
            (set (at vib_wait s) (- (at vib_wait s) 1))
            (do
                (let ph u8 (at vib_phase s))
                (set (at vib_phase s) (+ ph (at vib_rate s)))
                (let w u16
                    (as u16 (if (= (& (+ ph 0x20) 0x40) 0)
                        (>> a 1)
                        a)))
                (set f (if (< ph 0x80)
                    (+ r w)
                    (- r w)))
                ; past the 12 bits either way: the pitch without it
                (if (>= f 4096)
                    (set f r)))))
    (if (!= f (at player.period s))
        (do
            (set (at player.period s) f)
            (player.out s))))

(player.events_fn events sound
    ((if (= i 0)
            (fresh s)))
    ((fresh s))
    ((if (= b tune.VIBRATO)
            (do
                (set (at vib_shift s) (load (+ p i)))
                (set (at vib_rate s) (load (+ p (+ i 1))))
                (set (at vib_delay s) (load (+ p (+ i 2))))
                (set i (+ i 3))
                (continue)))
        (if (= b tune.BEND)
            (do
                ; $10 + k a glide; none, or a slide ($20 and up), 0
                (let g u8 (load (+ p i)))
                (set (at glide s) (if (< g 0x20)
                    (& g 0x0F)
                    0))
                (set i (+ i 1))
                (continue)))
        (if (= b tune.DETUNE)
            (do
                (set (at detune s) (load (+ p i)))
                (set i (+ i 2))
                (continue)))))

(player.banked_fn events_bank1 events)

(player.tick_fn tick events (if (at moving s)
    (move s)))

(player.tick_fn tick_bank1 events_bank1 (if (at moving s)
    (move s)))
