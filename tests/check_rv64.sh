#!/bin/sh
# RV64 conformance (M17): run each tests/rv64 program under quanta and confirm it
# exits 0 — meaning every one of its CHECKs passed. The user-mode programs are
# additionally cross-checked against qemu-riscv64, the golden-model safety net for
# the RV64 ISA (the RV64 analogue of `make check-diff`): quanta and qemu must
# agree on stdout and exit code. The privileged programs (*_priv, *_vm) use
# machine-mode CSRs, traps, and Sv39 paging that user-mode qemu rejects, so they
# are quanta-only, exactly like the RV32 privileged tests are excluded from
# check-diff.
#
# Skips the qemu differential cleanly if qemu-riscv64 is not installed.

QUANTA=./quanta
QEMU="${REF64:-qemu-riscv64-static}"
have_qemu=0
command -v "$QEMU" >/dev/null 2>&1 && have_qemu=1

n=0
rc=0
for elf in "$@"; do
    n=$((n + 1))
    "$QUANTA" --quiet "$elf" >/dev/null 2>&1
    q=$?
    if [ "$q" -ne 0 ]; then
        echo "FAIL  $elf (quanta exit $q — check $q failed)"
        rc=1
        continue
    fi

    case "$elf" in
    *_priv.elf|*_vm.elf)
        echo "OK    $elf (quanta exit 0; supervisor/paging, no qemu differential)"
        ;;
    *)
        if [ "$have_qemu" -eq 1 ]; then
            qout=$("$QUANTA" --quiet "$elf" 2>/dev/null)
            eout=$("$QEMU" "$elf" 2>/dev/null)
            e=$?
            if [ "$e" -eq 0 ] && [ "$qout" = "$eout" ]; then
                echo "OK    $elf (quanta == qemu-riscv64)"
            else
                echo "DIFF  $elf (qemu exit=$e; stdout differs)"
                rc=1
            fi
        else
            echo "OK    $elf (quanta exit 0; qemu-riscv64 absent, differential skipped)"
        fi
        ;;
    esac
done

if [ "$rc" -ne 0 ]; then
    echo "FAILED"
    exit 1
fi
echo "RV64 conformance: all $n program(s) passed"
