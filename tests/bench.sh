#!/bin/sh
# M25a: time the interpreter with the decoded-instruction cache on vs off on the
# long compute loop tests/bench.elf, and report the speedup. Uses best-of-N user
# CPU time (steadier than wall clock under load). Observability only — not a
# pass/fail check (make bench), so it never fails the build.

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
echo "  decode cache OFF : ${off} s"
echo "  decode cache ON  : ${on} s"
if [ "$off" != "n/a" ] && [ "$on" != "n/a" ]; then
    awk "BEGIN{ if ($on>0) printf \"  speedup          : %.2fx\n\", $off/$on }"
fi
