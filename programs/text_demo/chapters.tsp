; the book: two chapters of text, a character chart, and three made by computing macros. The
; table of numbers lives in aux memory; the rest goes wherever main memory has room

(import book)

(const INTRO_LINES u16 39)

(book.chapter intro 39
    ()
    "# Tisple ][ text viewer"
    ""
    "This is a book of about 72 KB, built into the program while it compiled, and"
    "shown here 80 columns at a time. That is three times what once fit between"
    "$6000 and DOS, so the program is in pieces, wherever there was room:"
    ""
    "  - the code and the first chapters from $6000 up to $BEFF,"
    "  - more chapters below $6000, from $0B00, around text page 1,"
    "  - the last of main memory's share in the language card, $D000-$FFFF, which"
    "    the startup switched in, since nothing here calls the ROM,"
    "  - and the table of numbers in aux memory, the 80-column card's own 64 KB."
    ""
    "The status line at the bottom says where the chapter at the top of the page"
    "lives. A disk built with tisple build --disk loads every piece by itself."
    ""
    "Keys"
    ""
    "  up and down arrows        a line at a time"
    "  left and right arrows     a page at a time; SPACE turns the page too"
    "  RETURN                    the next chapter"
    "  T                         back to the top"
    "  ESC                       leave"
    ""
    "Chapters"
    ""
    "  1  this one"
    "  2  how the program is put together"
    "  3  the character set, MouseText included"
    "  4  a sine wave, plotted a line at a time"
    "  5  the first 1480 primes"
    "  6  the numbers from 1 to 400: squares, cubes, hex, binary and factors"
    ""
    "Chapters 4 to 6 were computed while compiling, by macros in book.tsp: strings"
    "and floats exist only there, and what reaches the Apple II is bytes, one 80-"
    "column line of screen codes after another. Each chapter is one let, its lines"
    "back to back, so line n of a chapter is 80 n bytes in."
    ""
    "Turn the page for how it all fits."
    "")

(const BUILT_LINES u16 44)

(book.chapter built 44
    ()
    "# How the program is put together"
    ""
    "Memory. The linker fills from the origin first, as it always did; what does not"
    "fit goes on into the memory below the origin, then the language card. Whole"
    "functions move, so a branch never leaves its function, and data fills the gaps."
    "Text page 1 is never used. The other screens are ordinary memory unless the"
    "program keeps one, which takes a let:"
    ""
    "    (let page (array u8 8192) :address 0x2000 :reserve)"
    ""
    "A :reserve let is not a variable: the program reaches that memory by address,"
    "with (as (ptr u8) 0x2000) as before, and the linker keeps out."
    ""
    "Aux memory. One placement option puts a let there:"
    ""
    "    (book.numbers numbers 404 (:in aux))   ; in chapters.tsp"
    ""
    "and nothing else changes in the language. Code never reads aux data by name;"
    "it copies lines across with lib/auxmem:"
    ""
    "    (auxmem.read (addr_of (at line 0)) p 80)"
    ""
    "auxmem's mover is 99 bytes of 6502 code, run from page 3 with a copy at the"
    "same address in aux memory, since RAMRD sends the instruction fetches there too."
    ""
    "80 columns. lib/text80 writes the even columns with PAGE2 on, which with 80STORE"
    "means aux memory, and the odd ones with it off. It reaches the screen with"
    "io_write, so no store can move across the switch."
    ""
    "The disk. The loader is three sectors at $0800 and reads a table of segments:"
    "main memory, the language card's bank 2, and aux memory, each sector of that"
    "read to text page 1 and copied over with RAMWRT on. Every segment starts on a"
    "track of its own and track 17 is left to the DOS 3.3 catalog beside the"
    "program."
    ""
    "The machine. Tisple ]['s own Apple IIe now has the extended 80-column card:"
    "aux memory and its language card, 80-column text in its own font at half"
    "width, double hi-res and double lo-res, and MouseText drawn for it. Zellyn"
    "Hunter's a2audit finds 128K and passes its aux memory tests."
    ""
    "Sizes, from tisple build --disk: the main file from $6000, a segment below it,"
    "one in the language card and one in aux memory. Together they are far past"
    "24 KB, and the boot takes about ten seconds of Apple time."
    "")

(const CHARSET_LINES u16 18)
(book.charset charset 18 ())

(const SINES_LINES u16 202)
(book.sines sines 202 ())

(const PRIMES_LINES u16 150)
(book.primes primes 150 ())

(const NUMBERS_LINES u16 404)
(book.numbers numbers 404 (:in aux))
