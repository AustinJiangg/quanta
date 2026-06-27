#!/bin/sh
# tests/analyze.sh — static analysis over the emulator sources.
#
# Runs whatever analyzers are installed; each skips cleanly when its tool is
# absent (the same "no tool, no problem" stance as check_diff.sh without qemu),
# so a bare dev box can still `make analyze` and a CI box runs the full set.
# Returns non-zero if any analyzer reports an issue, so CI gates on it.
#
# Baselines live next to the project, not in flags:
#   tests/cppcheck-suppress.txt  — cppcheck suppressions
#   .clang-tidy                  — clang-tidy check list + warnings-as-errors
#
# scan-build (the clang static analyzer driving a real build) is run separately
# in CI, where it wraps `make`; see .github/workflows/ci.yml.
set -u

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
ran=0
rc=0

if command -v cppcheck >/dev/null 2>&1; then
    ran=1
    echo "== cppcheck =="
    cppcheck --std=c11 --language=c \
        --enable=warning,performance,portability \
        --inline-suppr --suppress=unmatchedSuppression \
        --suppressions-list=tests/cppcheck-suppress.txt \
        --error-exitcode=1 --quiet \
        -I src src || rc=1
else
    echo "cppcheck not installed — skipping"
fi

if command -v clang-tidy >/dev/null 2>&1; then
    ran=1
    echo "== clang-tidy =="
    # Checks and warnings-as-errors come from .clang-tidy; the flags after --
    # are the compile options (stdlib-only, so -Isrc is all it needs).
    clang-tidy --quiet src/*.c -- -std=c11 -Isrc || rc=1
else
    echo "clang-tidy not installed — skipping"
fi

if [ "$ran" -eq 0 ]; then
    echo "No static-analysis tools installed; nothing to do."
    echo "(install cppcheck and clang-tidy, or rely on the CI static-analysis job)"
fi
exit $rc
