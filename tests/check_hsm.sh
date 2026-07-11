#!/bin/sh
# SBI HSM hart state management (M22): boot the HSM test on four harts and confirm
# the whole machine reaches a clean exit 0.
#
# The guest (tests/rv64/test_rv64_hsm.S) runs all four harts from the entry, drops
# each to S-mode with no mtvec (so their ecalls reach Quanta's built-in SBI), and
# has hart 0 drive the SBI HSM extension: it waits for its three secondaries to
# park themselves with hart_stop, restarts each at a worker via hart_start (which
# increments a shared counter and stops again), and verifies the counter, the
# hart_get_status transitions, and the two error cases (an out-of-range hartid and
# an already-started hart). A clean pass powers the machine off via the SiFive
# test finisher.
#
# Exit 0 means every stage passed; a non-zero code is the failing stage id
# (16 hartid .. 23 already-available). Quanta-as-firmware + multi-hart + SBI, so
# it is quanta-only (out of the qemu-riscv64 differential). Needs the
# cross-toolchain to build the ELF, like the other multi-hart checks.
set -u

QUANTA=./quanta
ELF=tests/rv64/test_rv64_hsm.elf

if [ ! -x "$QUANTA" ]; then
    echo "check-hsm: build quanta first (run 'make')"
    exit 1
fi
if [ ! -f "$ELF" ]; then
    echo "check-hsm: $ELF missing (run 'make tests')"
    exit 1
fi

# --harts=4 must match NHARTS in the test (it drives harts 1..3 as secondaries).
"$QUANTA" --quiet --harts=4 "$ELF" </dev/null >/dev/null 2>&1
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "check-hsm: FAILED — $ELF exited $rc (HSM stage $rc failed)"
    exit 1
fi

echo "OK    3 secondaries parked themselves via SBI hart_stop"
echo "OK    hart 0 restarted each via hart_start; the worker counter reached NHARTS-1"
echo "OK    hart_get_status reported STARTED/STOPPED; the error cases returned right codes"
echo "SBI HSM behaves as expected"
