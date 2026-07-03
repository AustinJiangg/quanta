#!/bin/sh
# UART receive + --disk backend (M18 xv6 enablers).
#
# UART RX: pipe a known line into the guest, which echoes every byte back through
# the UART transmit register; assert the echo matches — proving host stdin reaches
# the guest's UART receive path and drives the RX side of the console.
#
# --disk: assert a raw image attaches (the run completes) and that a missing image
# is reported as an error rather than silently ignored. The functional block-device
# test arrives with the virtio-mmio device; this pins the CLI/loader plumbing.

QUANTA=./quanta
ELF=tests/uart_echo.elf
rc=0

# --- UART RX echo ---
line="hello uart"
got=$(printf '%s\n' "$line" | "$QUANTA" --quiet "$ELF" 2>/dev/null)
if [ "$got" = "$line" ]; then
    echo "OK    UART RX: guest echoed '$line'"
else
    echo "FAIL  UART RX: sent '$line', got '$got'"
    rc=1
fi

# --- --disk backend: an existing image attaches ---
disk=$(mktemp)
printf 'QUANTA-DISK-IMAGE' > "$disk"
if printf '\n' | "$QUANTA" --quiet --disk="$disk" "$ELF" >/dev/null 2>&1; then
    echo "OK    --disk: image attached and guest ran"
else
    echo "FAIL  --disk: attaching '$disk' failed"
    rc=1
fi
rm -f "$disk"

# --- --disk backend: a missing image is an error ---
if "$QUANTA" --quiet --disk=/nonexistent/quanta-xyz "$ELF" </dev/null >/dev/null 2>&1; then
    echo "FAIL  --disk: a missing image should error"
    rc=1
else
    echo "OK    --disk: missing image reported"
fi

if [ "$rc" -eq 0 ]; then
    echo "console I/O: UART RX and --disk OK"
fi
exit $rc
