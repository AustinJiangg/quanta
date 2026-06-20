#!/bin/sh
# Check the cache model two ways, using the array-traversal workload:
#
#   1. It is a pure observability layer — turning it on must not change what the
#      program computes, so the guest must still exit 0 under any geometry.
#   2. Cache geometry actually matters — a tiny, direct-mapped cache must miss
#      more often than a roomy, set-associative one on the same run.
#
# Needs ./quanta and tests/test_stack.elf (built by `make check-cache`).

QUANTA=./quanta
ELF=tests/test_stack.elf

# Echo the miss count from one run; the function's return code is the guest's
# exit status (0 means the workload's self-check passed, i.e. results unchanged).
run_misses() { # $1 = SIZE:WAYS:BLOCK
    out=$("$QUANTA" --cache="$1" "$ELF" 2>/dev/null)
    rc=$?
    echo "$out" | sed -n 's/.*misses \([0-9][0-9]*\).*/\1/p' | head -1
    return $rc
}

big=$(run_misses 1024:2:32);   rc_big=$?
small=$(run_misses 256:1:16);  rc_small=$?

fail=0
if [ "$rc_big" -ne 0 ] || [ "$rc_small" -ne 0 ]; then
    echo "FAIL  cache changed the result (exit $rc_big / $rc_small, want 0 / 0)"
    fail=1
fi
if [ -z "$big" ] || [ -z "$small" ]; then
    echo "FAIL  could not parse miss counts (big='$big' small='$small')"
    fail=1
elif [ "$small" -le "$big" ]; then
    echo "FAIL  smaller cache did not miss more (small=$small <= big=$big)"
    fail=1
else
    echo "OK    results unchanged; misses: 1024:2:32 -> $big, 256:1:16 -> $small"
fi

if [ "$fail" -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "cache model behaves as expected"
