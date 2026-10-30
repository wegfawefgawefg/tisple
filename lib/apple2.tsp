; Apple II hardware addresses, named as in Apple's reference manuals.
; any access to a soft switch flips it, read or write, unless noted.
; (import apple2 a2) then a2.SPKR

; keyboard
(const KBD u16 0xC000)     ; read: last key, bit 7 set while a new key waits
(const KBDSTRB u16 0xC010) ; access to clear bit 7 of KBD

; key codes, as (& (io_read u8 KBD) 0x7F) gives them
(const KEY_LEFT u8 0x08)  ; left arrow
(const KEY_RIGHT u8 0x15) ; right arrow
(const KEY_UP u8 0x0B)    ; up arrow (IIe)
(const KEY_DOWN u8 0x0A)  ; down arrow (IIe)
(const KEY_RETURN u8 '\r')
(const KEY_ESC u8 0x1B)
(const KEY_SPACE u8 ' ')
(const KEY_DELETE u8 0x7F)

; sound and cassette
(const SPKR u16 0xC030)    ; each access clicks the speaker
(const TAPEOUT u16 0xC020) ; each access toggles cassette out

; display soft switches
(const TXTCLR u16 0xC050) ; graphics
(const TXTSET u16 0xC051) ; text
(const MIXCLR u16 0xC052) ; full screen
(const MIXSET u16 0xC053) ; 4 lines of text at the bottom
(const LOWSCR u16 0xC054) ; display page 1
(const HISCR u16 0xC055)  ; display page 2
(const LORES u16 0xC056)  ; lo-res graphics
(const HIRES u16 0xC057)  ; hi-res graphics

; annunciator outputs on the game port
(const AN0OFF u16 0xC058)
(const AN0ON u16 0xC059)
(const AN1OFF u16 0xC05A)
(const AN1ON u16 0xC05B)
(const AN2OFF u16 0xC05C)
(const AN2ON u16 0xC05D)
(const AN3OFF u16 0xC05E) ; IIe with 80-column card: double-res on
(const AN3ON u16 0xC05F)

; game port inputs: bit 7
(const BUTN0 u16 0xC061) ; open-apple on IIe
(const BUTN1 u16 0xC062) ; closed-apple on IIe
(const BUTN2 u16 0xC063)
(const PADDL0 u16 0xC064) ; bit 7 set until the paddle timer runs out
(const PADDL1 u16 0xC065)
(const PADDL2 u16 0xC066)
(const PADDL3 u16 0xC067)
(const PTRIG u16 0xC070) ; access to start the paddle timers

; IIe memory and video switches: these are writes, any value
(const CLR80STORE u16 0xC000)
(const SET80STORE u16 0xC001)
(const RDMAINRAM u16 0xC002)
(const RDCARDRAM u16 0xC003)
(const WRMAINRAM u16 0xC004)
(const WRCARDRAM u16 0xC005)
(const SETSTDZP u16 0xC008)
(const SETALTZP u16 0xC009)
(const CLR80VID u16 0xC00C)
(const SET80VID u16 0xC00D)
(const CLRALTCHAR u16 0xC00E)
(const SETALTCHAR u16 0xC00F)

; IIe status reads: bit 7
(const RDLCRAM u16 0xC012)  ; the language card's ram read, not the rom
(const RDRAMRD u16 0xC013)  ; reads of $0200-$BFFF from aux memory
(const RDRAMWRT u16 0xC014) ; writes of $0200-$BFFF to aux memory
(const RDVBLBAR u16 0xC019) ; IIe: 0 during vertical blank (IIc and IIgs differ)

; screen memory
(const TEXT_PAGE1 u16 0x0400)
(const TEXT_PAGE2 u16 0x0800)
(const HGR_PAGE1 u16 0x2000)
(const HGR_PAGE2 u16 0x4000)
