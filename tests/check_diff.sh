#!/bin/sh
# Differential test: run each ELF under Quanta and a reference RISC-V simulator
# (a "golden model"), and assert they agree on the guest's stdout and exit code.
#
# Quanta runs with --quiet so its stdout is only the guest's write() output,
# directly comparable to the reference. The exit code matters too: quanta
# propagates the guest's exit status, as does the reference, so the conformance
# programs (which exit 0 on pass, non-zero on the first failed check) cross-check
# Quanta against the reference for free.
#
# The reference defaults to qemu-riscv32-static and is overridable via $REF — set
# it to a spike+pk wrapper, or any sim that runs these ELFs with the same syscall
# ABI. If the reference is not installed, the test skips cleanly so `make
# check-diff` is a no-op rather than a failure on machines without it.
#
# Usage: sh tests/check_diff.sh [elf...]   (defaults to all built tests/*.elf)

QUANTA=./quanta
REF="${REF:-qemu-riscv32-static}"

if ! command -v "$REF" >/dev/null 2>&1; then
    echo "check-diff: reference simulator '$REF' not found — skipping"
    echo "  (install qemu-user-static, or set REF to a spike/pk wrapper)"
    exit 0
fi

if [ "$#" -gt 0 ]; then
    elves="$*"
else
    elves=$(ls tests/*.elf 2>/dev/null)
fi
if [ -z "$elves" ]; then
    echo "check-diff: no ELF files (run 'make tests' first)"
    exit 1
fi

fail=0
count=0
for elf in $elves; do
    count=$((count + 1))
    q_out=$("$QUANTA" --quiet "$elf" 2>/dev/null); q_rc=$?
    r_out=$("$REF" "$elf" 2>/dev/null); r_rc=$?
    if [ "$q_rc" = "$r_rc" ] && [ "$q_out" = "$r_out" ]; then
        printf "OK    %-26s rc=%s\n" "$(basename "$elf")" "$q_rc"
    else
        printf "DIFF  %-26s quanta(rc=%s) vs %s(rc=%s)\n" \
            "$(basename "$elf")" "$q_rc" "$REF" "$r_rc"
        if [ "$q_out" != "$r_out" ]; then
            echo "  --- quanta stdout ---"; printf '%s\n' "$q_out" | sed 's/^/  /'
            echo "  --- $REF stdout ---";   printf '%s\n' "$r_out" | sed 's/^/  /'
        fi
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "check-diff: FAILED — quanta and $REF disagree"
    exit 1
fi
echo "quanta matches $REF on all $count program(s)"
