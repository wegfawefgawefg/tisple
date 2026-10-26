; 24 lines on hi-res page 1 with lib/hgr's line and plot

(import hgr)

(let x0s (array u16 24) :address 0x5E00)
(let y0s (array u8 24) :address 0x5E40)
(let x1s (array u16 24) :address 0x5E80)
(let y1s (array u8 24) :address 0x5EC0)

(fn main () void
    (for i u8 0 24
        (hgr.line (at x0s i) (at y0s i) (at x1s i) (at y1s i))))
