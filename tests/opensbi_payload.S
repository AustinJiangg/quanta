/* OpenSBI firmware-boot test payload (M18 / Linux-boot enabler).
 *
 * A minimal S-mode "OS" that a real M-mode firmware (OpenSBI, built fw_dynamic)
 * hands off to — the same boot path a Linux Image takes. It is loaded as a raw
 * binary at 0x80200000 (the qemu 'virt' kernel address), entered in S-mode with
 * a0 = hartid and a1 = DTB. It prints a line through the SBI console (an ecall
 * back into the firmware, which runs on Quanta) and then shuts the machine down
 * through SBI SRST — which OpenSBI services by writing Quanta's SiFive test
 * device, giving a clean exit 0. tests/check_opensbi.sh drives it under
 * `quanta --bios <opensbi> --kernel opensbi_payload.bin`.
 *
 * Built rv64 and -mno-relax so `la` stays PC-relative (gp is 0 here), like the
 * other in-RAM-address tests. Position-dependent, linked at 0x80200000. */
    .equ SBI_CONSOLE_PUTCHAR, 1              /* legacy console putchar (a7) */
    .equ SBI_EXT_SRST,        0x53525354     /* "SRST" system reset extension */

    .section .text
    .global _start
_start:
    la      a3, msg
1:  lbu     a0, 0(a3)                        /* next byte of the message */
    beqz    a0, 2f
    li      a7, SBI_CONSOLE_PUTCHAR
    ecall                                    /* SBI console putchar (a0) */
    addi    a3, a3, 1
    j       1b

2:  li      a7, SBI_EXT_SRST                 /* SBI system_reset(...) */
    li      a6, 0                            /* fid 0: system_reset */
    li      a0, 0                            /* type 0: shutdown */
    li      a1, 0                            /* reason 0 */
    ecall
3:  wfi                                      /* if reset is unavailable, idle */
    j       3b

    .section .rodata
msg:
    .asciz  "\nreached S-mode via OpenSBI on Quanta\n"
