/* UART receive test: echo host stdin back through the UART.
 *
 * Polls the 16550 line-status register for received-data-ready, reads each byte
 * from the receive register, and writes it straight back to the transmit
 * register (which the model prints to the host stdout) — until it sees a newline,
 * then exit(0). Driven by tests/check_uart_rx.sh, which pipes a known line in and
 * checks the echo, proving host stdin reaches the guest's UART RX path.
 *
 * Because it needs host input to terminate, it is deliberately not named test_*
 * (so `make check` never runs it input-less) and is filtered out of the
 * objdump/qemu suites (whose tools do not model the UART MMIO). Plain rv32i. */
    .equ UART,     0x10000000     /* 16550 base (qemu virt) */
    .equ SYS_exit, 93

    .section .text
    .global _start
_start:
    li    t0, UART
1:  lbu   t1, 5(t0)              /* LSR: bit 0 = receive data ready */
    andi  t1, t1, 1
    beqz  t1, 1b                 /* spin until a byte has arrived */
    lbu   t2, 0(t0)              /* RBR: read it (clears data-ready) */
    sb    t2, 0(t0)              /* THR: echo it to the host console */
    li    t3, 10                 /* stop at '\n' */
    bne   t2, t3, 1b
    li    a0, 0
    li    a7, SYS_exit
    ecall
