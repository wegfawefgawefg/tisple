; text page 2 ($0800-$0BFF) in both memories kept out of the program's layout, for a double
; hi-res program that shows the mixed window's text with page 2 (lib/dhgr's text_row on
; dhgr.PAGE2, or mixed mode while page 2 is shown). (import dhgr_text2) and nothing else.

(let text2_main (array u8 1024) :address 0x0800 :reserve)
(let text2_aux (array u8 1024) :address 0x0800 :in aux :reserve)
