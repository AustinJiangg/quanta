#!/bin/sh
# OpenSBI firmware boot (M18 / Linux-boot enabler): boot a real M-mode firmware
# and have it hand off to an S-mode OS — the way a RISC-V machine actually boots,
# and the path Linux takes.
#
# Loads the qemu-prebuilt OpenSBI (fw_dynamic) as --bios and a small S-mode
# payload (tests/opensbi_payload.bin) as --kernel. The payload prints through the
# SBI console (an ecall serviced by OpenSBI running on Quanta) and shuts down via
# SBI SRST, which OpenSBI carries out by writing Quanta's SiFive test device — a
# clean exit 0. We assert both: the console message reached stdout, and the
# machine exited cleanly.
#
# Needs an OpenSBI fw_dynamic binary: $QUANTA_OPENSBI, or qemu's default install
# path. Skips cleanly when neither is present, like check-diff without qemu.
set -u

OSBI="${QUANTA_OPENSBI:-/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.elf}"
if [ ! -f "$OSBI" ]; then
    echo "check-opensbi: no OpenSBI firmware found — skipping"
    echo "  (set QUANTA_OPENSBI=/path/to/opensbi-...-fw_dynamic.elf, or install"
    echo "   the qemu system package that ships /usr/share/qemu/opensbi-*.elf)"
    exit 0
fi

QUANTA=./quanta
PAYLOAD=tests/opensbi_payload.bin
if [ ! -x "$QUANTA" ]; then echo "check-opensbi: build quanta first (run 'make')"; exit 1; fi
if [ ! -f "$PAYLOAD" ]; then echo "check-opensbi: $PAYLOAD missing (run 'make tests')"; exit 1; fi

rc=0
# --quiet leaves only what the guest/firmware print through the UART (OpenSBI's
# banner and our payload's line); a 20M-step cap is a safety net — a clean SRST
# shutdown exits well before it.
out=$("$QUANTA" --quiet --memory=128M --max-steps=20M \
        --bios="$OSBI" --kernel="$PAYLOAD" 2>&1)
status=$?

if printf '%s' "$out" | grep -q "reached S-mode via OpenSBI on Quanta"; then
    echo "OK    OpenSBI handed off to S-mode; payload printed via the SBI console"
else
    echo "FAIL  the payload's SBI-console message did not appear"
    echo "---- output ----"; printf '%s\n' "$out"; echo "----------------"
    rc=1
fi

if [ "$status" -eq 0 ]; then
    echo "OK    machine shut down cleanly via SBI SRST (exit 0)"
else
    echo "FAIL  expected a clean exit 0 (SBI SRST), got $status"
    rc=1
fi

if [ "$rc" -eq 0 ]; then
    echo "OpenSBI firmware boot: M-mode firmware -> S-mode payload OK"
fi
exit $rc
