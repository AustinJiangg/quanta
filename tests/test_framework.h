/*
 * Minimal RV32I conformance-test framework.
 *
 * Each test program is structured as:
 *
 *     #include "test_framework.h"
 *         TEST_START
 *         ... compute a value, then  CHECK <id>, <reg>, <expected> ...
 *         TEST_END
 *
 * gp (x3) holds the id of the current check. On the first mismatch the program
 * exits with that id as its status; quanta propagates it as the process exit
 * code, so `make check` can pinpoint which check in which file failed. If every
 * check passes, the program exits 0.
 *
 * Framework scratch registers: gp (x3) and t6 (x31). A test must not expect
 * either to survive across a CHECK.
 */

    .equ SYS_exit, 93

    /* Begin a test: PC starts here (entry = _start). */
    .macro TEST_START
    .section .text
    .globl _start
_start:
    li   gp, 0
    .endm

    /* Assert that register \reg holds \val; otherwise exit with id \id. */
    .macro CHECK id, reg, val
    li   gp, \id
    li   t6, \val
    bne  \reg, t6, _fail
    .endm

    /* All checks passed -> exit(0). The fail path exits with the check id. */
    .macro TEST_END
    li   a0, 0
    li   a7, SYS_exit
    ecall
_fail:
    mv   a0, gp
    li   a7, SYS_exit
    ecall
    .endm
