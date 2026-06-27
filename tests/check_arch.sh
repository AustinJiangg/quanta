#!/bin/sh
# Official RISC-V architectural conformance for Quanta.
#
# Builds each program from the official suite (riscv-non-isa/riscv-arch-test,
# old-framework-2.x — a frozen branch with committed reference signatures) with
# the Quanta target in tests/arch/, runs it under `quanta --signature`, and
# diffs the dumped signature against the suite's committed reference. Agreement
# is the recognised bar for "this really is RV32I/M/...". No external reference
# *model* is needed: the golden signatures ship in the repo, generated upstream
# by Sail/Spike — so unlike `make check-diff` (which needs qemu) this needs only
# the cross-compiler and a one-time clone of the suite.
#
# Scope. We run the families Quanta implements and passes in full:
#   I         the RV32I base integer set
#   M         RV32M multiply/divide
#   Zifencei  fence.i
# Deliberately out of scope (see tests/arch/README.md):
#   C, F, K           unimplemented extensions (roadmap M11)
#   privilege         exercises traps on *misaligned* access, which Quanta
#                     handles in hardware (a spec-permitted choice) rather than
#                     trapping, so those signatures legitimately differ
#   I/jalr-01         uses `la x0,5b`, which modern binutils rejects (a suite
#                     wart, not a Quanta gap; JALR is covered by `make check`)
#
# Skips cleanly (exit 0) when the cross-toolchain or the network is unavailable,
# so it is a no-op rather than a failure on a machine without them. Override the
# compiler with RVCC=, or point ARCH_TEST_DIR= at an existing suite checkout.
#
# Usage: sh tests/check_arch.sh

set -u

QUANTA=./quanta
RVCC="${RVCC:-riscv64-unknown-elf-gcc}"

ARCH_REMOTE="https://github.com/riscv-non-isa/riscv-arch-test"
ARCH_BRANCH="old-framework-2.x"
ARCH_COMMIT="6f7f47bdc61c0c51c0cbf75789678a1235eeefc2"
ARCH_DIR="${ARCH_TEST_DIR:-build/riscv-arch-test}"
TARGET="tests/arch"
WORK="build/arch-work"

# family:march for each suite we claim.
FAMILIES="I:rv32i_zicsr M:rv32im_zicsr Zifencei:rv32i_zifencei_zicsr"
# Upstream tests skipped for reasons unrelated to Quanta's correctness.
EXCLUDE="jalr-01"

# --- preconditions: skip cleanly when we cannot run, like check-diff ----------
if [ ! -x "$QUANTA" ]; then
    echo "check-arch: build ./quanta first (run 'make')"
    exit 1
fi
if ! command -v "$RVCC" >/dev/null 2>&1; then
    echo "check-arch: cross-compiler '$RVCC' not found — skipping"
    echo "  (install the riscv64-unknown-elf toolchain, or set RVCC=)"
    exit 0
fi

# --- obtain the suite: a cached, pinned shallow clone -------------------------
if [ ! -f "$ARCH_DIR/riscv-test-suite/env/arch_test.h" ]; then
    echo "check-arch: fetching riscv-arch-test ($ARCH_BRANCH) into $ARCH_DIR ..."
    rm -rf "$ARCH_DIR"
    if ! git clone --quiet --depth 1 --branch "$ARCH_BRANCH" \
            "$ARCH_REMOTE" "$ARCH_DIR" 2>/dev/null; then
        echo "check-arch: cannot fetch the suite (no network?) — skipping"
        exit 0
    fi
fi
have=$(cd "$ARCH_DIR" 2>/dev/null && git rev-parse HEAD 2>/dev/null)
if [ -n "$have" ] && [ "$have" != "$ARCH_COMMIT" ]; then
    echo "check-arch: note: suite at $have, pinned to $ARCH_COMMIT"
fi

ENV="$ARCH_DIR/riscv-test-suite/env"
mkdir -p "$WORK"

excluded() {
    case " $EXCLUDE " in *" $1 "*) return 0 ;; esac
    return 1
}

total_pass=0
total_fail=0
total_skip=0
fails=""

for entry in $FAMILIES; do
    fam=${entry%%:*}
    march=${entry#*:}
    src="$ARCH_DIR/riscv-test-suite/rv32i_m/$fam/src"
    ref="$ARCH_DIR/riscv-test-suite/rv32i_m/$fam/references"
    if [ ! -d "$src" ]; then
        echo "check-arch: family $fam not present in the suite — skipping"
        continue
    fi

    p=0
    f=0
    s=0
    for sfile in "$src"/*.S; do
        name=$(basename "$sfile" .S)
        if excluded "$name"; then
            s=$((s + 1))
            continue
        fi
        elf="$WORK/$name.elf"
        sig="$WORK/$name.signature"

        if ! "$RVCC" -march="$march" -mabi=ilp32 -static -mcmodel=medany \
                -fvisibility=hidden -nostdlib -nostartfiles -DXLEN=32 \
                -I"$ENV" -I"$TARGET" -T"$TARGET/link.ld" \
                "$sfile" -o "$elf" 2>/dev/null; then
            f=$((f + 1))
            fails="$fails $fam/$name(compile)"
            continue
        fi

        # The exit code does not decide pass/fail — the signature does — but a
        # missing/short signature (a crash or runaway) will fail the diff below.
        "$QUANTA" --quiet --signature="$sig" "$elf" >/dev/null 2>&1

        if diff -q "$ref/$name.reference_output" "$sig" >/dev/null 2>&1; then
            p=$((p + 1))
        else
            f=$((f + 1))
            fails="$fails $fam/$name"
        fi
    done

    printf "  %-9s %2d/%-2d passed" "$fam" "$p" "$((p + f))"
    [ "$s" -gt 0 ] && printf " (%d excluded)" "$s"
    echo
    total_pass=$((total_pass + p))
    total_fail=$((total_fail + f))
    total_skip=$((total_skip + s))
done

echo "check-arch: $total_pass passed, $total_fail failed, $total_skip excluded"
if [ "$total_fail" -ne 0 ]; then
    echo "  FAILED:$fails"
    exit 1
fi
echo "quanta matches the official riscv-arch-test reference signatures"
