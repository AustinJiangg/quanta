#!/bin/sh
# Exercise the GDB remote stub (--gdb) end to end with a self-contained RSP
# client (tests/gdb_client.py), so no riscv `gdb` is required. The client speaks
# the same protocol gdb does — register/memory access, single-step, breakpoint,
# continue — and asserts the known outcomes of running tests/hello.elf.
#
# Skips cleanly when python3 is absent, the same "no tool, no problem" stance as
# check_diff.sh without qemu.
set -u

if ! command -v python3 >/dev/null 2>&1; then
    echo "check-gdb: python3 not found — skipping"
    echo "  (install python3 to exercise the GDB remote stub)"
    exit 0
fi

QUANTA=./quanta
ELF=tests/hello.elf

if [ ! -x "$QUANTA" ]; then
    echo "check-gdb: build quanta first (run 'make')"
    exit 1
fi
if [ ! -f "$ELF" ]; then
    echo "check-gdb: $ELF missing (run 'make tests')"
    exit 1
fi

exec python3 tests/gdb_client.py "$QUANTA" "$ELF"
