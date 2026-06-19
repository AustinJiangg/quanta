/*
 * A minimal bare-metal RV32I program, mirroring the built-in demo in
 * src/main.c. Quanta can load ELF files now: build this with the cross-
 * toolchain and run it with `./quanta hello.elf` instead of the hardcoded
 * words. It computes a couple of values, then terminates with the exit
 * syscall (a7 = 93).
 *
 * Build (in WSL2, with gcc-riscv64-unknown-elf installed):
 *
 *   riscv64-unknown-elf-gcc -march=rv32i -mabi=ilp32 -nostdlib -nostartfiles \
 *       -Ttext=0x80000000 -o hello.elf tests/hello.S
 *
 * Inspect what you built:
 *
 *   riscv64-unknown-elf-objdump -d hello.elf
 */

    .section .text
    .globl _start
_start:
    addi a0, zero, 5     /* a0 = 5                    */
    addi a1, zero, 37    /* a1 = 37                   */
    add  a2, a0, a1      /* a2 = 42                   */
    sub  a3, a1, a0      /* a3 = 32                   */
    li   a7, 93          /* a7 = exit syscall         */
    li   a0, 0           /* exit status 0 (reuses a0) */
    ecall                /* exit(0)                   */
