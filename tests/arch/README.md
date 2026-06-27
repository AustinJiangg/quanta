# Official RISC-V architectural conformance (`make check-arch`)

This directory is Quanta's **target** for the official RISC-V architectural test
suite, [`riscv-non-isa/riscv-arch-test`][repo]. Running it is the recognised bar
for "this really is RV32I / RV32M / …": each test computes results into a
*signature* region, the simulator dumps that region, and it is compared against a
**reference signature** that ships with the suite.

## Why this needs no reference model

The hand-written `make check` pins Quanta against our own expectations, and
`make check-diff` cross-checks it against qemu. This goes one step further to the
*official* tests — but, unlike a full RISCOF flow, it needs **no Sail or Spike**.
We pin the suite's `old-framework-2.x` branch, a frozen release that **commits
its golden reference signatures** (generated upstream by the maintainers' formal
model). So the comparison is official-tests-vs-official-references, entirely
offline: all `make check-arch` needs is the cross-compiler and a one-time clone.

`make check-diff` remains the qemu-backed differential net; this is the
conformance net. They are complementary.

## How it works

`tests/check_arch.sh`:

1. Clones the pinned suite into `build/riscv-arch-test/` (gitignored) on first
   run, and caches it after.
2. Builds each test with the framework's `arch_test.h` plus this directory's
   `model_test.h` and `link.ld`.
3. Runs it under `quanta --quiet --signature=FILE`, which dumps
   `[begin_signature, end_signature)` in the suite's reference format.
4. Diffs that against the committed `…/references/<test>.reference_output`.

It **skips cleanly** (exit 0) when the cross-toolchain or the network is
unavailable, so it is a no-op rather than a failure on a machine without them.

### Files

- `model_test.h` — the DUT glue. Defines the signature-region markers and halts
  via Quanta's built-in SEE (`exit(0)`); console and interrupt hooks are no-ops.
- `link.ld` — a flat image based at `0x80000000` (Quanta's load base).

## Scope

Run, and fully passing against the official references:

| Family     | What it covers          | `-march`                 |
| ---------- | ----------------------- | ------------------------ |
| `I`        | RV32I base integer      | `rv32i_zicsr`            |
| `M`        | RV32M multiply / divide | `rv32im_zicsr`           |
| `Zifencei` | `fence.i`               | `rv32i_zifencei_zicsr`   |

(`Zicsr` is enabled because the framework's startup touches CSRs.)

### Deliberately out of scope

- **`C`, `F`, `K`** — unimplemented extensions (roadmap M11).
- **`privilege`** — these exercise traps on **misaligned** loads/stores and
  branches. The RISC-V spec *permits* a hart to handle misaligned access in
  hardware instead of trapping, which is what Quanta does, so those signatures
  legitimately differ. The hand-written `test_trap`/`test_priv` cover Quanta's
  actual trap behaviour. (Revisiting misaligned-access traps is possible future
  work; it would let this family run too.)
- **`I/jalr-01`** — uses `la x0, 5b`, which modern binutils rejects (a suite
  wart, not a Quanta gap). JALR is covered by `make check` and `make check-diff`.

## Running

```sh
make check-arch                       # fetch (first time), build, run, compare
RVCC=riscv32-unknown-elf-gcc make check-arch     # different toolchain prefix
ARCH_TEST_DIR=/path/to/riscv-arch-test make check-arch   # reuse a checkout
```

## Provenance

`model_test.h` and `link.ld` are adapted from the suite's `sail-riscv-c` example
target (BSD-3-Clause). The suite itself is fetched at run time and not vendored
here; it is pinned to commit `6f7f47bd` of `old-framework-2.x`.

[repo]: https://github.com/riscv-non-isa/riscv-arch-test
