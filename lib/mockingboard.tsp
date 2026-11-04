; the Mockingboard: two AY-3-8910 sound chips, six voices, and a 6522 timer that plays music
; from interrupts. (import mockingboard mb), then:
;   (fn irq () void :interrupt (mb.service)) ; the program's handler: every tick runs here
;   (mb.init)                ; find the card, reset it, start the tick (60 a second)
;   (mb.play (addr_of (at music 0)))          ; a song from lib/mockingboard_song
;   (mb.play_bank1 (addr_of (at room 0)))     ; one in the card's bank 1 (a let :in bank1),
;                                             ; with (mb.service_bank1) in the handler
;   (mb.effect (addr_of (at zap 0)) 2 5)      ; an effect on voice 2, priority 5
;   (mb.stop)                ; the music off; effects go on
;   (mb.fade 15)             ; the music down to silent over 1 s, then stopped (at once when
;                            ; no voice plays); (mb.faded)
;   (mb.fade_in 15)          ; and from silent up to full
;   (mb.off)                 ; before main returns: the timer off, the chips quiet
; mb.found says whether there is a card, mb.slot where. With no card the music and effects are
; silent; (mb.speaker_effect (addr_of (at zap 0))) plays an effect on the speaker instead, while
; main waits (a program that never calls it has none of its code).
;
; The handler must call (mb.service) and the program must call (mb.off) before it ends: the
; card's interrupts go on otherwise, into whatever runs next. Songs and effects are made with
; lib/mockingboard_song, which has the format.
;
; Detection: in slots 1-7, the first 6522's timer 1 counts down every cycle, so two reads of
; its low byte a fixed few instructions apart differ by the same small number each time; an
; empty slot reads the floating bus, a rom the same byte twice. Then its latch has to keep
; what is written to it. The timer: timer 1 of the first 6522, free-running, every rate
; cycles (17030 by default: one video frame, 59.9 a second).
;
; Voices 0-2 are the first chip's channels A-C ($Cn00, the left speaker), 3-5 the second's
; ($Cn80, right). An effect takes one voice while it plays; the music on that voice goes on
; silently under it and comes back after. One effect plays at a time: a new one with a higher
; priority than the one playing cuts it off, any other waits in line (4 at most; more are
; dropped) and plays when the ones before it are over.

(import apple2 a2)
(import mockingboard_bends bends)
(import mockingboard_player player)
(import mockingboard_song tune)
(import random)

(let found bool false)
(let slot u8 0)

; the bytes after each command from tune.NOISE on (n h w p/s t), for speaker_effect
(const OPERANDS (array u8 5) (make (array u8 5) 1 3 3 1 2))
(let rate u16 17030) ; cycles a tick

(fn slot_page ((n u8)) u16
    (+ 0xC000 (<< (as u16 n) 8)))

; how far timer 1's low byte moves between two reads
(fn timer_step ((t1 u16)) u8
    (let first u8 (io_read u8 t1))
    (let second u8 (io_read u8 t1))
    (- first second))

; a 6522 at page: a timer that counts, and a latch that keeps what is written
(fn is_card ((page u16)) bool
    (let t1 u16 (+ page player.T1C_L))
    (let d u8 (timer_step t1))
    (if (or (< d 2) (> d 40))
        (return false))
    (for i u8 0 3
        (wait_cycles 97) ; another phase of the counter
        (if (!= (timer_step t1) d)
            (return false)))
    (let latch u16 (+ page player.T1L_L))
    (io_write u8 latch 0x55)
    (if (!= (io_read u8 latch) 0x55)
        (return false))
    (io_write u8 latch 0xAA)
    (= (io_read u8 latch) 0xAA))

; main's copy of the register write
(player.psg_writer poke)

; the 6522s' ports to outputs, both chips reset, every register written
(fn reset () void
    (let page u16 (slot_page slot))
    (for chip u8 0 2
        (let via u16 (if (= chip 0)
            page
            (+ page player.SECOND)))
        (io_write u8 (+ via player.IER) 0x7F)
        (io_write u8 (+ via player.DDRB) 0xFF)
        (io_write u8 (+ via player.DDRA) 0xFF)
        (io_write u8 (+ via player.ORB) player.BUS_RESET)
        (io_write u8 (+ via player.ORB) player.BUS_INACTIVE)
        (for r u8 0 14
            (poke chip r (if (= r 7)
                0x3F
                0))))
    (for i u8 0 28
        (set (at player.sent i) (if (or (= i 7) (= i 21))
            0x3F
            0)))
    (for v u8 0 player.VOICES
        (set (at player.voice_mix v) 0)
        (set (at player.stale v) false)
        (set (at player.voice v) v)
        (set (at player.chip_of v) (at player.VOICE_CHIP v))
        (set (at player.owns v) true)
        (set (at player.tone_at v) (at player.VOICE_TONE v))
        (set (at player.volume_at v) (at player.VOICE_VOLUME v)))
    (for s u8 0 player.SLOTS
        (set (at player.playing s) false))
    (set (at player.owns player.FX) false)
    (set player.fx_on false)
    (set player.line_count 0)
    (set player.cut false)
    (set player.hush 0)
    (set player.hush_to 0))

; timer 1 of the first 6522 free-running every rate cycles, its interrupt on
(fn start_timer () void
    (let page u16 (slot_page slot))
    (io_write u8 (+ page player.ACR) 0x40)
    (io_write u16 (+ page player.T1C_L) (- rate 2)) ; the high byte starts it
    (io_write u8 (+ page player.IER) 0xC0))

; find the card, reset it and start the tick
(fn init () void
    (set found false)
    (set player.found false)
    (for n u8 1 8
        (if (is_card (slot_page n))
            (do
                (set slot n)
                (set found true)
                (break))))
    (if (not found)
        (return))
    (sei)
    (let page u16 (slot_page slot))
    (set player.page page)
    (reset)
    (set player.found true)
    (start_timer)
    (cli))

; ticks every cycles cycles from the next one on
(fn set_rate ((cycles u16)) void
    (set rate cycles)
    (if (not found)
        (return))
    (io_write u16 (+ (slot_page slot) player.T1L_L) (- rate 2)))

; main's copy of player.word
(fn header_word ((p (ptr u8))) u16
    (| (as u16 (load p)) (<< (as u16 (load (+ p 1))) 8)))

; a song from its first byte, every voice from the start
(fn play ((song (ptr u8))) void
    (if (not found)
        (return))
    (sei)
    (set player.song song)
    (for v u8 0 player.VOICES
        (let off u16 (header_word (+ song (<< v 1))))
        (set (at player.playing v) (!= off 0))
        (set (at player.order v) (+ song off))
        (set (at player.pos v) (addr_of (at player.end_mark 0)))
        (set (at player.index v) 0)
        (set (at player.wait v) 1)
        (set (at player.length v) 8)
        (set (at player.volume v) 12)
        (set (at player.level v) 0)
        (set (at player.decay v) 0)
        (set (at player.mix v) 1)
        ; a silent voice goes quiet now; the others with their first note
        (set (at player.stale v) (= off 0)))
    (set player.any_stale true)
    (cli))

; a song in the language card's bank 1 (a let :in bank1, which lib/disk reads into with
; disk.read_card_with), for a handler that calls service_bank1: play's, with bank 1 in for the
; header's reads (they call nothing, and what this names stays out of $D000-$DFFF) and each
; music voice marked to be read with bank 1 in. From then on every song plays that way, play's
; too: with service_bank1, songs in main memory work (bank 1 in does not hide them), songs in
; bank 2's $D000-$DFFF do not
(fn play_bank1 ((song (ptr u8))) void
    (if (not found)
        (return))
    (sei)
    (set player.song song)
    (io_read u8 player.BANK1_IN)
    (for v u8 0 player.VOICES
        (let h (ptr u8) (+ song (<< v 1)))
        (let off u16 (| (as u16 (load h)) (<< (as u16 (load (+ h 1))) 8)))
        (set (at player.playing v) (!= off 0))
        (set (at player.from_bank1 v) true)
        (set (at player.order v) (+ song off))
        (set (at player.pos v) (addr_of (at player.end_mark 0)))
        (set (at player.index v) 0)
        (set (at player.wait v) 1)
        (set (at player.length v) 8)
        (set (at player.volume v) 12)
        (set (at player.level v) 0)
        (set (at player.decay v) 0)
        (set (at player.mix v) 1)
        (set (at player.stale v) (= off 0)))
    (io_read u8 player.BANK2_IN)
    (set player.any_stale true)
    (cli))

(fn stop () void
    (if (not found)
        (return))
    (sei)
    (for v u8 0 player.VOICES
        (set (at player.playing v) false)
        (set (at player.stale v) true))
    (set player.any_stale true)
    (cli))

; the music (not the effects) toward to steps down, 0-15: 15 silent, 0 as the song has it, a
; step every 4 ticks from the next tick on. At 15 the music stops, as stop does. A voice on the
; hardware envelope stays as it is until then. play keeps the fade where it is
(fn fade ((to u8)) void
    (set player.hush_to (<< to 2))
    ; no music voice playing (a song whose voices have ended, or none yet): done at once, so a
    ; song can start at once
    (for v u8 0 player.VOICES
        (if (at player.playing v)
            (return)))
    (set player.hush player.hush_to))

; the music from steps down at once, then up to as the song has it: (mb.play s) (mb.fade_in 15)
; brings a song in from silence
(fn fade_in ((from u8)) void
    (set player.hush (<< from 2))
    (set player.hush_to 0))

; whether the fade has got where it goes
(fn faded () bool
    (= player.hush player.hush_to))

; the speaker's square wave for ticks ticks at the AY's period: 8 period cycles each half
(fn buzz ((tone u16) (ticks u8) (noisy bool)) void
    (let half u16 (<< tone 3))
    (if (< half 150)
        (set half 150))
    (let clicks u16 (/ rate half))
    (for t u8 0 ticks
        (for i u16 0 clicks
            (io_read u8 a2.SPKR)
            (if noisy
                (wait_cycles (- half (as u16 (& (random.byte) 0x3F))))
                (wait_cycles (- half 40))))))

(fn hush ((ticks u8)) void
    (for t u8 0 ticks
        (wait_cycles rate)))

; an effect on the speaker, while main waits: notes as square waves, the volume only on or off,
; noise as jittered clicks; no decay. For a program with no card: (if mb.found (mb.effect ...)
; (mb.speaker_effect ...))
(fn speaker_effect ((fx (ptr u8))) void
    (let p (ptr u8) fx)
    (let len u8 1)
    (let vol u8 15)
    (let mixing u8 1)
    (let tone u16 0)
    (while true
        (let b u8 (load p))
        (set p (+ p 1))
        (if (= b tune.END)
            (return))
        (if (< b tune.REST)
            (set tone (at tune.NOTE_PERIOD b)))
        (if (< b tune.LENGTH)
            (do
                (if (or (= b tune.REST) (or (= vol 0) (or (= mixing 0) (= tone 0))))
                    (hush len)
                    (buzz tone len (= mixing 2)))
                (continue)))
        (if (< b tune.VOLUME)
            (do
                (set len (- b 0x7F))
                (continue)))
        (if (< b tune.DECAY)
            (do
                (set vol (- b tune.VOLUME))
                (continue)))
        (if (and (>= b tune.MIXER) (< b tune.NOISE))
            (do
                (set mixing (- b tune.MIXER))
                (continue)))
        ; the commands with bytes after them, the bends' too (no pitch moves on the speaker)
        (if (>= b tune.NOISE)
            (set p (+ p (at OPERANDS (- b tune.NOISE)))))))

; an effect (a pattern from mockingboard_song's effect) on a voice, 0-5: now if its priority is
; higher than the one playing (which it cuts off), else after the ones playing and waiting.
; The handler starts it on the next tick.
(fn effect ((sound (ptr u8)) (voice u8) (priority u8)) void
    (if (not found)
        (return))
    (sei)
    (let first u8 player.line_first)
    (let count u8 player.line_count)
    ; the priority of the one playing, or of the next if none plays yet
    (let ahead u8 (if player.fx_on
        player.fx_priority
        (at player.line_priority first)))
    (let busy bool (or player.fx_on (!= count 0)))
    (let k u8 0)
    (if (and busy (> priority ahead))
        (do
            ; at the front, cutting off the one playing; a full line drops its last
            (set k (& (- first 1) (- player.LINE 1)))
            (set player.line_first k)
            (if (< count player.LINE)
                (set player.line_count (+ count 1)))
            (set player.cut player.fx_on))
        (do
            (if (= count player.LINE)
                (do
                    (cli)
                    (return)))
            (set k (& (+ first count) (- player.LINE 1)))
            (set player.line_count (+ count 1))))
    (set (at player.line_sound k) sound)
    (set (at player.line_voice k) voice)
    (set (at player.line_priority k) priority)
    (cli))

; whether an effect plays or waits to
(fn effect_playing () bool
    (or player.fx_on (!= player.line_count 0)))

; in the program's :interrupt handler
(fn service () void
    (player.tick))

; service's place in a program whose songs or effects use w, p or s (lib/mockingboard_bends):
; pitch that moves. A program calls one of the two; service plays those commands' bytes wrong
(fn service_bends () void
    (bends.tick))

; service's place, or service_bends', in a program that plays songs from the language card's
; bank 1 (play_bank1): the other two play them wrong. A voice's events, the bytes up to its next
; note, are read with bank 1 in (15 cycles more each time: docs/mockingboard.md)
(fn service_bank1 () void
    (player.tick_bank1))

(fn service_bends_bank1 () void
    (bends.tick_bank1))

; the timer and its interrupts off, every voice quiet
(fn off () void
    (if (not found)
        (return))
    (sei)
    (let page u16 (slot_page slot))
    (io_write u8 (+ page player.IER) 0x7F)
    (io_write u8 (+ page player.ACR) 0x00)
    (io_write u8 (+ (+ page player.SECOND) player.IER) 0x7F)
    (set player.found false)
    (set player.fx_on false)
    (set player.line_count 0)
    (for chip u8 0 2
        (for r u8 8 11
            (poke chip r 0)))
    (cli))
