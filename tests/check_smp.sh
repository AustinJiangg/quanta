#!/bin/sh
# SMP multi-hart (M19): boot the multi-hart test on four harts and confirm the
# whole machine reaches a clean exit 0.
#
# The guest (tests/rv64/test_rv64_smp.S) runs all four harts from the entry and
# has each one: check its mhartid matches its boot a0, do 500 LR/SC increments of
# a single shared counter (the round-robin scheduler makes sibling stores break a
# hart's reservation mid-sequence, so sc.d retries — yet the total must be exactly
# 4*500 with no lost updates), and join a barrier. Hart 0 then verifies the total
# and sends hart 1 a CLINT IPI, taken as a real machine software interrupt; only
# once hart 1 acknowledges does hart 0 power the machine off cleanly.
#
# Exit 0 means every stage passed on every hart; a non-zero code is the failing
# stage id (16 hartid, 17 counter, 18 IPI). Machine-mode + multi-hart + MMIO, so
# it is quanta-only (out of the qemu-riscv64 differential). Needs the
# cross-toolchain to build the ELF, like the other device checks.
set -u

QUANTA=./quanta
ELF=tests/rv64/test_rv64_smp.elf

if [ ! -x "$QUANTA" ]; then
    echo "check-smp: build quanta first (run 'make')"
    exit 1
fi
if [ ! -f "$ELF" ]; then
    echo "check-smp: $ELF missing (run 'make tests')"
    exit 1
fi

# --harts=4 must match NHARTS in the test (its barrier waits for exactly four).
"$QUANTA" --quiet --harts=4 "$ELF" </dev/null >/dev/null 2>&1
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "check-smp: FAILED — $ELF exited $rc (SMP stage $rc failed on some hart)"
    exit 1
fi

echo "OK    4 harts booted, each with its own mhartid"
echo "OK    contended LR/SC counter reached NHARTS*ITERS (no lost updates)"
echo "OK    CLINT IPI delivered hart-to-hart as a machine software interrupt"
echo "SMP multi-hart behaves as expected"
