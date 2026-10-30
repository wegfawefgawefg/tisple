; Dhrystone 2.1 (Reinhold Weicker, 1988), for the program race. Changes, the same in
; program.c: the two records are globals; the record's union is a plain struct (only its
; first variant is ever used); the out parameters that point into a record (proc_3,
; proc_6, proc_7) return their value instead, since addr_of does not go through
; pointers; the run count comes from the race (runs); the values Dhrystone prints at the
; end are folded into checksum. Strings are copied and compared by loops (no library), and
; the strings main keeps are globals, since a local is never an array.

(enum Ident (ident_1 ident_2 ident_3 ident_4 ident_5))

(struct Rec ((ptr_comp (ptr Rec)) (discr Ident) (enum_comp Ident) (int_comp i16)
    (str_comp (array u8 31))))

(let checksum u16 :address 0x5F00)
(let runs u16 :address 0x5F02)

(let glob_rec Rec)
(let next_glob_rec Rec)
(let ptr_glob (ptr Rec) (addr_of glob_rec))
(let next_ptr_glob (ptr Rec) (addr_of next_glob_rec))
(let int_glob i16 0)
(let bool_glob bool false)
(let ch_1_glob u8 0)
(let ch_2_glob u8 0)
(let arr_1_glob (array i16 50))
(let arr_2_glob (array i16 2500)) ; 50 x 50, row by row
(let str_1_loc (array u8 31))
(let str_2_loc (array u8 31))

(fn str_copy ((dst (ptr u8)) (src (ptr u8))) void
    (let i u8 0)
    (while (!= (load (+ src i)) 0)
        (store (+ dst i) (load (+ src i)))
        (set i (+ i 1)))
    (store (+ dst i) 0))

; below 0, 0 or above 0 as strcmp
(fn str_compare ((a (ptr u8)) (b (ptr u8))) i8
    (let i u8 0)
    (while (= (load (+ a i)) (load (+ b i)))
        (if (= (load (+ a i)) 0)
            (return 0))
        (set i (+ i 1)))
    (if (< (load (+ a i)) (load (+ b i))) -1 1))

(fn proc_7 ((int_1_par_val i16) (int_2_par_val i16)) i16
    (let int_loc i16 (+ int_1_par_val 2))
    (+ int_2_par_val int_loc))

(fn func_3 ((enum_par_val Ident)) bool
    (let enum_loc Ident enum_par_val)
    (= enum_loc Ident.ident_3))

(fn proc_6 ((enum_val_par Ident)) Ident
    (let enum_ref Ident enum_val_par)
    (if (not (func_3 enum_val_par))
        (set enum_ref Ident.ident_4))
    (match enum_val_par
        (Ident.ident_1 (set enum_ref Ident.ident_1))
        (Ident.ident_2
            (if (> int_glob 100)
                (set enum_ref Ident.ident_1)
                (set enum_ref Ident.ident_4)))
        (Ident.ident_3 (set enum_ref Ident.ident_2))
        (Ident.ident_4)
        (Ident.ident_5 (set enum_ref Ident.ident_3)))
    enum_ref)

(fn proc_3 ((ptr_ref_par (ptr Rec))) (ptr Rec)
    (let result (ptr Rec) ptr_ref_par)
    (if (!= (as u16 ptr_glob) 0)
        (set result (load ptr_glob ptr_comp)))
    (store ptr_glob int_comp (proc_7 10 int_glob))
    result)

(fn proc_1 ((ptr_val_par (ptr Rec))) void
    (let next_record (ptr Rec) (load ptr_val_par ptr_comp))
    (store (load ptr_val_par ptr_comp) (load ptr_glob))
    (store ptr_val_par int_comp 5)
    (store next_record int_comp (load ptr_val_par int_comp))
    (store next_record ptr_comp (load ptr_val_par ptr_comp))
    (store next_record ptr_comp (proc_3 (load next_record ptr_comp)))
    (if (= (load next_record discr) Ident.ident_1)
        (do
            (store next_record int_comp 6)
            (store next_record enum_comp (proc_6 (load ptr_val_par enum_comp)))
            (store next_record ptr_comp (load ptr_glob ptr_comp))
            (store next_record int_comp (proc_7 (load next_record int_comp) 10)))
        (store ptr_val_par (load (load ptr_val_par ptr_comp)))))

(fn proc_2 ((int_par_ref (ptr i16))) void
    (let int_loc i16 (+ (load int_par_ref) 10))
    (let enum_loc Ident Ident.ident_2)
    (while true
        (if (= ch_1_glob 'A')
            (do
                (set int_loc (- int_loc 1))
                (store int_par_ref (- int_loc int_glob))
                (set enum_loc Ident.ident_1)))
        (if (= enum_loc Ident.ident_1)
            (break))))

(fn proc_4 () void
    (let bool_loc bool (= ch_1_glob 'A'))
    (set bool_glob (or bool_loc bool_glob))
    (set ch_2_glob 'B'))

(fn proc_5 () void
    (set ch_1_glob 'A')
    (set bool_glob false))

; arr_2 is 50 x 50, row by row
(fn proc_8 ((arr_1_par_ref (ptr i16)) (arr_2_par_ref (ptr i16)) (int_1_par_val i16)
        (int_2_par_val i16)) void
    (let int_loc i16 (+ int_1_par_val 5))
    (let row u16 (* (as u16 int_loc) 50))
    (store (+ arr_1_par_ref (as u16 int_loc)) int_2_par_val)
    (store (+ arr_1_par_ref (as u16 (+ int_loc 1))) (load (+ arr_1_par_ref (as u16 int_loc))))
    (store (+ arr_1_par_ref (as u16 (+ int_loc 30))) int_loc)
    (let int_index i16 int_loc)
    (while (<= int_index (+ int_loc 1))
        (store (+ arr_2_par_ref (+ row (as u16 int_index))) int_loc)
        (set int_index (+ int_index 1)))
    (let at_minus_1 (ptr i16) (+ arr_2_par_ref (+ row (as u16 (- int_loc 1)))))
    (store at_minus_1 (+ (load at_minus_1) 1))
    (store (+ arr_2_par_ref (+ (* (as u16 (+ int_loc 20)) 50) (as u16 int_loc)))
        (load (+ arr_1_par_ref (as u16 int_loc))))
    (set int_glob 5))

(fn func_1 ((ch_1_par_val u8) (ch_2_par_val u8)) Ident
    (let ch_1_loc u8 ch_1_par_val)
    (let ch_2_loc u8 ch_1_loc)
    (if (!= ch_2_loc ch_2_par_val)
        (return Ident.ident_1))
    (set ch_1_glob ch_1_loc)
    Ident.ident_2)

(fn func_2 ((str_1_par_ref (ptr u8)) (str_2_par_ref (ptr u8))) bool
    (let int_loc i16 2)
    (let ch_loc u8 0)
    (while (<= int_loc 2)
        (if (= (func_1 (load (+ str_1_par_ref (as u16 int_loc)))
                    (load (+ str_2_par_ref (as u16 (+ int_loc 1)))))
                Ident.ident_1)
            (do
                (set ch_loc 'A')
                (set int_loc (+ int_loc 1)))))
    (if (and (>= ch_loc 'W') (< ch_loc 'Z'))
        (set int_loc 7))
    (if (= ch_loc 'R')
        (return true))
    (if (> (str_compare str_1_par_ref str_2_par_ref) 0)
        (do
            (set int_loc (+ int_loc 7))
            (set int_glob int_loc)
            (return true)))
    false)

(let sum_a u16 0)
(let sum_b u16 0)

(fn fold ((v u16)) void
    (set sum_a (+ sum_a v))
    (set sum_b (+ sum_b sum_a)))

(fn fold_string ((s (ptr u8))) void
    (let i u8 0)
    (while (!= (load (+ s i)) 0)
        (fold (as u16 (load (+ s i))))
        (set i (+ i 1))))

; addr_of does not go through pointers, so the record's string is read in place
(fn fold_record ((r (ptr Rec))) void
    (fold (as u16 (as u8 (load r discr))))
    (fold (as u16 (as u8 (load r enum_comp))))
    (fold (as u16 (load r int_comp)))
    (let i u8 0)
    (while (!= (at (load r str_comp) i) 0)
        (fold (as u16 (at (load r str_comp) i)))
        (set i (+ i 1))))

(fn main () void
    (store ptr_glob ptr_comp next_ptr_glob)
    (store ptr_glob discr Ident.ident_1)
    (store ptr_glob enum_comp Ident.ident_3)
    (store ptr_glob int_comp 40)
    (str_copy (addr_of (at glob_rec.str_comp 0)) "DHRYSTONE PROGRAM, SOME STRING")
    (str_copy (addr_of (at str_1_loc 0)) "DHRYSTONE PROGRAM, 1'ST STRING")
    (set (at arr_2_glob (+ (* 8 50) 7)) 10)
    (let number_of_runs u16 runs)
    (let int_1_loc i16 0)
    (let int_2_loc i16 0)
    (let int_3_loc i16 0)
    (let enum_loc Ident Ident.ident_1)
    (for run_index u16 1 (+ number_of_runs 1)
        (proc_5)
        (proc_4)
        (set int_1_loc 2)
        (set int_2_loc 3)
        (str_copy (addr_of (at str_2_loc 0)) "DHRYSTONE PROGRAM, 2'ND STRING")
        (set enum_loc Ident.ident_2)
        (set bool_glob (not (func_2 (addr_of (at str_1_loc 0)) (addr_of (at str_2_loc 0)))))
        (while (< int_1_loc int_2_loc)
            (set int_3_loc (- (* 5 int_1_loc) int_2_loc))
            (set int_3_loc (proc_7 int_1_loc int_2_loc))
            (set int_1_loc (+ int_1_loc 1)))
        (proc_8 (addr_of (at arr_1_glob 0)) (addr_of (at arr_2_glob 0)) int_1_loc int_3_loc)
        (proc_1 ptr_glob)
        (let ch_index u8 'A')
        (while (<= ch_index ch_2_glob)
            (if (= enum_loc (func_1 ch_index 'C'))
                (do
                    (set enum_loc (proc_6 Ident.ident_1))
                    (str_copy (addr_of (at str_2_loc 0)) "DHRYSTONE PROGRAM, 3'RD STRING")
                    (set int_2_loc (as i16 run_index))
                    (set int_glob (as i16 run_index))))
            (set ch_index (+ ch_index 1)))
        (set int_2_loc (* int_2_loc int_1_loc))
        (set int_1_loc (/ int_2_loc int_3_loc))
        (set int_2_loc (- (* 7 (- int_2_loc int_3_loc)) int_1_loc))
        (proc_2 (addr_of int_1_loc)))
    (set sum_a 0)
    (set sum_b 0)
    (fold (as u16 int_glob))
    (fold (if bool_glob 1 0))
    (fold (as u16 ch_1_glob))
    (fold (as u16 ch_2_glob))
    (fold (as u16 (at arr_1_glob 8)))
    (fold (as u16 (at arr_2_glob (+ (* 8 50) 7))))
    (fold_record ptr_glob)
    (fold_record next_ptr_glob)
    (fold (as u16 int_1_loc))
    (fold (as u16 int_2_loc))
    (fold (as u16 int_3_loc))
    (fold (as u16 (as u8 enum_loc)))
    (fold_string (addr_of (at str_1_loc 0)))
    (fold_string (addr_of (at str_2_loc 0)))
    (set checksum (^ sum_a sum_b)))
