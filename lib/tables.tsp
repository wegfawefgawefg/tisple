; tables computed while compiling: (import tables), then
;   (tables.table SQUARES u16 256 square)  every (square i), i from 0 to 255
;   (tables.sprite BALL ".##." "####")     hi-res bytes from ASCII art

; (const NAME (array type count) (make (array type count) (f 0) (f 1) ...) options ...).
; f is a macro or a function of the index, and options are placement: :boundary 256
(macro table (name type count f & options)
    `(const ,name (array ,type ,count)
        (make (array ,type ,count) ,@(map f (range 0 count)))
        ,@options))

; one row of ASCII art as hi-res bytes: 7 pixels a byte, the low bit leftmost, # on, . off
(macro sprite_row (row)
    (let codes (chars row))
    (let bytes (list))
    (let byte 0)
    (let bit 0)
    (for i 0 (len codes)
        (match (nth codes i)
            (35 (set byte (| byte (<< 1 bit)))) ; #
            (46)                                ; .
            (_ (error "sprite rows are # and . only, got " row)))
        (set bit (+ bit 1))
        (if (= bit 7)
            (do
                (set bytes (append bytes (list byte)))
                (set byte 0)
                (set bit 0))))
    (if (> bit 0)
        (set bytes (append bytes (list byte))))
    bytes)

; the rows one after another, each (width + 6) / 7 bytes
(macro sprite (name & rows)
    (if (= (len rows) 0)
        (error "sprite " name " needs rows: (sprite " name " \"#..\" \".#.\")"))
    (let width (len (head rows)))
    (let bytes (list))
    (for i 0 (len rows)
        (let row (nth rows i))
        (if (!= (len row) width)
            (error "sprite " name ": row " i " is " (len row) " wide, the first is " width))
        (set bytes (append bytes (sprite_row row))))
    `(const ,name (array u8 ,(len bytes)) (make (array u8 ,(len bytes)) ,@bytes)))
