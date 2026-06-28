#!/bin/sh
# Pin the M15 SBI: run the bare-metal S-mode test and confirm both halves of its
# "done when" — a clean exit 0 (the S-mode program dropped out of Machine mode,
# made its SBI calls, and shut the machine down via SRST system_reset) and the
# string it printed through the SBI console (putchar) actually reaching stdout.
#
# Builds need the cross-toolchain (like check-devices); the test runs in S-mode
# and calls Quanta's SBI, so it stays out of check-diff and is pinned here and by
# make check.
set -u

QUANTA=./quanta
ELF=tests/test_sbi.elf

if [ ! -x "$QUANTA" ]; then
    echo "check-sbi: build quanta first (run 'make')"
    exit 1
fi
if [ ! -f "$ELF" ]; then
    echo "check-sbi: $ELF missing (run 'make tests')"
    exit 1
fi

out=$("$QUANTA" --quiet "$ELF" 2>/dev/null); rc=$?
if [ "$rc" -ne 0 ]; then
    echo "check-sbi: FAILED — $ELF exited $rc (an SBI call returned an error)"
    exit 1
fi
if ! printf '%s' "$out" | grep -q 'sbi ok'; then
    echo "check-sbi: FAILED — SBI console output not seen on stdout"
    echo "  got: '$out'"
    exit 1
fi

echo "OK    S-mode program ran via the SBI and shut down cleanly (exit 0)"
echo "OK    SBI console output reached stdout"
echo "sbi model behaves as expected"
