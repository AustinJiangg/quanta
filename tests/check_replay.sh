#!/bin/sh
# Pin the E10 snapshot serialisation (--snapshot / --restore): a run split by a
# mid-run snapshot must resume to exactly the same result as the whole run, and a
# saved machine must round-trip through a file. Because the scheduler is
# deterministic, --restore reproduces the tail bit-for-bit.
#
# Drives the CLI only (the C-level primitive is pinned by check-snapshot). Needs
# the cross-toolchain for the guest ELFs, like the other console/device checks.
set -u

QUANTA=./quanta
HELLO=tests/hello_world.elf   # prints a string via write, then exits 0
STACK=tests/test_stack.elf    # a silent compute workload (~4391 instructions)

if [ ! -x "$QUANTA" ]; then
    echo "check-replay: build quanta first (run 'make')"
    exit 1
fi
for f in "$HELLO" "$STACK"; do
    if [ ! -f "$f" ]; then
        echo "check-replay: $f missing (run 'make tests')"
        exit 1
    fi
done

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT INT TERM
fail=0

# 1. Mid-run snapshot on a printing guest: snapshot before the write syscall, so
#    the resumed half must produce the whole output and the same exit code.
"$QUANTA" --quiet "$HELLO" >"$tmp/full.out" 2>/dev/null; full_rc=$?
"$QUANTA" --quiet --max-steps=4 --snapshot="$tmp/mid.snap" "$HELLO" \
    >"$tmp/p1.out" 2>/dev/null
if [ ! -s "$tmp/mid.snap" ]; then
    echo "check-replay: FAILED — --snapshot wrote no file"
    fail=1
fi
"$QUANTA" --quiet --restore="$tmp/mid.snap" >"$tmp/p2.out" 2>/dev/null; res_rc=$?
cat "$tmp/p1.out" "$tmp/p2.out" >"$tmp/joined.out"
if cmp -s "$tmp/full.out" "$tmp/joined.out" && [ "$res_rc" -eq "$full_rc" ]; then
    echo "OK    mid-run snapshot resumes to the same output and exit ($res_rc)"
else
    echo "check-replay: FAILED — resumed run diverged from the whole run"
    echo "  full:   '$(cat "$tmp/full.out")' (exit $full_rc)"
    echo "  joined: '$(cat "$tmp/joined.out")' (resume exit $res_rc)"
    fail=1
fi

# 2. Snapshot of an already-halted machine round-trips: restoring it is halted at
#    once with the same exit and no further output.
"$QUANTA" --quiet --snapshot="$tmp/end.snap" "$HELLO" >/dev/null 2>&1; end_rc=$?
"$QUANTA" --quiet --restore="$tmp/end.snap" >"$tmp/e2.out" 2>/dev/null; e2_rc=$?
if [ ! -s "$tmp/e2.out" ] && [ "$e2_rc" -eq "$end_rc" ]; then
    echo "OK    halted-machine snapshot round-trips (exit $e2_rc, no re-run)"
else
    echo "check-replay: FAILED — halted snapshot did not round-trip"
    echo "  resumed output: '$(cat "$tmp/e2.out")' (exit $e2_rc, want $end_rc)"
    fail=1
fi

# 3. Mid-run snapshot on a silent compute guest: the resumed run reaches the same
#    exit code as the whole run (a state-heavy workload — stack, array traversal).
"$QUANTA" --quiet "$STACK" >/dev/null 2>&1; s_full=$?
"$QUANTA" --quiet --max-steps=2000 --snapshot="$tmp/st.snap" "$STACK" >/dev/null 2>&1
"$QUANTA" --quiet --restore="$tmp/st.snap" >/dev/null 2>&1; s_res=$?
if [ "$s_res" -eq "$s_full" ]; then
    echo "OK    compute-guest snapshot resumes to the same exit ($s_res)"
else
    echo "check-replay: FAILED — compute guest exit $s_res != whole-run $s_full"
    fail=1
fi

# 4. A corrupt snapshot file is rejected cleanly (a non-zero exit, not a crash).
printf 'not a valid snapshot file' >"$tmp/bad.snap"
"$QUANTA" --quiet --restore="$tmp/bad.snap" >/dev/null 2>&1; bad_rc=$?
if [ "$bad_rc" -ne 0 ] && [ "$bad_rc" -lt 128 ]; then
    echo "OK    a corrupt snapshot file is rejected cleanly (exit $bad_rc)"
else
    echo "check-replay: FAILED — corrupt snapshot not handled cleanly (exit $bad_rc)"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "check-replay: FAILED"
    exit 1
fi
echo "snapshot serialisation round-trips (--snapshot / --restore)"
