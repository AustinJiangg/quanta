# M25a performance benchmark — a long, CPU-bound RV64IMAC compute loop used by
# `make bench` to time the interpreter with the decoded-instruction cache on vs
# off. The loop body deliberately mixes compressed and 32-bit instructions (the
# assembler emits c.* for add/andi/addi under -march=rv64imac) so the run
# exercises the RVC expansion the cache memoises. It is not a conformance test
# (it is filtered out of the suites); it just runs 20M iterations of an xorshift/
# LCG mix and exits with the low byte of the accumulator.

    .section .text
    .globl _start
_start:
    li   a0, 0                 # accumulator
    li   t0, 0                 # iteration counter i
    li   t1, 20000000          # iterations
    li   t2, 1103515245        # LCG multiplier
    li   t3, 12345             # LCG increment
loop:
    mul  a0, a0, t2            # acc *= A            (RV64M)
    add  a0, a0, t3            # acc += C
    slli t4, a0, 13            # xorshift mix
    xor  a0, a0, t4
    srli t4, a0, 7
    xor  a0, a0, t4
    add  a0, a0, t0            # fold in i
    andi a0, a0, -1            # keep 64-bit (compresses)
    addi t0, t0, 1             # i++              (compresses)
    bltu t0, t1, loop          # hot taken branch
    andi a0, a0, 0xff          # exit status = low byte
    li   a7, 93                # exit syscall
    ecall
