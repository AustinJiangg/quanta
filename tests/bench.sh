#!/bin/sh
# M25a/M25b: time the interpreter (decode cache off / on) and the basic-block
# JIT on the long compute loop tests/bench.elf, and report the speedups. Uses
# best-of-N user CPU time (steadier than wall clock under load). Observability
# only — not a pass/fail check (make bench), so it never fails the build.

set -u
BIN=./quanta
ELF=tests/bench.elf
RUNS=3

# Best (minimum) user CPU time of RUNS runs of "$BIN $extra_flags $ELF", in
# seconds. Falls back cleanly if /usr/bin/time is missing.
best_user() {
    flags="$1"
    if ! command -v /usr/bin/time >/dev/null 2>&1; then
        echo "n/a"; return
    fi
    best=""
    i=0
    while [ "$i" -lt "$RUNS" ]; do
        t=$(/usr/bin/time -v $BIN $flags --max-steps=0 "$ELF" 2>&1 \
                | awk '/User time/{print $4}')
        [ -n "$t" ] || { echo "n/a"; return; }
        if [ -z "$best" ] || awk "BEGIN{exit !($t < $best)}"; then best=$t; fi
        i=$((i + 1))
    done
    echo "$best"
}

echo "Timing $ELF (best user-time of $RUNS runs each)..."
off=$(best_user "--no-dcache")
on=$(best_user "")
jit=$(best_user "--jit")
echo "  decode cache OFF : ${off} s"
echo "  decode cache ON  : ${on} s"
echo "  JIT              : ${jit} s"
if [ "$off" != "n/a" ] && [ "$on" != "n/a" ]; then
    awk "BEGIN{ if ($on>0) printf \"  dcache speedup   : %.2fx\n\", $off/$on }"
fi
if [ "$on" != "n/a" ] && [ "$jit" != "n/a" ]; then
    awk "BEGIN{ if ($jit>0) printf \"  jit speedup      : %.2fx over the interpreter\n\", $on/$jit }"
fi
