#!/bin/sh
# tests/coverage.sh — collect and summarise gcov line coverage.
#
# Run after an instrumented build (CFLAGS += --coverage) and a suite run, so
# src/*.gcno (from compiling) and src/*.gcda (from the runs) both exist. Prefers
# lcov for an HTML report plus an accurate aggregate, and falls back to plain
# gcov for a per-file text summary when lcov is absent (e.g. a bare dev box).
#
# Reports only: it never fails on the coverage number — the test suite already
# gates correctness. The instrumentation is on the host emulator, not the guest
# ELFs, the same split as `make sanitize`.
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
out=build/coverage
mkdir -p "$out"

# Exercise a few CLI paths the conformance suite doesn't drive, so main.c's
# argument handling, the built-in demo, and the signature/error paths count too.
# Exit codes are irrelevant here — we only want the lines executed.
./quanta                                         >/dev/null 2>&1 || true  # built-in demo
./quanta --cache --pipeline tests/test_stack.elf >/dev/null 2>&1 || true  # overlays
./quanta --signature=- tests/test_op.elf         >/dev/null 2>&1 || true  # no sig syms
./quanta --bogus                                 >/dev/null 2>&1 || true  # usage path
./quanta /no/such/file                           >/dev/null 2>&1 || true  # load failure

if command -v lcov >/dev/null 2>&1; then
    info=$out/coverage.info
    # lcov 2.x is fussy about gcov-format quirks; downgrade those to warnings so
    # the report still generates.
    ign=mismatch,unused,negative,empty,source,gcov
    lcov --quiet --capture --directory src --base-directory "$root" \
         --output-file "$info" --ignore-errors "$ign" 2>/dev/null ||
        lcov --capture --directory src --output-file "$info"
    # Keep only this project's own sources in the report.
    lcov --quiet --extract "$info" "*/src/*" --output-file "$info" \
         --ignore-errors unused 2>/dev/null || true
    # Report via `lcov --summary`: its per-file `--list` table miscomputes rates
    # under lcov 2.0, while --summary (and genhtml) stay correct. The per-file
    # breakdown lives in the HTML report below.
    echo "Coverage summary:"
    lcov --summary "$info" 2>/dev/null | grep -E 'lines|functions|branches' ||
        lcov --summary "$info"
    if command -v genhtml >/dev/null 2>&1; then
        genhtml --quiet "$info" --output-directory "$out/html" \
                --ignore-errors source >/dev/null 2>&1 &&
            echo "HTML report: $out/html/index.html"
    fi
else
    echo "lcov not installed — using gcov for a per-file summary"
    echo "(install lcov for an HTML report and an accurate aggregate)"
    echo
    ( cd "$out" && gcov -o "$root/src" "$root"/src/*.c ) >"$out/gcov.log" 2>&1 || true
    # Pull a tidy per-file table out of gcov's chatter. gcov prints "File 'X'"
    # then "Lines executed:..." for each source (merging a shared header to one
    # entry), then a trailing "Lines executed:" with no "File" line — its grand
    # total over everything. Print each file, and surface that total verbatim.
    awk '
        /^File/ {
            f = $2; gsub(/'\''/, "", f); sub(/.*\/src\//, "src/", f);
            pend = 1; next
        }
        /^Lines executed:/ {
            v = $0; sub(/Lines executed:/, "", v);
            if (pend) { printf "  %-20s %s\n", f, v; pend = 0 }
            else      { total = v }
        }
        END { if (total != "") printf "  %-20s %s\n", "TOTAL", total }
    ' "$out/gcov.log"
fi
