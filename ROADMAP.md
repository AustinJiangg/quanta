# Roadmap

Quanta is built incrementally as a way to learn computer architecture, so this
roadmap is both a development plan and a learning path. Each milestone is a
self-contained, runnable step: it states what to build, which RV32I
instructions or mechanisms it touches, the architecture concept it teaches, how
to know it's done, and the commits it roughly breaks into.

Milestones are ordered so the emulator is always in a working state at the end
of each one. Treat the checkboxes as a progress tracker.

Legend: **Build** = what to implement · **ISA** = instructions/mechanisms ·
**Concept** = the architecture idea to internalise · **Done when** = acceptance
check · **Commits** = an illustrative breakdown of the work (land it as a single
commit — see "How to use this roadmap").

---

## M0 — Minimal core (DONE)

The current MVP. Establishes the fetch/decode/execute loop and the project
skeleton.

- [x] **Build:** CPU state (32 regs + PC), flat little-endian memory,
  fetch/decode/execute loop, a hardcoded program in `main.c`, register dump.
- [x] **ISA:** OP-IMM (ADDI etc.), OP (ADD/SUB etc.), LUI, AUIPC, JAL, JALR,
  BRANCH, LOAD, STORE, ECALL-as-halt.
- [x] **Concept:** the architectural state of a hart; the instruction cycle;
  how a 32-bit word encodes opcode/registers/immediate.
- [x] **Done when:** `make run` prints `a2 = 42, a3 = 32 -> OK`.

---

## M1 — ELF loader (DONE)

Stop hardcoding machine code. Load real compiled programs so the cross-toolchain
output becomes the input. `src/elf.{h,c}` now parses the ELF32 header and
program headers, copies `PT_LOAD` segments to their virtual addresses, and
hands the entry point back as the initial PC; the no-argument built-in demo is
kept as a toolchain-free smoke test.

- [x] **Build:** a loader that parses a 32-bit RISC-V ELF, reads the program
  headers, copies `PT_LOAD` segments into memory at their virtual addresses,
  and sets PC to the ELF entry point. Add a CLI: `quanta <program.elf>`.
- [x] **ISA:** none new — this is infrastructure.
- [x] **Concept:** the ELF object format; the difference between an instruction
  stream on disk and a loaded memory image; entry points and load addresses;
  why the linker script's `-Ttext` matters.
- [x] **Done when:** `make tests` builds `tests/hello.elf`, and
  `./quanta tests/hello.elf` produces the same register state as the M0 demo.
- [x] **Commits:** `feat: add minimal ELF32 loader` (parser and segment copy
  landed together), `feat: run elf programs from the command line`,
  `docs: document running ELF programs`.

Reference: parse just enough of the ELF header and program headers; ignore
sections. `riscv64-unknown-elf-readelf -l tests/hello.elf` shows what to load.

---

## M2 — System calls (real output) (DONE)

Give programs a way to interact with the outside world, starting with printing
and a proper exit. `src/syscall.{h,c}` is the new "kernel" side: ECALL traps
into it, the syscall number in `a7` selects `write` (fd 1=stdout, 2=stderr) or
`exit`/`exit_group`, and EBREAK — distinguished from ECALL by its immediate —
stops the machine. The driver reports the exit status (and the halt reason for
ebreak/traps).

- [x] **Build:** an ECALL handler driven by the syscall number in `a7`,
  implementing at least `write` (so programs can print to stdout) and `exit`
  (clean halt with a status code). Follow the RISC-V Linux/`newlib` syscall ABI
  for numbers and argument registers (`a0`–`a6`).
- [x] **ISA:** ECALL (replacing the M0 halt-on-any-system-instruction stub);
  distinguish ECALL from EBREAK via the immediate field.
- [x] **Concept:** the user/kernel boundary; how syscalls pass a number and
  arguments in registers; the calling convention (ABI) layered on top of the
  raw ISA.
- [x] **Done when:** a "hello, world" C/asm program built for bare metal prints
  text and exits with a chosen status that the emulator reports.
- [x] **Commits:** `feat: dispatch ecall to write and exit syscalls`,
  `feat: report exit status and halt reason`,
  `test: add syscall test programs`,
  `docs: document system-call support`.

---

## M3 — Full RV32I conformance (DONE)

Make sure every base-integer instruction is correct, not just the demo subset.
A hand-written conformance suite (`tests/test_*.S`, run by `make check`)
exercises each instruction group's easy-to-miss semantics and signals the first
failed check through its exit code, which quanta now propagates. The official
`riscv-tests` were *not* used: their `-p` environment needs CSR/trap support
that belongs to a later privileged-mode milestone. The audit confirmed the
arithmetic, branch, load/store, jump and upper-immediate cores were already
correct, and found one gap — FENCE wasn't decoded — now run as a no-op.

- [x] **Build:** audit each RV32I instruction against the spec; fix edge cases
  (shift-amount masking, signed vs unsigned comparisons, sign-extension on
  loads, JALR clearing the low bit, branch offset arithmetic). Add a focused
  test per instruction group.
- [x] **ISA:** the complete RV32I set, with attention to the easy-to-miss
  semantics rather than new opcodes.
- [x] **Concept:** why ISA specs are precise about corner cases; the cost of
  ambiguity in hardware; how a conformance suite pins down behaviour.
- [x] **Done when:** the project passes the relevant `riscv-tests` (or a hand-
  written equivalent) for RV32I — each test program runs to its success exit.
- [x] **Commits:** `feat: propagate guest exit status as the process exit code`,
  `test: add rv32i conformance suite`, `fix: execute fence as a no-op`,
  `docs: document the rv32i conformance suite`.

Reference: the official `riscv-software-src/riscv-tests` repository; the
RISC-V Unprivileged ISA spec, Vol. 1.

---

## M4 — Disassembler and trace mode (DONE)

Make the machine observable. A disassembler (`src/disasm.{h,c}`) turns an
instruction word back into objdump-style assembly — ABI register names, the
common pseudo-instructions (`nop`/`mv`/`li`/`j`/`jr`/`ret`/`beqz`/…), and
absolute branch/jump targets. It reuses the very decode helpers the executor
uses, now lifted into a shared `src/decode.h`, so the two views can never
disagree about an instruction's layout. A `--trace` flag narrates execution to
stderr: each instruction's PC, raw word, disassembly, the registers it changed,
and the redirect target on a taken branch/jump/trap — recovered by diffing a
register snapshot around `cpu_step()`, so the core stays untouched. Interactive
single-step is deferred; the trace already gives later milestones the
observability they need.

- [x] **Build:** a disassembler that turns an instruction word back into
  readable assembly, and a `--trace` flag that prints each executed instruction
  with PC and the registers it changed. (Single-step mode deferred.)
- [x] **ISA:** none new — a reverse view of the decoder.
- [x] **Concept:** the symmetry between encoding and decoding; how real tools
  (`objdump`, simulators) present execution; why observability matters for
  debugging hardware/emulators.
- [x] **Done when:** `./quanta --trace tests/hello.elf` prints a line per
  instruction that matches `objdump -d` mnemonics — pinned by `make
  check-disasm`, which diffs the traced disassembly against objdump across the
  whole sample suite.
- [x] **Commits:** `refactor: extract shared decode helpers`,
  `feat: add RV32I disassembler`, `feat: add per-instruction trace flag`,
  `test: check disassembly against objdump`.

---

## M5 — RV32M (multiply / divide) (DONE)

The first ISA *extension*, showing how RISC-V is modular. RV32M reuses the OP
opcode and is selected purely by `funct7 = 0x01`, so `cpu.c` dispatches into a
new `exec_muldiv`: MUL/MULH(SU/U) form a 64-bit intermediate product (the three
high-half variants differ only in each operand's sign- vs zero-extension), and
DIV(U)/REM(U) return the spec's *defined* results for divide-by-zero and the
`INT_MIN / -1` signed overflow instead of trapping. The disassembler gained the
eight mnemonics, and `tests/test_muldiv.S` — assembled with `-march=rv32im` via
a per-target Makefile override — pins the semantics; both `make check` and `make
check-disasm` cover it.

- [x] **Build:** the M extension — MUL, MULH(SU/U), DIV(U), REM(U) — including
  the spec-mandated results for divide-by-zero and signed overflow.
- [x] **ISA:** RV32M (the OP opcode with `funct7 = 0x01`).
- [x] **Concept:** ISA modularity (base + optional extensions); why
  multiply/divide are separable; defined behaviour for exceptional arithmetic
  instead of faulting.
- [x] **Done when:** programs using `*`, `/`, `%` (compiled with
  `-march=rv32im`) produce correct results, including the div-by-zero cases.
- [x] **Commits:** `feat: add RV32M multiply and divide`,
  `feat: disassemble RV32M instructions`,
  `test: add RV32M conformance suite`, `docs: document RV32M support`.

---

### Infrastructure interlude — a stack and real workloads

Before the performance milestones, two MVP-era limits were lifted so non-trivial
programs can run. The ELF loader now reserves a 64 KiB stack block above the
load image and `main.c` initialises `sp` to the top of the guest region, so
programs can make function calls and spill locals (the ISA reset state had
`sp = 0`, which faulted on the first push). And the per-run instruction cap was
raised from 1000 to 100M so loop-heavy workloads run to completion.
`tests/test_stack.S` validates the stack — a non-leaf function spilling to it —
and doubles as a small array-traversal workload for the cache model below.

---

## M6 — Memory hierarchy: a cache model (DONE)

Cross from "correct execution" into *performance* architecture — the area most
relevant to real optimisation work. `src/cache.{h,c}` adds a set-associative
cache with LRU replacement (direct-mapped is the associativity-1 case), enabled
by `--cache[=SIZE:WAYS:BLOCK]`. It is a pure observability layer: `cpu_step`'s
load/store paths notify it of each data address, but the bytes still flow
through the flat memory, so it can never change a result. At exit it reports
accesses/hits/misses/miss-rate, and `make check-cache` pins both invariants —
results unchanged, and a smaller cache misses more — on the `test_stack`
array-traversal workload. A geometry sweep on that workload shows the textbook
effects: a larger block lowers the miss rate (spatial locality), and a larger
cache lets the second pass hit (temporal locality).

- [x] **Build:** an optional cache model in front of memory (start with a single
  direct-mapped L1, then set-associative with configurable size/associativity/
  block size and LRU replacement). Count hits and misses; report a summary.
  Keep it as an observability layer — it must not change program results.
- [x] **ISA:** none new — this wraps LOAD/STORE.
- [x] **Concept:** the memory hierarchy; temporal and spatial locality; how
  associativity, block size, and replacement policy trade off; why cache
  behaviour dominates real-world performance.
- [x] **Done when:** running a program reports a hit/miss breakdown, and
  changing cache parameters measurably changes the miss rate on a
  locality-sensitive workload.
- [x] **Commits:** `feat: add a set-associative LRU cache model`,
  `feat: report cache stats behind a --cache flag`,
  `test: pin cache behaviour on a workload`, `docs: document the cache model`.

---

## M7 — A simple pipeline model (stretch) (DONE)

Model time, not just outcomes. This is the conceptual capstone. `src/pipeline.
{h,c}` adds a 5-stage timing overlay enabled by `--pipeline`. Like the cache it
changes nothing functionally: it watches the retired instruction stream and
estimates cycles by charging the stalls a forwarding pipeline cannot hide — one
bubble per load-use hazard, and a predict-not-taken control penalty (2 cycles
for a taken branch or JALR, 1 for a JAL). It reports instructions, cycles, CPI,
and the stall breakdown. `tests/hazard_slow.S` and `tests/hazard_fast.S` are the
same array sum scheduled two ways; `make check-pipeline` confirms that moving the
load away from its use drops load-use stalls from 256 to 0 (3860 → 3604 cycles)
with an unchanged result.

- [x] **Build:** a classic 5-stage pipeline view (IF, ID, EX, MEM, WB) with
  cycle counting, detecting data hazards and modelling stalls/forwarding and
  control-hazard penalties on branches. Functional results stay identical;
  this estimates cycles.
- [x] **ISA:** none new — a timing overlay on the existing core.
- [x] **Concept:** instruction-level parallelism; pipeline hazards (data,
  control, structural); forwarding and stalls; CPI as a performance metric.
- [x] **Done when:** the emulator reports an estimated cycle count and CPI, and
  reordering instructions to avoid a hazard visibly lowers the stall count.
- [x] **Commits:** `feat: add a 5-stage pipeline timing model`,
  `feat: report cycle count and CPI behind --pipeline`,
  `test: show scheduling cuts load-use stalls`, `docs: document the pipeline model`.

---

# Part II — Toward a production-grade emulator

Milestones M0–M7 took Quanta from nothing to a correct, observable RV32IM core
with cache and pipeline overlays — the *learning* arc. Part II changes the goal:
make Quanta a **production-grade** project. That means advancing two independent
axes at once — **engineering maturity** (how it is built, tested, and shipped)
and **capability** (what it can actually do) — not merely adding instructions.

The chosen flagship capability is **full-system emulation that boots an
operating system**. The substrate — virtual memory, devices, interrupts, traps —
is built and validated on **RV32 first**; the move to **RV64GC** is deferred
until a mainstream OS demands it. Be honest about the consequence: the RV32
phase boots an RV32 target (a bare-metal S-mode program, then a small RV32 OS or
RV32 Linux), while upstream xv6-riscv and standard Linux distributions are
RV64GC — that trophy lands after the RV64 transition (M17). RV64 is the *unlock*
for mainstream OSes, not the starting point.

Two architectural changes anchor the whole effort, each as defining as M0's
fetch loop:

- **Engine/library split (`libquanta`).** The core becomes a reusable library
  with a clean C API and no fatal `exit()` calls; the CLI is a thin client.
  This unlocks the GDB stub, language bindings, and real testability.
- **MMU + MMIO memory layer.** `memory.c` stops being a flat array: it gains
  VA→PA translation through a page-table walker and dispatches physical address
  ranges to device models. This is the heart of the full-system phase.

Two tracks run in parallel. The **engineering track (E-line)** is continuous and
front-loaded — it is what most separates a learning project from a production
one, and every later milestone leans on it. The **capability track (M8+)**
advances the ISA and then the full-system feature set in dependency order.

Legend is unchanged (**Build / ISA / Concept / Done when / Commits**); E-line
entries add **Why** — the production rationale.

---

## Engineering track (E-line)

Not sequential milestones but a parallel track to keep green throughout Part II.
Front-load E1, E2, E5: the library split unblocks tooling, CI makes regressions
visible, and differential testing against a golden model is the safety net under
every ISA change that follows.

### E1 — Split the engine into `libquanta` (DONE)

- [x] **Build:** the core is wrapped behind an opaque `Quanta *` handle
  (`src/quanta.{h,c}`): lifecycle, ELF/raw-image loading, optional cache,
  `quanta_step`/`quanta_run`, and register/memory accessors, with public
  `QuantaStatus`/`QuantaHalt` enums decoupled from the internal `HaltReason`. The
  core's only host-killing `exit()` is gone — an out-of-range access is now a
  recorded fault. `main.c` is a thin client; the engine builds as `libquanta.a`.
  (The richer event/hook interface is deferred to E9, where the GDB stub needs it.)
- [x] **Why:** precondition for the GDB stub, bindings, fuzz harnesses, and unit
  tests. Most of Part II's tooling depends on it.
- [x] **Done when:** `examples/embed.c` (~30 lines) embeds the emulator and runs
  a guest (`make embed`); the CLI links `libquanta`; no `exit()` remains in the
  core.
- [x] **Commits:** `refactor: replace core exit() with halt reasons`,
  `refactor: extract libquanta engine api`,
  `refactor: rebuild the cli on libquanta`.

### E2 — Continuous integration (DONE)

- [x] **Build:** a GitHub Actions matrix (`gcc`×`clang`, release×debug) builds
  the library + CLI and runs the embedding example and every `make check /
  check-disasm / check-cache / check-pipeline` on each push and PR, with
  `-Werror` so warnings fail. (The cross-toolchain is installed via apt per run;
  caching it is a later optimisation.)
- [x] **Why:** regressions surface immediately, not at the next manual run.
- [x] **Done when:** every push reports a status; a CI badge is in the README.
- [x] **Commits:** `chore: add ci build-and-test matrix`,
  `docs: add ci status badge`.

### E3 — Sanitizer builds (DONE)

- [x] **Build:** `make sanitize` builds the emulator with ASan + UBSan
  (`-fno-sanitize-recover`, so UB aborts) and runs the whole suite — embed,
  conformance, disasm, cache, pipeline, differential — through the instrumented
  binary. A dedicated CI job runs it on every push. The tree was already clean
  (E1's bounds-safe memory and UB-free decode), so no fixes were needed.
- [x] **Why:** memory-safety bugs in an emulator are both crashes and exploits;
  it parses untrusted ELF and executes arbitrary decoded instructions.
- [x] **Done when:** the suite passes clean under ASan/UBSan in CI.
- [x] **Commits:** `chore: add a sanitizer build and ci job`.

### E4 — Fuzzing (DONE)

- [x] **Build:** two libFuzzer harnesses (`fuzz/`) over the untrusted-input
  surfaces — the ELF loader (`fuzz_elf`) and decode→execute (`fuzz_decode`),
  each linking the engine under `-fsanitize=fuzzer,address,undefined`. `make
  fuzz` builds them (clang); `make fuzz-replay` runs them over the seed corpus
  under gcc + ASan/UBSan (via `fuzz/standalone.c`) so they stay exercised without
  clang. A CI job fuzzes each for 40s, seeded with the sample ELFs, and uploads
  any crash input as an artifact.
- [x] **Why:** untrusted input deserves adversarial input.
- [x] **Done when:** a fuzz target runs in CI; malformed ELF always errors
  cleanly rather than hitting UB. (No bug found — the loader is defensively
  bounds-checked; a local 650-input malformed smoke was clean too.)
- [x] **Commits:** `test: add libfuzzer harnesses`,
  `chore: run fuzzing in ci`.

### E5 — Differential testing against a golden model (DONE)

- [x] **Build:** `tests/check_diff.sh` runs each sample ELF through Quanta
  (`--quiet`, so stdout is only the guest's output) and a reference simulator,
  asserting they agree on stdout and exit code. The reference is qemu-riscv32 by
  default (spike was neither installed nor apt-installable in this environment),
  but the harness is model-agnostic via `$REF`, so a spike+pk wrapper drops in.
  Wired into CI as a dedicated job (`make check-diff`). (Per-instruction register
  lockstep and a randomly generated corpus are noted follow-ups.)
- [x] **Why:** agreement with a golden model is how real emulators earn trust —
  and the safety net under every ISA change in M8–M17.
- [x] **Done when:** the sample corpus matches the reference (qemu-riscv32)
  bit-for-bit in CI — all 13 programs agree on stdout and exit code.
- [x] **Commits:** `feat: add --quiet to suppress driver chatter`,
  `test: diff-test quanta against qemu-riscv32`,
  `chore: run differential test in ci`.

### E6 — Official conformance (riscv-arch-test) (DONE)

The official architectural tests, run as `make check-arch`. Each test computes a
signature the simulator dumps and the suite compares against a golden reference —
the recognised bar for "this really is RV32I/M/...". A new `quanta --signature`
flag (backed by an `elf_symbol` lookup for the begin/end_signature markers) makes
Quanta a drop-in target, like spike's `--test-signature`; `tests/arch/` holds the
model glue (a SEE-`exit` halt) and link script, and `tests/check_arch.sh` builds,
runs, and diffs each test. Crucially this needs **no reference model**: rather
than the full RISCOF + Sail/Spike flow (none of which is installable in this
environment), it pins the suite's frozen `old-framework-2.x` branch, which
*commits* the golden signatures its maintainers generated — so the check is
offline, needing only the cross-compiler and a one-time clone. Quanta passes
every test in the families it implements: RV32I (37), RV32M (8), and Zifencei
(1). Out of scope, with reasons in `tests/arch/README.md`: C/F/K (unimplemented,
M11); the `privilege` family (it traps on *misaligned* access, which Quanta
handles in hardware — a spec-permitted choice — rather than trapping); and the
`jalr-01` wart (`la x0,5b`, rejected by modern binutils). `make check-diff` stays
the qemu differential net; this is the conformance net. A CI job runs it on every
push.

- [x] **Build:** run the official architectural test suite, augmenting the
  hand-written `make check`. Needs the minimal trap/CSR support from M8/M9.
- [x] **Why:** the recognised bar for "this really is RV32I/M/A/...".
- [x] **Done when:** RV32I (then IMAC…) signatures match the reference.
- [x] **Commits:** `feat: dump arch-test signature with --signature`,
  `test: run riscv-arch-test conformance suite`,
  `chore: run riscv-arch-test in ci`, `docs: document arch-test conformance`.

### E7 — Coverage and static analysis (DONE)

Two CI nets that measure and police the code itself. `make coverage` does a
clean gcov-instrumented rebuild (`--coverage`) and runs the whole functional
suite plus the embedding example so the `.gcda` counts accumulate across every
program; `tests/coverage.sh` then summarises line coverage, preferring lcov (an
HTML report under `build/coverage`, uploaded as a CI artifact) and falling back
to plain gcov on a bare box. `make analyze` runs cppcheck and clang-tidy over
`src/` — each skipping cleanly when its tool is absent, like the qemu
differential — and CI adds scan-build (the clang static analyzer) over a full
build. The analyzers are configured to pass *clean*, not merely run: the curated
`.clang-tidy` disables only genuine noise — the C11 Annex K `*_s`-function nag
(glibc ships no such functions), include-cleaner, and the missing-default-case
check that fights an emulator's exhaustive fixed-width decode switches — each
with a written reason, while the real findings were fixed in the code (64-bit
size-macro arithmetic, a checked `fseek` replacing `rewind`, and hoisting two
assignments out of `if` conditions). Coverage currently sits at ~81% of lines
and 93% of functions, the gaps being mostly CLI and error paths the conformance
suite does not drive. New CI jobs run both on every push.

- [x] **Build:** gcov/lcov coverage reporting plus a clang-tidy + scan-build +
  cppcheck pass in CI.
- [x] **Why:** measure what the tests miss; catch defects before runtime.
- [x] **Done when:** coverage is reported per PR; static analysis is clean or
  baselined.
- [x] **Commits:** `chore: add coverage reporting`,
  `chore: add static analysis ci`.

### E8 — Release engineering (DONE)

Quanta now ships as a versioned, documented artifact. The version is defined
once as `QUANTA_VERSION_*` in `src/quanta.h` (Semantic Versioning), surfaced by
`quanta --version` and the `quanta_version()` API, and kept in step with a
`CHANGELOG.md` (Keep a Changelog) and the git tag. A `quanta.1` man page
documents the CLI, and `make install` lays the binary, the engine library, the
public header, and the man page under `$(DESTDIR)$(PREFIX)`. Builds are
reproducible: the static archive is created deterministically (`ar D`) and the
objects embed no `__DATE__`/`__TIME__`, so `libquanta.a` is byte-identical across
rebuilds. The first release is tagged `v0.1.0` (M0–M12, E1–E8).

- [x] **Build:** SemVer, a CHANGELOG, a `--version` flag, tagged GitHub
  releases, a man page, reproducible builds.
- [x] **Why:** a production project ships versioned, documented artefacts.
- [x] **Done when:** `quanta --version` works and a tagged v0.x release exists.
- [x] **Commits:** `feat: add --version`,
  `chore: add changelog and release tagging`.

### E9 — GDB remote stub (DONE)

Quanta now speaks the GDB remote serial protocol (RSP) over TCP, so a stock
`gdb` debugs a guest the same way it would real hardware or qemu's `-s`.
`src/gdbstub.{h,c}` adds `quanta_gdb_serve(q, port)`, built entirely on the
public `quanta.h` surface (register/memory access, single-step, halt reason) —
so it doubles as a worked example of driving the engine from outside, and as a
headline embeddable feature. `quanta --gdb[=PORT]` (default 1234) binds
localhost, accepts one debugger, and drives the machine on its behalf: the
`g`/`G`/`p`/`P` register packets, `m`/`M` memory, `s`/`c` (and `vCont`)
step/continue, `Z0`/`z0` breakpoints — tracked stub-side and enforced in the
continue loop, so guest memory is never patched with trap words — a Ctrl-C
interrupt, and a `qXfer` target description advertising the RV32 register file
(x0..x31 + pc) so gdb needs no hand-set architecture. The session ends on
detach/kill or guest exit, mapping the halt reason to the right stop reply
(`W` for a clean exit, `S05`/`S0b`/`S04` for trap/fault/illegal). It is the one
piece of OS-specific code in an otherwise ISO-C project, so the POSIX-sockets
feature macro is isolated to `gdbstub.c`. `tests/check_gdb.sh` exercises it end
to end with a self-contained RSP client (`tests/gdb_client.py`, no riscv `gdb`
needed in this environment) — asserting register/memory access, a single step,
and a breakpoint-then-continue-to-exit on `tests/hello.elf` — under `make
check-gdb`, and the same client runs inside `make sanitize` (ASan/UBSan over the
socket and packet-buffer code) and `make coverage`. A CI job runs it on push.

- [x] **Build:** a gdbserver-protocol stub over TCP (read/write registers and
  memory, single-step, breakpoints, continue) on top of the `libquanta` hooks.
- [x] **Why:** debugging a booting kernel without a debugger is brutal — this is
  effectively required tooling for Stage 3, and a headline embeddable feature.
- [x] **Done when:** real `gdb` attaches to a running guest and single-steps it.
- [x] **Commits:** `feat: add gdb remote stub`, `docs: document gdb debugging`.

---

## Capability track — full-system, boot an OS

### Stage 1 — ISA prerequisites (M8–M11)

## M8 — Zicsr + Zifencei (DONE)

The first Part II capability milestone. A SYSTEM word with a non-zero funct3 now
runs the six CSR instructions (`csrrw/csrrs/csrrc` and their immediate forms)
through a CSR file in `cpu.c` (`exec_csr`, with `csr_read`/`csr_write` as the
choke point M9 will hook privilege into). Most CSRs are plain WARL storage for
now; the unprivileged counters (`cycle`/`time`/`instret` and their RV32 high
halves) read back a live retired-instruction count, and writes to read-only CSRs
are dropped rather than trapped (traps are M9). FENCE.I (Zifencei) joins FENCE as
a no-op — a single in-order hart with no modelled icache has nothing to flush.
The disassembler gained the CSR pseudo-instructions (`csrr`/`csrw`/`rdinstret`/…)
so `make check-disasm` still matches objdump, and `tests/test_csr.S` pins the
read-modify-write and side-effect semantics. That test uses `mscratch`, a
machine-mode CSR user-mode qemu rejects, so it is kept out of `make check-diff`
and covered by `make check` instead. The official `riscv-tests` `-p` environment
(E6) needs the trap support that follows in M9; this milestone is its CSR half.

- [x] **Build:** a CSR register file and `csrrw/csrrs/csrrc(+i)`, plus `FENCE.I`.
- [x] **ISA:** Zicsr, Zifencei.
- [x] **Concept:** control/status registers as the machine's configuration and
  status surface; why CSR access is its own instruction class.
- [x] **Done when:** CSR programs run instead of halting; this unlocks the
  official `riscv-tests` `-p` environment (E6).
- [x] **Commits:** `feat: add csr register file and zicsr`,
  `feat: implement fence.i`, `docs: document csr support`.

## M9 — Privileged architecture (M/S/U + traps) (DONE)

The privilege model and the trap mechanism the full-system phase is built on.
The hart now tracks a current mode (M/S/U, resetting to M), and a single
`raise_trap` in `cpu.c` is the choke point every synchronous exception flows
through: it resolves the target mode by delegation (`medeleg` sends a trap taken
in S/U down to S-mode; a trap taken in M never delegates), stacks the
interrupt-enable and previous-privilege bits into `mstatus`, records
`*epc`/`*cause`/`*tval`, and vectors to `*tvec`. `mret`/`sret` pop that stacked
state to return, and `exec_csr` gained the privilege and read-only checks the
CSR encoding implies. The M-mode trap CSRs are real storage; the S-mode set
(`sstatus`/`sie`/`sip`) are masked views of their M counterparts. ECALL, EBREAK,
illegal-instruction, and misaligned-fetch faults route through real traps — but
with a deliberate hook: when no handler is installed (the resolved `*tvec` is
still 0), a trap falls back to the built-in SEE, so every pre-M9 program keeps
running unchanged while a guest that sets `mtvec`/`stvec` takes over its own
traps. `tests/test_trap.S` pins the M-mode path (ecall/ebreak/illegal →
`mcause`/`mepc` → `mret`) and `tests/test_priv.S` the M→U→S→U→M round trip
through delegation and `sret`; both stay out of `make check-diff` (machine CSRs)
and are held by `make check`. Interrupt *delivery* (vectored mode, `mip`-driven
entry) waits on the devices in M13 — the mechanism is here, the sources are not.

- [x] **Build:** privilege levels (M/S/U), the trap CSRs
  (`mstatus/mtvec/mepc/mcause/mie/mip/medeleg/mideleg` and the `s*` mirrors),
  exception and interrupt entry, `mret`/`sret`, and delegation. Route ECALL,
  EBREAK, illegal-instruction and misaligned faults through real traps.
- [x] **ISA:** the privileged-spec subset (no MMU yet).
- [x] **Concept:** the privilege model; precise traps; how interrupts and
  exceptions share a vector; delegation between modes.
- [x] **Done when:** a handler installed via `mtvec` catches a deliberate
  exception, inspects `mcause/mepc`, and returns with `mret`.
- [x] **Commits:** `feat: add privilege levels and trap csrs`,
  `feat: implement trap entry and mret/sret`,
  `feat: route exceptions through traps`, `docs: document the trap model`.

## M10 — RV32A atomics (DONE)

The atomic extension the kernel's locks and C11 atomics are built on. RV32A gets
its own major opcode (`0x2f`); `exec_amo` in `cpu.c` dispatches on the five-bit
`funct5`, with funct3 fixing the 32-bit "word" width. The nine AMO* operations
each atomically load the addressed word, combine it with rs2, store the result,
and return the old value; LR.W/SC.W split that into a reservation pair — LR
registers a word-granularity reservation, SC stores only while it still holds
and reports 0/1 success. The aq/rl ordering bits are no-ops on a single in-order
hart, and a misaligned atomic faults rather than being handled silently the way
base accesses are. `tests/test_atomic.S` pins every AMO (old value and stored
result, signed-vs-unsigned min/max) plus LR/SC both ways; being user-mode RV32A,
it is the first extension test qemu can also run, so `make check-diff`
cross-checks it. (Real multi-hart contention over the atomics waits on the SMP
work in M19.)

- [x] **Build:** `LR/SC` and the `AMO*` set (single-hart semantics).
- [x] **ISA:** RV32A.
- [x] **Concept:** atomic read-modify-write; load-reserved/store-conditional;
  why atomics are a separate extension the kernel requires.
- [x] **Done when:** an LR/SC spinlock and the AMOs pass a focused test —
  required before any Linux boot.
- [x] **Commits:** `feat: add rv32a atomics`, `feat: disassemble rv32a`,
  `test: add rv32a conformance suite`.

## M11 — Optional extensions: RV32C, RV32F/D (deferrable)

- [ ] **Build:** RV32C (expand the 16-bit encodings) and/or RV32F/D (an `fcsr`,
  rounding modes, the F/D op set via softfloat for host-independent results).
- [ ] **ISA:** RV32C, RV32F, RV32D.
- [ ] **Concept:** compressed encodings and code density; the float register
  file and the hard-float vs soft-float ABI split.
- [ ] **Done when:** as the chosen guest demands — defer by building the kernel
  without C and userspace soft-float; a full glibc GC userspace needs both.
- [ ] **Commits:** `feat: add rv32c compressed`, `feat: add rv32f/d float`.

### Stage 2 — Full-system substrate (M12–M14)

## M12 — Sv32 virtual memory (DONE)

The central architectural change of the full-system phase: addresses stop being
physical. `src/mmu.c` adds the Sv32 two-level page-table walker, a small TLB, and
the permission/A-D logic, and `cpu.c` now runs every instruction fetch and data
address through `mmu_translate` before it reaches memory. (The translation layer
lives in mmu.c rather than memory.c — it needs CPU state, satp/priv/mstatus,
while memory.c stays a dumb physical array.) A walk turns a 32-bit VA into a
physical one, honouring megapages, the U/SUM/MXR permission rules, and hardware
accessed/dirty updates; a missing mapping or permission violation raises a
precise page fault (cause 12/13/15) with the faulting VA in *tval. Paging is the
identity in M-mode and Bare mode, so it is inert until a guest writes satp —
every earlier test runs unchanged. The TLB caches fetch/load translations
(stores always walk so the dirty bit lands on the real PTE) and is flushed by
sfence.vma and any satp write. `tests/test_vm.S` builds a page table by hand,
enables Sv32, drops to S-mode, and proves non-identity translation (two VAs
aliased to one frame), the hardware dirty bit, and a caught load page fault; it
uses satp, so it joins the privileged tests outside make check-diff. (Sv39 and
its three-level walk are M18, after the RV64 transition.)

- [x] **Build:** a two-level Sv32 page-table walker, a TLB, `satp`,
  `sfence.vma`, and page-fault traps. `memory.c` gains VA→PA translation — the
  central architectural change of the full-system phase.
- [x] **ISA:** Sv32 address translation.
- [x] **Concept:** virtual memory; page tables and the walk; TLBs; how paging
  and the privilege model combine.
- [x] **Done when:** with paging enabled a user program runs in its virtual
  address space and an unmapped access faults precisely.
- [x] **Commits:** `feat: add sv32 page-table walk`, `feat: add a tlb`,
  `feat: trap on page faults`, `docs: document virtual memory`.

## M13 — Platform devices and interrupts (DONE)

The machine grows hardware beyond RAM, and the trap mechanism from M9 gains its
asynchronous half. `memory.c` stops being purely a flat array: physical-address
windows now dispatch to device models in `src/device.{h,c}`, on the de-facto qemu
`virt` layout. Three devices land — a **CLINT** (the `mtime` timer with a
per-hart `mtimecmp`, and the `msip` software-interrupt/IPI register), a **PLIC**
(external-interrupt routing with per-source priority, an enable bitmap, a
threshold, and the claim/complete handshake), and a **16550 UART** whose transmit
register prints to the host console. The CPU pulls the resulting pending bits
(`MTIP`/`MSIP`/`MEIP`) into `mip` each step and, at the instruction boundary
before fetch, delivers the highest-priority enabled interrupt: `take_interrupt`
honours the global `mstatus.MIE`/`SIE` gates and `mideleg`, and `enter_trap` (now
shared with the synchronous `raise_trap`) supports vectored `*tvec`. `mtime`
advances one tick per step, so timers are deterministic. The platform is attached
to every loaded machine but inert until a guest programs it — no pre-M13 test
enables an interrupt, so all keep running unchanged. `tests/test_irq.S` arms the
timer, raises an IPI, and routes a UART interrupt through the PLIC (claim →
deassert → complete), checking each fires exactly once, then prints through the
UART; `make check` pins the interrupt assertions and `make check-devices` pins the
console output. Being machine-mode + MMIO, it stays out of `make check-diff`.

- [x] **Build:** MMIO dispatch in the memory layer plus a CLINT (`mtime`/
  `mtimecmp` timer, `msip` IPI), a PLIC (external-interrupt claim/complete), and
  a 16550 UART for console I/O.
- [x] **ISA:** none new — memory-mapped device models and interrupt delivery.
- [x] **Concept:** memory-mapped I/O; the interrupt path from device to trap;
  timer-driven preemption.
- [x] **Done when:** a timer interrupt fires on `mtimecmp` and the UART prints.
- [x] **Commits:** `feat: add mmio dispatch`, `feat: add clint timer and ipi`,
  `feat: add plic`, `feat: add a 16550 uart`.

## M14 — Device tree and boot protocol (DONE)

The boot handoff: how a kernel learns what hardware it is running on. Instead of
assuming fixed addresses, a RISC-V system enters its OS with the boot hart id in
`a0` and the physical address of a *flattened device tree* (DTB) in `a1` — a
self-describing blob, built by firmware (OpenSBI/qemu), that lists the RAM ranges
and the memory-mapped devices and how their interrupts are wired. `src/dtb.{h,c}`
adds a from-scratch FDT serialiser: `dtb_build` emits the standard binary form (a
big-endian header, a memory-reservation block, a structure block of nested
nodes/properties, and a deduplicated strings block) with no external `dtc`. It
describes Quanta's own RAM (`/memory`) and the M13 platform (`/soc` with the
CLINT, PLIC, and 16550 UART, plus the per-hart interrupt controller and phandle
wiring). `quanta_load_elf` now generates this tree, drops it at the top of guest
memory (in the loader's stack headroom, with `sp` moved just below it), and sets
`a0`/`a1` per the convention; `quanta_dtb_addr` exposes its address and the CLI
banner reports it. The raw-image/demo path is unchanged (no tree, `a0`/`a1` = 0),
so the boot protocol is inert for hand-assembled embeds. `tests/test_dtb.S` plays
bootloader: it walks the structure block token by token straight from `a1`,
reads the `/memory` reg range back out and confirms it contains the program, and
finds the UART device node — proving the guest can discover its layout from the
DTB. Being a Quanta-supplied boot artifact user-mode qemu does not provide, it is
kept out of `make check-diff`; `make check` pins it (and `make check-disasm`
covers its disassembly).

- [x] **Build:** load (or generate) a flattened device tree and enter the guest
  per the RISC-V boot convention (`a0`=hartid, `a1`=DTB pointer).
- [x] **ISA:** none new — the firmware/OS boot contract.
- [x] **Concept:** hardware discovery via device tree; the boot handoff.
- [x] **Done when:** the guest reads its memory layout and devices from the DTB.
- [x] **Commits:** `feat: pass a device tree at boot`,
  `docs: document the boot protocol`.

### Stage 3 — Boot an OS (RV32 trophy) (M15–M16)

## M15 — Bare-metal S-mode + SBI

- [ ] **Build:** an SBI implementation (timer, console putchar/getchar, hart
  ops) so S-mode software has a firmware interface — or run OpenSBI in emulated
  M-mode.
- [ ] **ISA:** none new — the SEE/SBI contract over ECALL.
- [ ] **Concept:** the supervisor/firmware boundary; what an SEE provides.
- [ ] **Done when:** a bare-metal S-mode program prints via the SBI console and
  exits.
- [ ] **Commits:** `feat: add an sbi implementation`,
  `test: boot a bare-metal s-mode program`.

## M16 — Boot a small RV32 OS

- [ ] **Build:** boot a real RV32 kernel — a teaching kernel or a buildroot RV32
  Linux + initramfs — through to userspace.
- [ ] **ISA:** none new — the integration milestone for Stages 1–2.
- [ ] **Concept:** what "booting" entails end to end; where emulator bugs hide
  that unit tests miss.
- [ ] **Done when:** the guest boots to a shell or runs a userspace process.
- [ ] **Commits:** `test: boot a small rv32 os`, `docs: document booting an os`.

### Stage 4 — Mainstream OS (triggers RV64) (M17–M19)

## M17 — RV64GC transition

- [ ] **Build:** parameterise XLEN (a width-agnostic register/ALU/decode path),
  taking the core to RV64. The Spike differential harness (E5) is the safety net
  for the whole refactor.
- [ ] **ISA:** RV64I/M/A/C (and F/D from M11) — the GC base.
- [ ] **Concept:** XLEN parameterisation; why the world standardised on RV64GC.
- [ ] **Done when:** the RV64 conformance and differential suites pass.
- [ ] **Commits:** `refactor: parameterise xlen`, `feat: add rv64 base`,
  `test: rv64 conformance and differential`.

## M18 — Sv39 + boot a mainstream OS

- [ ] **Build:** the three-level Sv39 page-table scheme; then boot xv6-riscv and
  a standard Linux + OpenSBI.
- [ ] **ISA:** Sv39 address translation.
- [ ] **Concept:** deeper page-table hierarchies; a real distro's boot
  requirements.
- [ ] **Done when:** xv6-riscv reaches its shell; Linux boots to userspace.
- [ ] **Commits:** `feat: add sv39 paging`, `test: boot xv6-riscv`,
  `test: boot linux`.

## M19 — SMP multi-hart (stretch)

- [ ] **Build:** multiple harts with CLINT IPIs, exercising the M10 atomics
  under real contention.
- [ ] **ISA:** none new — multi-hart coordination.
- [ ] **Concept:** shared memory, memory ordering, and inter-hart interrupts.
- [ ] **Done when:** an SMP guest boots and schedules across harts.
- [ ] **Commits:** `feat: add smp multi-hart support`.

## How to use this roadmap

- Do one milestone at a time; keep `main` runnable at every step.
- Land each milestone as a single commit (code + tests + docs together); the
  per-milestone **Commits:** lines are an illustrative breakdown of the work, not
  a target to split commits along.
- In Part II, keep the E-line green continuously and let the Spike differential
  harness (E5) gate every ISA change; advance the M-line one milestone at a time.
- Write the test before or alongside the feature where M3+ calls for it.
- When a milestone is done, tick its boxes and note anything surprising in
  `CLAUDE.md` so future sessions inherit it.
- The concept line is the point — if a milestone works but the concept hasn't
  clicked, that's the signal to slow down and read the spec section before
  moving on.
