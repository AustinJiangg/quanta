#!/bin/sh
# Pin M16: boot the small teaching kernel (tests/os/) and confirm it reaches
# userspace and shuts down cleanly — the integration of M8-M15. We assert every
# stage of the boot left its mark on stdout:
#
#   - the kernel turned on Sv32 paging,
#   - the userspace process ran and printed through the write syscall,
#   - the supervisor timer preempted it the expected number of times,
#   - the user called exit and the kernel shut the machine down via SBI SRST,
#     so Quanta exits 0.
#
# The kernel manages the spare RAM that --memory carves out above its image; it
# runs in S-mode with paging and MMIO, which user-mode qemu cannot host, so like
# the other privileged tests it is pinned here rather than in check-diff.
set -u

QUANTA=./quanta
ELF=tests/os/kernel.elf
MEM=8M
WANT_TICKS=3

if [ ! -x "$QUANTA" ]; then
    echo "check-os: build quanta first (run 'make')"
    exit 1
fi
if [ ! -f "$ELF" ]; then
    echo "check-os: $ELF missing (run 'make $ELF')"
    exit 1
fi

out=$("$QUANTA" --quiet "--memory=$MEM" "$ELF" 2>/dev/null); rc=$?

if [ "$rc" -ne 0 ]; then
    echo "check-os: FAILED — kernel exited $rc (did not shut down cleanly)"
    echo "  got: '$out'"
    exit 1
fi
if ! printf '%s' "$out" | grep -q 'sv32 paging enabled'; then
    echo "check-os: FAILED — kernel did not report paging enabled"
    echo "  got: '$out'"
    exit 1
fi
if ! printf '%s' "$out" | grep -q 'hello from userspace'; then
    echo "check-os: FAILED — userspace process output not seen on stdout"
    echo "  got: '$out'"
    exit 1
fi
ticks=$(printf '%s\n' "$out" | grep -c 'timer tick')
if [ "$ticks" -ne "$WANT_TICKS" ]; then
    echo "check-os: FAILED — expected $WANT_TICKS timer ticks, saw $ticks"
    echo "  got: '$out'"
    exit 1
fi
if ! printf '%s' "$out" | grep -q 'user exited (code 0)'; then
    echo "check-os: FAILED — kernel did not report a clean user exit"
    echo "  got: '$out'"
    exit 1
fi

echo "OK    kernel booted to S-mode and enabled Sv32 paging"
echo "OK    userspace process ran and printed via the write syscall"
echo "OK    supervisor timer preempted the user $WANT_TICKS times"
echo "OK    user exited and the kernel shut down cleanly (exit 0)"
echo "teaching kernel boots and runs a userspace process"
