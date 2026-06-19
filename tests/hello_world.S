/*
 * "Hello, world!" through system calls — the M2 milestone program.
 *
 * Where tests/hello.S only computes values, this program reaches the outside
 * world: it uses the RISC-V Linux/newlib syscall ABI to write a string to
 * stdout, then exits cleanly. The syscall number goes in a7, arguments in
 * a0..a2, and `ecall` performs the call.
 *
 * Build:
 *   riscv64-unknown-elf-gcc -march=rv32i -mabi=ilp32 -nostdlib -nostartfiles \
 *       -Ttext=0x80000000 -o hello_world.elf tests/hello_world.S
 *
 * Run:
 *   ./quanta tests/hello_world.elf      # prints "Hello, world!" and exits 0
 */

    .equ SYS_write, 64
    .equ SYS_exit,  93
    .equ STDOUT,    1

    .section .rodata
msg:
    .ascii "Hello, world!\n"
    .set   msg_len, . - msg

    .section .text
    .globl _start
_start:
    li   a7, SYS_write      /* syscall number: write  */
    li   a0, STDOUT         /* arg0: fd = stdout       */
    la   a1, msg            /* arg1: buffer address    */
    li   a2, msg_len        /* arg2: byte count        */
    ecall                   /* write(1, msg, msg_len)  */

    li   a7, SYS_exit       /* syscall number: exit    */
    li   a0, 0              /* arg0: status = 0        */
    ecall                   /* exit(0)                 */
