# Self-modifying code + FENCE.I — the decoded-instruction cache's invalidation
# test (M25a). The guest executes a patchable instruction, overwrites it with a
# different one, issues FENCE.I, and re-executes it: it must observe the *new*
# instruction. That is exactly the RISC-V Zifencei contract, and it is the one
# path that proves cpu_step() flushes the decode cache on FENCE.I — with the
# cache on but no flush, the second execution would run the stale cached decode
# and the guest would fail. It is plain user-mode integer code (writes to its own
# .text, then FENCE.I), so both the plain interpreter and the decode cache — and
# real hardware / qemu — must reach the same result.
#
# Built -march=rv32i_zifencei with no compressed extension, so `site` and the
# replacement instruction are both fixed 4-byte `ori`s with identical encoding
# shape (the store overwrites one valid instruction with another). Exits 0 on
# success, 1 on failure. Driven by the on/off differential (make check-dcache),
# and deliberately kept out of check / check-disasm / check-diff (its runtime
# self-modification would not match the static objdump).

    .section .text
    .globl _start
_start:
    li      a0, 0                 # accumulator
    li      t1, 0                 # pass counter (0 = first pass)

loop_top:
site:
    ori     a0, a0, 0x100         # PATCHABLE: sets bit 8; patched to set bit 9
    bnez    t1, done              # second pass: skip the patch, go check
    li      t1, 1                 # mark that we have patched
    la      t2, site              # address of the instruction to overwrite
    la      t3, patched_insn      # address of the replacement instruction word
    lw      t3, 0(t3)             # load its 32-bit encoding
    sw      t3, 0(t2)             # overwrite `site` in instruction memory
    fence.i                       # make the new instruction visible to fetch
    j       loop_top              # re-execute `site` (now the patched form)

done:
    li      t4, 0x300             # expect bit 8 (first pass) | bit 9 (patched)
    bne     a0, t4, fail
    li      a0, 0                 # success
    j       exit
fail:
    li      a0, 1                 # the patched instruction did not take effect
exit:
    li      a7, 93                # exit syscall
    ecall

    # The replacement instruction, sitting past the ecall so it is never executed
    # in place — only copied over `site` as data. Same `ori a0, a0, imm` shape.
patched_insn:
    ori     a0, a0, 0x200         # sets bit 9
