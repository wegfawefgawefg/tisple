; pitch that moves, for lib/mockingboard_song's w (vibrato), p (glide), s (slide) and t
; (detune): the tick of a program whose :interrupt handler calls (mockingboard.service_bends)
; instead of (mockingboard.service). Its events, sound and tick are the player's with more in
; them (mockingboard_player's events_fn and tick_fn), so a program that calls service has none
; of this code, and none of these lets (the link leaves out a let only unreached code names).
;
; A slot's pitch is goal + off: off, signed, goes a stride nearer 0 each tick (a glide from the
; note before, or a slide to the end of the periods), and the vibrato goes on top; what comes
; out goes to period and the voice's registers when it changes. A note works out its stride
; and vibrato from its own period, shifted right: so a speed or depth is about the same
; interval on any note (a semitone is about 1/17 of the period).

(import mockingboard_player player)
(import mockingboard_song tune)

(let vib_shift (array u8 7))  ; the depth as a shift of the note's period; 0 none
(let vib_rate (array u8 7))   ; the wobble's phase step a tick
(let vib_delay (array u8 7))  ; ticks of each note before it wobbles
(let vib_amount (array u8 7)) ; the note's: period >> vib_shift, at least 1; 0 for none
(let vib_phase (array u8 7))
(let vib_wait (array u8 7))
(let bend (array u8 7))     ; as the BEND byte: 0, $10 + k glide, $20 + k up, $30 + k down
(let detune (array u16 7))  ; added to each note's period (two's complement: signed)
(let goal (array u16 7))    ; the period the bend goes to
(let off (array u16 7))     ; the pitch less goal, two's complement; 0 there
(let stride (array u16 7))  ; how much nearer 0 off gets a tick
(let moving (array bool 7)) ; off is not 0, or the note has vibrato

; a song's or an effect's first byte: no vibrato, bend or detune, no note to glide from (the
; period 0)
(fn fresh ((s u8)) void
    (set (at vib_shift s) 0)
    (set (at bend s) 0)
    (set (at detune s) 0)
    (set (at player.period s) 0)
    (set (at moving s) false))

; a note, a rest or a tie, then where the note's pitch goes
(fn sound ((s u8) (b u8)) void
    (let last u16 (at player.period s)) ; what sounded last: a glide's start
    (player.sound s b)
    (if (= b tune.REST)
        (set (at moving s) false))
    (if (>= b tune.REST)
        (return))
    (let f u16 0)
    (let p u16 (+ (at player.period s) (at detune s)))
    (let g u16 p)
    (set f p)
    (set (at moving s) false)
    (let mode u8 (at bend s))
    (let kv u8 (at vib_shift s))
    (if (!= (| mode kv) 0)
        (do
            ; the stride and the vibrato's amount: p shifted right, one shift at a time
            (let ks u8 (& mode 0x0F))
            (let q u16 p)
            (let d u16 1)
            (let a u8 0)
            (for j u8 1 8
                (set q (>> q 1))
                (if (= j ks)
                    (set d q))
                (if (= j kv)
                    (set a (as u8 q))))
            (if (= d 0)
                (set d 1))
            (set (at stride s) d)
            (if (and (!= kv 0) (= a 0))
                (set a 1))
            (set (at vib_amount s) a)
            ; from where the pitch was (a glide), or from the note to the end of the periods (a
            ; slide)
            (if (and (>= mode 0x10) (!= last 0))
                (set f last))
            (if (>= mode 0x20)
                (do
                    (set f p)
                    (set g (if (>= mode 0x30)
                        4095
                        0))))
            (set (at goal s) g)
            (set (at off s) (- f g))
            (set (at vib_phase s) 0)
            (set (at vib_wait s) (at vib_delay s))
            (set (at moving s) (or (!= a 0) (!= f g)))))
    (if (!= f (at player.period s))
        (do
            (set (at player.period s) f)
            (player.out s))))

; one tick of a note after its first: off a stride nearer 0, then the vibrato, a triangle of
; two heights (half the amount, the amount) a quarter of its phase each, above the pitch then
; below
(fn move ((s u8)) void
    (let o u16 (at off s))
    (if (!= o 0)
        (do
            (let d u16 (at stride s))
            (if (>= o 0x8000)
                (do
                    (set o (+ o d))
                    (if (< o 0x8000)
                        (set o 0)))
                (do
                    (set o (- o d))
                    (if (>= o 0x8000)
                        (set o 0))))
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
            ; the tone's two registers, if the slot has the voice
            (if (at player.owns s)
                (do
                    (let t u8 (at player.tone_at s))
                    (player.reg t (as u8 f))
                    (player.reg (+ t 1) (as u8 (>> f 8))))))))

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
                (set (at bend s) (load (+ p i)))
                (set i (+ i 1))
                (continue)))
        (if (= b tune.DETUNE)
            (do
                (set (at detune s) (player.word (+ p i)))
                (set i (+ i 2))
                (continue)))))

(player.banked_fn events_bank1 events)

(player.tick_fn tick events (if (at moving s)
    (move s)))

(player.tick_fn tick_bank1 events_bank1 (if (at moving s)
    (move s)))
