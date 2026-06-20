#!/bin/sh
# Demonstrate the pipeline model's headline result: scheduling code to avoid a
# load-use hazard lowers both the stall count and the cycle estimate, while
# computing exactly the same thing. hazard_slow.S and hazard_fast.S are the same
# array sum; they differ only in the order of the inner-loop instructions.
#
# Needs ./quanta and the two hazard ELFs (built by `make check-pipeline`).

QUANTA=./quanta
SLOW=tests/hazard_slow.elf
FAST=tests/hazard_fast.elf

# Pull "<label> <number>" out of a report blob.
field() { echo "$1" | sed -n "s/.*$2 \([0-9][0-9]*\).*/\1/p" | head -1; }

slow=$("$QUANTA" --pipeline "$SLOW" 2>/dev/null); rc_s=$?
fast=$("$QUANTA" --pipeline "$FAST" 2>/dev/null); rc_f=$?

s_lu=$(field "$slow" "load-use"); s_cy=$(field "$slow" "cycles")
f_lu=$(field "$fast" "load-use"); f_cy=$(field "$fast" "cycles")

fail=0
if [ "$rc_s" -ne 0 ] || [ "$rc_f" -ne 0 ]; then
    echo "FAIL  a version computed the wrong result (exit $rc_s / $rc_f, want 0 / 0)"
    fail=1
fi
if [ -z "$s_lu" ] || [ -z "$f_lu" ] || [ -z "$s_cy" ] || [ -z "$f_cy" ]; then
    echo "FAIL  could not parse the pipeline report (slow='$s_lu/$s_cy' fast='$f_lu/$f_cy')"
    fail=1
elif [ "$f_lu" -ge "$s_lu" ] || [ "$f_cy" -ge "$s_cy" ]; then
    echo "FAIL  reordering did not help (load-use $s_lu->$f_lu, cycles $s_cy->$f_cy)"
    fail=1
else
    echo "OK    reorder cut load-use stalls $s_lu -> $f_lu and cycles $s_cy -> $f_cy"
fi

if [ "$fail" -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "pipeline model behaves as expected"
