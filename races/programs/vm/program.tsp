; A bytecode interpreter: a 16-bit stack machine with 16 variables, a byte memory and a
; call stack, the loop a match on the opcode. The program in code (assembled by spec.py)
; bubble-sorts, takes gcds, makes Fibonacci numbers, hashes and sieves mem, and outputs
; bytes; the checksum folds the variables, mem, the output and the step count.

; the opcodes
(const HALT u8 0)
(const PUSH8 u8 1)
(const PUSH16 u8 2)
(const LOAD u8 3)
(const STORE u8 4)
(const ADD u8 5)
(const SUB u8 6)
(const AND u8 7)
(const OR u8 8)
(const XOR u8 9)
(const SHL1 u8 10)
(const SHR1 u8 11)
(const LT u8 12)
(const EQ u8 13)
(const JMP u8 14)
(const JZ u8 15)
(const JNZ u8 16)
(const DUP u8 17)
(const DROP u8 18)
(const SWAP u8 19)
(const LOADA u8 20)
(const STOREA u8 21)
(const CALL u8 22)
(const RET u8 23)
(const INC u8 24)
(const OUT u8 25)

(const STACK u8 32)
(const CALLS u8 16)

(let code (array u8 1024) :address 0x2000)
(let mem (array u8 256) :address 0x3000)
(let out_buf (array u8 256) :address 0x3100)
(let checksum u16 :address 0x5F00)

(let stack (array u16 STACK))
(let sp u8 0)
(let calls (array u16 CALLS))
(let csp u8 0)
(let vars (array u16 16))
(let pc u16 0)
(let steps u16 0)
(let out_len u8 0)

(fn fetch8 () u8
    (let v u8 (at code pc))
    (set pc (+ pc 1))
    v)

(fn fetch16 () u16
    (let v u16 (| (as u16 (at code pc)) (<< (as u16 (at code (+ pc 1))) 8)))
    (set pc (+ pc 2))
    v)

(fn push ((v u16)) void
    (set (at stack sp) v)
    (set sp (+ sp 1)))

(fn pop () u16
    (set sp (- sp 1))
    (at stack sp))

; the top two replaced by one: sp moves down, top is the new top's place
(fn top () u8
    (- sp 1))

(fn run () void
    (set pc 0)
    (set sp 0)
    (set csp 0)
    (set steps 0)
    (while true
        (let op u8 (fetch8))
        (set steps (+ steps 1))
        (match op
            (HALT (return))
            (PUSH8 (push (as u16 (fetch8))))
            (PUSH16 (push (fetch16)))
            (LOAD (push (at vars (fetch8))))
            (STORE (set (at vars (fetch8)) (pop)))
            (ADD
                (let b u16 (pop))
                (set (at stack (top)) (+ (at stack (top)) b)))
            (SUB
                (let b u16 (pop))
                (set (at stack (top)) (- (at stack (top)) b)))
            (AND
                (let b u16 (pop))
                (set (at stack (top)) (& (at stack (top)) b)))
            (OR
                (let b u16 (pop))
                (set (at stack (top)) (| (at stack (top)) b)))
            (XOR
                (let b u16 (pop))
                (set (at stack (top)) (^ (at stack (top)) b)))
            (SHL1 (set (at stack (top)) (<< (at stack (top)) 1)))
            (SHR1 (set (at stack (top)) (>> (at stack (top)) 1)))
            (LT
                (let b u16 (pop))
                (set (at stack (top)) (if (< (at stack (top)) b)
                    1
                    0)))
            (EQ
                (let b u16 (pop))
                (set (at stack (top)) (if (= (at stack (top)) b)
                    1
                    0)))
            (JMP (set pc (fetch16)))
            (JZ
                (let to u16 (fetch16))
                (if (= (pop) 0)
                    (set pc to)))
            (JNZ
                (let to u16 (fetch16))
                (if (!= (pop) 0)
                    (set pc to)))
            (DUP (push (at stack (top))))
            (DROP (set sp (- sp 1)))
            (SWAP
                (let a u16 (at stack (- sp 1)))
                (set (at stack (- sp 1)) (at stack (- sp 2)))
                (set (at stack (- sp 2)) a))
            (LOADA (set (at stack (top)) (as u16 (at mem (as u8 (at stack (top)))))))
            (STOREA
                (let b u16 (pop))
                (let a u16 (pop))
                (set (at mem (as u8 a)) (as u8 b)))
            (CALL
                (let to u16 (fetch16))
                (set (at calls csp) pc)
                (set csp (+ csp 1))
                (set pc to))
            (RET
                (set csp (- csp 1))
                (set pc (at calls csp)))
            (INC
                (let v u8 (fetch8))
                (set (at vars v) (+ (at vars v) 1)))
            (OUT
                (set (at out_buf out_len) (as u8 (pop)))
                (set out_len (+ out_len 1)))
            (_))))

(let sum_a u16 0)
(let sum_b u16 0)

(fn fold ((v u16)) void
    (set sum_a (+ sum_a v))
    (set sum_b (+ sum_b sum_a)))

(fn main () void
    (set out_len 0)
    (for i u8 0 16
        (set (at vars i) 0))
    (run)
    (set sum_a 0)
    (set sum_b 0)
    (for i u8 0 16
        (fold (at vars i)))
    (for i u8 0 256
        (fold (as u16 (at mem i))))
    (for i u8 0 out_len
        (fold (as u16 (at out_buf i))))
    (fold (as u16 out_len))
    (fold steps)
    (fold (as u16 sp))
    (set checksum (^ sum_a sum_b)))
