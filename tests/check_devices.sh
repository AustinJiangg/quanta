#!/bin/sh
# Pin the M13 platform: run the device/interrupt test and confirm both halves of
# its "done when" — the interrupt assertions pass (a clean exit 0, covering the
# CLINT timer, the software IPI, and an external interrupt routed through the
# PLIC) and the string it writes to the 16550 UART transmit register actually
# reaches stdout (the MMIO console path).
#
# Builds need the cross-toolchain (like check-cache/check-pipeline); the test is
# machine-mode, so it stays out of check-diff and is pinned here and by make check.
set -u

QUANTA=./quanta
ELF=tests/test_irq.elf

if [ ! -x "$QUANTA" ]; then
    echo "check-devices: build quanta first (run 'make')"
    exit 1
fi
if [ ! -f "$ELF" ]; then
    echo "check-devices: $ELF missing (run 'make tests')"
    exit 1
fi

out=$("$QUANTA" --quiet "$ELF" 2>/dev/null); rc=$?
if [ "$rc" -ne 0 ]; then
    echo "check-devices: FAILED — $ELF exited $rc (interrupt check $rc failed)"
    exit 1
fi
if ! printf '%s' "$out" | grep -q 'uart ok'; then
    echo "check-devices: FAILED — UART console output not seen on stdout"
    echo "  got: '$out'"
    exit 1
fi

echo "OK    CLINT timer, software IPI, and PLIC external interrupts all fired"
echo "OK    16550 UART console output reached stdout"
echo "device model behaves as expected"
