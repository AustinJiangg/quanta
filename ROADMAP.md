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

## M11 — Optional extensions: RV32C done, RV32F/D deferred

RV32C is in: the compressed extension, the first to break the project's
one-instruction-is-four-bytes assumption. Rather than teach the executor and
disassembler a second format, `src/rvc.c` *expands* — a 16-bit instruction is
widened to the exact 32-bit instruction it abbreviates, and the existing
decode/execute and disassembly paths handle it unchanged, so the expander is the
single source of truth shared by both. The fetch in `cpu.c` now reads a halfword
first, branches on its low two bits (0b11 = a 32-bit instruction whose upper half
may sit in the next page; otherwise a compressed one), and advances the PC by the
instruction's true length — which also fixed the `+4` baked into the branch
fall-through and the JAL/JALR link, now the actual `ilen` (2 or 4). Alignment
relaxes to IALIGN=16 (only odd targets fault), and `misa` advertises C.
`tests/test_rvc.S` checks every compressed instruction's semantics and, being
plain user-mode code, is differential-tested against qemu (which also implements
C); `make check-disasm` pins the compressed disassembly against objdump, which
renders the same expanded mnemonics. RV32F/D stay deferred — they need an `fcsr`,
rounding modes, and a softfloat op set for host-independent results, and no
chosen guest demands them yet.

- [x] **Build:** RV32C (expand the 16-bit encodings). RV32F/D (an `fcsr`,
  rounding modes, the F/D op set via softfloat) remain deferred.
- [x] **ISA:** RV32C. (RV32F, RV32D deferred.)
- [x] **Concept:** compressed encodings and code density; variable-length fetch
  and the IALIGN=16 alignment relaxation. (The float register file and the
  hard-float vs soft-float ABI split come with F/D.)
- [x] **Done when:** as the chosen guest demands — RV32C lands now (most kernels
  build with it); userspace soft-float defers F/D until a glibc GC guest needs it.
- [x] **Commits:** `feat: add rv32c compressed`. (`feat: add rv32f/d float` later.)

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

## M15 — Bare-metal S-mode + SBI (DONE)

The supervisor/firmware boundary: how an OS reaches the machine without owning
M-mode. A supervisor-mode kernel does not program the lowest-level hardware
itself; it calls down to the firmware (M-mode) through the **SBI** — a
register-based RPC layered on `ecall` (extension id in `a7`, function id in `a6`,
args in `a0`-`a5`, an `(error, value)` pair back in `a0`/`a1`). `src/sbi.{h,c}`
implements that firmware side from scratch: the Base extension (spec version,
implementation id, `probe_extension`), legacy console putchar/getchar, the TIME
`set_timer`, HSM `hart_get_status`, and SRST `system_reset`/legacy shutdown
(which stops the machine via `HALT_EXIT`). The hook is one line in the SEE: a
trap with no guest M-mode handler (`mtvec` still 0 — `mtvec` belongs to the
firmware, which here is Quanta) routes an **S-mode** `ecall` to `sbi_call`, while
M/U-mode `ecall`s keep reaching the newlib syscall layer as before. So existing
bare programs are untouched, and a guest that drops to S-mode gets a real
firmware interface. `tests/test_sbi.S` is the bare-metal S-mode program: it
`mret`s into Supervisor mode, probes the Base extension, arms `set_timer`, prints
"sbi ok" through the SBI console, and shuts down via SRST — a clean exit pinned
by `make check`, the console output by `make check-sbi`. Being S-mode + SBI
(which user-mode qemu does not provide), it stays out of `make check-diff`.
Running OpenSBI in emulated M-mode was the alternative; implementing a small SBI
directly keeps the from-scratch ethos and needs no external firmware blob.

Supervisor-*timer* delivery — the firmware relaying the machine timer to the OS
as a supervisor timer interrupt (STIP) — landed next as an enabler for the
OS-boot milestones: `cpu_arm_supervisor_timer` records the SBI deadline, and a
per-step `firmware_timer_tick` raises STIP once the CLINT reaches it, which an
S-mode OS takes at `stvec` when it has delegated (`mideleg`) and enabled it.
`tests/test_stimer.S` drives a tick loop through it — three supervisor timer
interrupts, each re-armed via SBI `set_timer`, then a clean SRST shutdown.

- [x] **Build:** an SBI implementation (timer, console putchar/getchar, hart
  ops) so S-mode software has a firmware interface — or run OpenSBI in emulated
  M-mode.
- [x] **ISA:** none new — the SEE/SBI contract over ECALL.
- [x] **Concept:** the supervisor/firmware boundary; what an SEE provides.
- [x] **Done when:** a bare-metal S-mode program prints via the SBI console and
  exits.
- [x] **Commits:** `feat: add an sbi implementation`,
  `test: boot a bare-metal s-mode program`.

## M16 — Boot a small RV32 OS (DONE)

The integration milestone: a small teaching kernel (`tests/os/`) that boots on
Quanta and runs a userspace process, exercising everything M8–M15 built at once —
which is where bugs the per-feature tests miss would surface (none did; the
pieces had each been pinned). Rather than port a third-party kernel, it is
written from scratch, keeping the project's ethos (the same call made for the SBI
in M15) and ensuring every bug it could hit is Quanta's own, not a port's. A
mainstream RV32 Linux is deferred: it needs a Linux/glibc cross-toolchain (only a
newlib bare-metal one is to hand) and far more RAM and run length — and the
roadmap already routes mainstream OSes through the RV64 transition (M17/M18).

The one engine prerequisite was a way to give a guest more RAM than its own
image: `elf_load` grew a `min_size`, surfaced as `quanta_load_elf_ex` and the
CLI's `--memory=SIZE` flag, so the loader leaves spare RAM above the image for
the kernel to manage — and the boot DTB's `/memory` node reports the true size,
so the guest discovers it. The kernel is `boot.S` (an M-mode stub that delegates
user traps to S-mode, leaves `mtvec` 0 so its own SBI `ecall`s reach Quanta, and
`mret`s into `kmain`; plus the S-mode trap vector and the user-entry trampoline),
`kernel.c` (the C kernel), `user.S` (a position-independent U-mode blob the kernel
copies into a page), and `kernel.ld`. `kmain` reads its RAM from the device tree
(M14), hands out physical pages from the spare RAM, builds an Sv32 address space
(M12 — megapage identity map for the kernel and the CLINT/UART MMIO, plus a
user code and stack page mapped low with the U bit), installs an `stvec` handler,
sets `sstatus.SUM` so it can read the user's buffers, arms a periodic deadline via
SBI `set_timer` (M15), and `sret`s into user mode (M9). The user prints with the
`write` syscall, is preempted three times by the supervisor timer (M13/M15), then
`exit`s — and the kernel shuts the machine down via SBI `system_reset`. Console
output goes straight to the mapped 16550 UART, proving MMIO through Sv32. Pinned
by `make check-os` (and run under `make sanitize`/`make coverage`); being an
S-mode paging+MMIO guest, it stays out of `make check-diff` like the other
privileged tests.

- [x] **Build:** boot a real RV32 kernel — a teaching kernel or a buildroot RV32
  Linux + initramfs — through to userspace. *(A from-scratch teaching kernel;
  mainstream Linux deferred to the RV64 phase, M17/M18.)*
- [x] **ISA:** none new — the integration milestone for Stages 1–2.
- [x] **Concept:** what "booting" entails end to end; where emulator bugs hide
  that unit tests miss.
- [x] **Done when:** the guest boots to a shell or runs a userspace process.
- [x] **Commits:** `feat: boot a small rv32 teaching kernel`.

### Stage 4 — Mainstream OS (triggers RV64) (M17–M19)

## M17 — RV64 transition (RV64IMAC) (DONE)

The width parameterisation: the core stops being RV32-only and runs RV64 too,
selected per program from the ELF class. Rather than two builds, XLEN is a
*runtime* property (`cpu->xlen`): every register, the PC, the CSRs, and every
address are stored as 64 bits, and a single `sext_xlen` helper (applied at
`reg_write` and the PC update) keeps the Spike-style invariant that in RV32 mode
a register holds the sign-extension of its 32-bit value. So one `./quanta` runs
either width, and the executor stays mostly width-agnostic — add/compare/logic
are automatic once results re-sign-extend; only the shifts, the `*W` ops, and the
loads carry an explicit width branch. The MMU masks a VA to XLEN at its single
choke point (recovering the real address from a sign-extended RV32 register), and
RV64 runs Bare (Sv32 is an RV32 scheme; RV64's Sv39 walk is M18). The pieces:
the widened datapath (RV32 stays bit-identical, the green checkpoint proving it),
RV64 integer + `*W` + RV64M/RV64A (gated illegal in RV32), RV64C by the same
expand-to-32-bit path (C.ADDIW/C.LD/C.SD/C.LDSP/C.SDSP/C.SUBW/C.ADDW, 6-bit
shifts), the XLEN-wide CSRs and the interrupt-cause bit that moves to bit 63,
the ELF64 loader, and the width-aware disassembler, GDB stub (riscv:rv64,
64-bit register packets), and boot device tree. `tests/rv64/` is the hand-written
RV64 conformance suite — the `*W` ops, LD/SD, 6-bit shifts, RV64M/RV64A, RV64C,
and a privileged trap test (a 64-bit CSR round-trip that would truncate on RV32);
the user-mode ones are differential-tested against **qemu-riscv64** (`make
check-rv64`), the golden-model net the roadmap called for. RV32F/D stay deferred
(as in M11), so this is RV64IMAC, not the full GC — the "G" (F/D) and the RV64
Sv39 paging both wait, F/D until a guest needs it and Sv39 for M18.

- [x] **Build:** parameterise XLEN (a width-agnostic register/ALU/decode path),
  taking the core to RV64. The qemu-riscv64 differential harness (E5) is the
  safety net for the whole refactor. *(Runtime XLEN, single binary.)*
- [x] **ISA:** RV64I/M/A/C — the IMAC base. *(F/D deferred with M11.)*
- [x] **Concept:** XLEN parameterisation; why the world standardised on RV64.
- [x] **Done when:** the RV64 conformance and differential suites pass
  (`make check-rv64`; RV32 stays green).
- [x] **Commits:** `refactor: parameterise xlen`, `feat: add rv64 base`,
  `test: rv64 conformance and differential`.

## M18 — Sv39 + boot a mainstream OS (DONE)

Sv39 is in — the three-level page-table scheme RV64 needs, and the second half of
the M17 "RV64 runs Bare" gap. Rather than a second walker, `mmu.c`'s existing
Sv32 walk was *generalised*: the two schemes are the same walk at different
sizes, so one loop now serves both, parameterised by a small descriptor (table
depth, PTE width, VPN-field width, PPN mask) chosen from `satp.MODE`. Sv32 stays
2 levels / 4-byte PTEs / 22-bit PPN; Sv39 is 3 levels / 8-byte PTEs / 44-bit PPN.
The superpage merge is uniform across both (a leaf above the last level takes its
low VPN fields from the VA, and the PTE's matching PPN bits must be zero), so it
covers the Sv32 4 MiB megapage and the Sv39 2 MiB megapage / 1 GiB gigapage with
the same three lines; the TLB, permission, A/D-writeback, and page-fault paths
are shared unchanged. Sv39 adds one front-of-walk check the narrower scheme does
not need: the VA must be canonical (bits [63:39] a sign-extension of bit 38) or
it faults. `satp.MODE` is now enforced WARL — `csr_write` drops a write selecting
an unsupported scheme (Sv48/Sv57) via `mmu_satp_supported`, so a guest probing
for the widest mode (Linux tries Sv57, then Sv48, then Sv39) sees the ones we do
not model not stick. The refactor kept every RV32 net bit-for-bit green (the Sv32
`test_vm`, `check-arch`, `check-os`) — the safety checkpoint. `tests/rv64/
test_rv64_vm.S` is the Sv39 conformance test: it hand-builds a three-level table,
proves non-identity translation through the full walk (two VAs aliased to one
frame), the hardware dirty bit, and a load page fault from a VA whose walk
reaches the last level with no leaf — the RV64 analogue of the Sv32 `test_vm.S`,
quanta-only (S-mode + satp, so out of the qemu differential).

**OS-boot work — xv6-riscv scoped, first enablers landing.** The mainstream-OS
target was scoped against upstream `mit-pdos/xv6-riscv`, and it fits Quanta well:
xv6 builds with `-bios none`, so qemu enters it *in M-mode at 0x80000000* — exactly
how Quanta enters every ELF — and it drops to S-mode itself in `start.c`, needing
neither OpenSBI nor even Quanta's SBI. Its devices are the qemu `virt` CLINT/PLIC/
UART Quanta already models, its 128 MiB fits `--memory`, and its PMP writes are
inert against our unenforced-PMP model (they only grant S-mode full access, which
we already allow). The gaps, in order: **(1) the Sstc supervisor-timer extension**
— xv6's `start.c` sets `menvcfg.STCE` and arms `stimecmp` directly rather than
using a firmware relay; **DONE** (`sstc_tick` in cpu.c, `tests/rv64/test_rv64_sstc.S`).
**(2)** UART receive (host stdin → the guest's RX path) and a `--disk=fs.img`
backend; **DONE** (`plat_uart_rx`/`quanta_uart_input` + `main.c`'s stdin pump, and
`quanta_attach_disk` staging a raw image in `Platform.disk`; `make check-uart-rx`).
**(3)** the crux — a **virtio-mmio (modern, v2) block device** with one split
virtqueue, since xv6's root filesystem (and thus `/init` and the shell) lives on
a virtio disk; **DONE** (`src/device.c`'s virtio model, serving the `--disk`
image; `tests/rv64/test_rv64_virtio.S`, `make check-virtio`). The driver is
deterministic-friendly, exactly as planned: it processes the descriptor chain
synchronously on `QUEUE_NOTIFY`, then asserts PLIC IRQ 1 — safe because xv6 holds
`vdisk_lock` with interrupts off until it sleeps. The device is a bus master, so
the platform now carries a pointer to guest RAM (`plat_attach_ram`) for DMA.
**xv6-riscv now boots to its shell.** Built integer-only (`rv64imac_zicsr`, no
F/D) with `CPUS=1`, upstream `mit-pdos/xv6-riscv` boots on Quanta all the way to
an interactive shell — `ls` reads the filesystem off the virtio disk, `echo`
runs, processes fork/exec through Sv39 paging, and the console echoes host input.
Getting there took four Quanta changes beyond the M18 devices: **(a)** a genuine
RV64 bug fix — `exec_branch` compared the low 32 bits of the operands, so a
`bgeu`/`bltu` on values differing above bit 31 (xv6's page-table teardown loop
runs over high user VAs like `0x3ffffff000`..`0x4000000000`) was decided wrong;
now it compares the full XLEN; **(b)** the PLIC gained its **S-mode context**
(context 1) driving **SEIP** — an S-mode OS claims/completes device interrupts
through the context-1 registers (enable `0x2080`, threshold/claim `0x201xxx`),
not the M-mode context `test_irq.S` used; **(c)** the 16550 UART's THR-empty
interrupt became a one-shot (set on THR write / TX-int enable, cleared by reading
IIR) so an always-empty transmitter no longer storms an OS that leaves TX
interrupts on; **(d)** a `--max-steps=N` flag (0 = uncapped) lets an interactive
guest run past the runaway guard. The boot DTB's `mmu-type` for RV64 is now
`riscv,sv39`. `tests/rv64/test_rv64_plic.S` pins the S-mode external-interrupt
path and the branch fix is pinned by `tests/rv64/test_rv64.S` (qemu-verified).
The interactive console is now clean: when stdin is a tty the run puts it in raw
mode (the qemu `-nographic` recipe — character-at-a-time, no host echo, Ctrl-C
and flow-control keys delivered to the guest, `Ctrl-A x` to quit) and restores it
on every exit path (after the loop, via `atexit`, and from signal handlers), so a
guest's shell reads and echoes exactly once with no line buffering.

**Toward Linux: the OpenSBI firmware path.** The mainstream path runs a real
M-mode firmware that hands off to an S-mode OS (as opposed to xv6, which owns
M-mode itself). `quanta --bios=<opensbi> --kernel=<image>` now boots that way
(`quanta_load_firmware`): it loads OpenSBI's **fw_dynamic** build (qemu ships one)
at 0x80000000 and the raw OS image at 0x80200000, and enters the firmware with a
`fw_dynamic_info` descriptor in `a2` directing it into S-mode. Two placement rules
made it work — the DTB is parked with headroom (OpenSBI expands the FDT in place),
and the descriptor sits just below it. **Upstream OpenSBI v1.3 boots on Quanta**:
full platform init from Quanta's device tree (`uart8250`, `aclint-mtimer/mswi`,
64 PMP entries) and a clean hand-off to an S-mode payload, which prints through
the SBI console (an `ecall` serviced by OpenSBI *on Quanta*) and powers off via
SBI SRST — which needed a **SiFive test finisher** device (0x100000) for clean
shutdown. Quanta's own `sbi.c` is bypassed on this path: OpenSBI is the SBI, so
Quanta only has to be a correct M-mode machine, which it already was (no CSR or
instruction gaps surfaced). `tests/opensbi_payload.S` + `make check-opensbi` pin
it. **And it works:** a mainline **Linux 6.6** `Image` (built rv64imac, no
float/vector) boots on Quanta through OpenSBI all the way to userspace launch —
SBI brought up (TIME/IPI/RFENCE/SRST/HSM), console via `earlycon`, Sv39 paging,
memory zones, `Machine model: quanta,virt`, PID 1 — panicking only at "unable to
mount root fs" with no rootfs. Reaching it took the new `--append` kernel-cmdline
flag and, decisively, **one genuine CPU bug fix**: JALR wrote its link register
before computing the target from the base, so a `jalr rd,off(rd)` (`rd == rs1`,
the far-`call` thunk a linker emits beyond ~2 MiB) jumped to garbage — invisible
to xv6/OpenSBI/the suite (all small enough to relax every call to `jal`), fatal to
a 22 MiB kernel whose cross-section calls broke, so paging silently never came up.

**Linux reaches a userspace shell.** The last piece was an initramfs: a new
`--initrd=FILE` flag stages a cpio archive in RAM below the DTB and advertises it
to the kernel via `/chosen` `linux,initrd-start`/`-end` (the way qemu's `-initrd`
does), so the kernel unpacks it as its root filesystem and runs `/init`. That
`/init` (`tests/linux/init.c`) is a freestanding RV64 program — no libc, every
action a raw Linux `ecall` — that drives a tiny line shell over the serial console
and powers the machine off (reboot syscall → SBI SRST) on command. It is packed
with a self-contained newc-cpio builder (`tests/linux/mkcpio.c`, no `cpio`/root
needed, since the archive carries the `/dev/console` device node the kernel opens
as PID 1's console). **Linux 6.6 now boots all the way to an interactive
userspace shell** on Quanta: `Unpacking initramfs` → `Run /init as init process`
→ a `quanta$` prompt that echoes typed commands through the kernel tty and shuts
down cleanly. `make linux-initramfs` builds the image; see `tests/linux/README.md`.

- [x] **Build (paging):** the three-level Sv39 page-table scheme (a
  descriptor-parameterised generalisation of the Sv32 walk).
- [x] **Build (OS boot):** boot xv6-riscv. *Done:* Sstc timer, UART receive, the
  `--disk` backend, the virtio-mmio block device, the PLIC S-mode context/SEIP,
  the UART THRE one-shot, `--max-steps`, and the full-XLEN branch fix — xv6 boots
  to its shell.
- [x] **Build (Linux):** boot Linux under OpenSBI to a userspace shell. *Done:*
  the `--bios`/`--kernel` firmware-boot path, the fw_dynamic handoff, the
  DTB-headroom placement, the SiFive test device, the `--append` kernel cmdline,
  the JALR far-call fix, and the `--initrd` cpio initramfs (+ the freestanding
  `/init` and its self-contained packer) — **Linux 6.6 boots to an interactive
  userspace shell.**
- [x] **ISA:** Sv39 address translation; Sstc (`stimecmp`/`menvcfg.STCE`).
- [x] **Concept:** deeper page-table hierarchies; a real distro's boot
  requirements.
- [x] **Done when:** xv6-riscv reaches its shell — *and* Linux 6.6 reaches an
  interactive userspace shell via an initramfs.
- [x] **Commits:** `feat: add sv39 paging`, `feat: add sstc supervisor timer`,
  `feat: add virtio-mmio block device`, `feat: boot xv6-riscv`,
  `feat: boot linux under opensbi`, `feat: boot linux to a userspace shell`.

## M19 — SMP multi-hart (DONE)

Quanta now models up to `QUANTA_MAX_HARTS` (8) harts sharing one memory and one
platform, chosen with `--harts=N`. Concurrency is modelled deterministically: a
single-threaded round-robin scheduler in the engine interleaves the harts one
instruction at a time, so runs stay reproducible (no host threads, no wall-clock
nondeterminism) while still exposing real interleaving to the guest. The pieces:

- **Per-hart CLINT** — `msip[h]`/`mtimecmp[h]` arrays on the qemu virt map, so a
  hart IPIs another by writing its `msip` and each hart has its own timer compare;
  the shared `mtime` ticks once per scheduler round (one rate regardless of hart
  count).
- **Per-hart PLIC contexts** — context `2h`/`2h+1` drive hart `h`'s MEIP/SEIP, so
  external interrupts route to the right hart's M- or S-mode.
- **Per-hart interrupt state** — `plat_mip_bits(p, hart)` and the trap path key
  off `cpu->hartid`; `mhartid` reads the hart's id, delivered in `a0` at boot.
- **Cross-hart atomics** — a store on any hart (a plain store, an AMO, *or a
  successful `sc`*) voids every other hart's LR/SC reservation to that word — the
  contention LR/SC exists to handle. Fixing the `sc` case (it was clearing only
  its own reservation) was a real correctness bug the SMP test caught.
- **Boot** — the direct ELF/image path brings up every hart at the entry with
  `a0`=hartid (the qemu `-bios none` convention), so an SMP guest dispatches on
  `mhartid`. (The firmware `--bios` path parks the secondaries; SMP Linux under
  OpenSBI, which needs SBI HSM `hart_start`, is future work.)
- **Device tree** — one `cpu@h` node per hart with its own interrupt-controller
  phandle, and CLINT/PLIC `interrupts-extended` wiring every hart.

Pinned by `tests/rv64/test_rv64_smp.S` + `make check-smp`: four harts each verify
their `mhartid`, hammer one shared counter with LR/SC under contention (the total
must land at `NHARTS*ITERS` with no lost updates — a broken reservation loses
some), sync on a barrier, and pass a CLINT IPI hart-to-hart as a real machine
software interrupt. And the trophy: **upstream xv6-riscv boots SMP** (`--harts=3`)
to its shell — `hart 1 starting` / `hart 2 starting`, processes scheduled across
harts, spinlocks (its atomics) correct under the interleaving.

- [x] **Build:** multiple harts with CLINT IPIs, exercising the M10 atomics
  under real contention.
- [x] **ISA:** none new — multi-hart coordination.
- [x] **Concept:** shared memory, memory ordering, and inter-hart interrupts.
- [x] **Done when:** an SMP guest boots and schedules across harts — the SMP
  conformance test passes and xv6-riscv boots to its shell on 3 harts.
- [x] **Commits:** `feat: add smp multi-hart support`.

---

# Part III — a complete, real, fast machine

Parts I and II took Quanta from a bare fetch loop to a machine that boots Linux
6.6 and SMP xv6. Every roadmap box through M19 / E9 is ticked. Part III keeps the
same two axes — **capability** and **engineering maturity** — but changes the
goal one more time: make Quanta a machine that is genuinely **complete** (runs
real software, not just IMAC), **real** (networks, boots a stock distribution),
and **fast** (leaves the naive interpreter behind), all without breaking the
project's contract — **C11 + standard library only, no third-party dependencies,
the interpreter stays the golden reference, and every ISA change is gated by the
qemu differential and the arch-tests.**

Three pillars carry the M-line:

- **Completeness** — finish the ISA: F/D floating point (the missing "G"), then
  bit-manipulation, then the vector extension.
- **Platform realism** — networking, SMP under real firmware, and a writable disk,
  so a stock distribution boots and runs.
- **Performance** — move from a switch-dispatched interpreter to threaded dispatch
  and then dynamic binary translation, with the interpreter as the reference.

The engineering track (E10+) continues in parallel, and it opens with the one
capability the deterministic M19 scheduler makes nearly free: **record/replay and
reverse debugging.**

Legend is unchanged (**Build / ISA / Concept / Done when / Commits**). Nothing
here is started yet; the boxes are the tracker.

---

## Engineering track (E-line, continued)

### E10 — Record/replay, snapshots, and reverse debugging

The round-robin scheduler is already fully deterministic — one host thread, no
wall-clock, `mtime` stepped per scheduler round — so a run is a pure function of
its initial state plus its external inputs (only console/UART stdin bytes; the
disk is fixed at load). That determinism makes time-travel debugging almost free,
and it is the highest-leverage tool to build *first*, because every later
milestone (softfloat, the JIT) is far easier to debug when a failing run can be
replayed and stepped backwards.

- [x] **Build (foundation):** a full-machine **snapshot/restore** primitive in the
  engine — capture every mutable byte (all harts' registers/CSRs/TLB/PC, the whole
  RAM image, the device register files, the in-memory disk, the scheduler cursor
  and machine-halt state) into an opaque `QuantaSnapshot`, and restore it into the
  same instance, re-wiring the borrowed pointers to the live objects. The
  observability-only cache is excluded (it never changes results). *Done:*
  `quanta_snapshot`/`quanta_restore`/`quanta_snapshot_free` in `quanta.c`, pinned
  by `tests/snapshot_test.c` / `make check-snapshot` — a guest run to completion,
  then snapshotted midway and replayed, must match bit-for-bit (registers, memory,
  device state, exit, and step count).
- [x] **Build (record/replay):** serialise a snapshot to a file and resume from
  it — `--snapshot=FILE` writes the whole machine state when the run ends,
  `--restore=FILE` rebuilds a machine from such a file (no program needed) and
  continues, so a capped run checkpoints and resumes bit-for-bit. *Done:*
  `quanta_save_snapshot`/`quanta_load_snapshot` in `quanta.c` (a self-describing,
  layout-signature-guarded file), the two CLI flags in `main.c`, pinned by
  `tests/check_replay.sh` / `make check-replay` (a mid-run snapshot resumes to the
  same output+exit as the whole run; a corrupt file is rejected cleanly). Because
  every tested run is deterministic, snapshot-restore alone replays exactly; a
  stdin **input-log** for replaying *interactive* console-driven runs is the one
  noted follow-up.
- [x] **Build (reverse debugging):** in the GDB stub, keep a ring of periodic
  snapshots and a monotonic machine step-count; implement `bs`/`bc` (reverse-step,
  reverse-continue) by restoring the nearest snapshot at or before the target step
  and replaying forward, so a stock `gdb` steps a booting kernel backwards. *Done:*
  `gdbstub.c` advertises `ReverseStep+`/`ReverseContinue+`, checkpoints lazily
  (only once a reverse op is used, so forward-only sessions pay nothing) with a
  step-0-pinned ring, and `goto_step` restores + replays; `tests/gdb_client.py` /
  `make check-gdb` exercise `bs` (twice, then a forward replay) and `bc` to an
  earlier breakpoint on `tests/hello.elf`.
- [x] **Why:** debugging a fault that appears billions of instructions into a boot
  is otherwise brutal; deterministic replay + reverse-step turns it into a bisect.
- [x] **Done when:** a snapshot taken mid-run and restored reproduces the exact
  final state; a recorded run replays bit-for-bit; `gdb` reverse-steps a guest.
- [x] **Commits:** `feat: add machine snapshot and restore`,
  `feat: add reverse debugging to the gdb stub`,
  `feat: serialise snapshots for record and replay`.

### E11 — WebAssembly build (Quanta in the browser)

- [ ] **Build:** compile `libquanta` to WebAssembly and wrap it in a minimal
  browser console UI, so a RISC-V Linux boots in a tab. The engine is already
  clean C with an opaque handle and no fatal `exit()`, so this is mostly build
  plumbing (a WASM target) plus a small JS console and a virtual disk.
- [ ] **Why:** a zero-install demo and a portfolio-grade showcase of the library
  split; also stress-tests the "no hidden host assumptions" property of the core.
- [ ] **Done when:** a hosted page boots a guest to a prompt with keyboard input.
- [ ] **Commits:** `feat: add a wasm build target`, `feat: browser console front-end`.

### E12 — Language bindings (Python, Rust)

- [ ] **Build:** thin FFI bindings over `libquanta.a` — a Python module (cffi/
  ctypes) and a Rust crate — exposing lifecycle, load, step/run, and register/
  memory access, so the engine can be scripted for analysis and unit-tested from a
  higher-level language.
- [ ] **Why:** scripting unlocks quick experiments, teaching notebooks, and
  property tests that are painful to write in C.
- [ ] **Done when:** a Python script loads and single-steps a guest; a Rust test
  drives one.
- [ ] **Commits:** `feat: add python bindings`, `feat: add rust bindings`.

### E13 — Multi-hart-aware GDB stub + watchpoints

- [ ] **Build:** extend the stub to be SMP-aware — per-hart threads (`Hg`/`Hc`,
  `qfThreadInfo`/`qsThreadInfo`, `T` stop replies naming the hart) and per-hart
  register access — and add hardware watchpoints (`Z2`/`Z3`/`Z4`) enforced in the
  step loop, so a data-change or a specific hart can be broken on.
- [ ] **Why:** debugging an SMP kernel needs to see and select harts; watchpoints
  catch memory corruption the current PC breakpoints cannot.
- [ ] **Done when:** `gdb` lists harts, switches between them, and breaks on a
  memory write.
- [ ] **Commits:** `feat: make the gdb stub smp-aware`, `feat: add watchpoints`.

### E14 — Interactive monitor console

- [ ] **Build:** a qemu-HMP-style monitor reached from the console escape
  (`Ctrl-A c`): pause/continue, `info registers`, dump memory, walk and print the
  page tables / TLB, inspect device state, and drive a snapshot save/restore
  (E10) — a machine-level console distinct from the guest's serial line.
- [ ] **Why:** inspecting a live machine without an external debugger; the natural
  home for the E10 snapshot controls.
- [ ] **Done when:** the monitor pauses a booting guest and prints its state.
- [ ] **Commits:** `feat: add an interactive machine monitor`.

### E15 — Guest profiling and richer performance models

- [ ] **Build:** a sampling profiler over the guest PC (symbolised through the ELF
  symbol table) reporting function-level hot spots, and richer `--pipeline`
  variants — a branch-predictor model and an optional superscalar/out-of-order
  timing overlay — extending the M6/M7 observability theme.
- [ ] **Why:** closes the loop on the learning arc (what actually costs cycles),
  and gives the JIT (M25) a baseline to beat.
- [ ] **Done when:** a profile attributes time to guest functions; the new timing
  models report defensible estimates on a hazard workload.
- [ ] **Commits:** `feat: add a guest sampling profiler`,
  `feat: add branch-predictor and superscalar models`.

### E16 — Differential fuzzing and extended conformance

- [ ] **Build:** a random-instruction-sequence generator run in per-instruction
  register lockstep against qemu/spike (extending E5 from whole-program to
  instruction granularity), and extend `make check-arch` to the F/D, C, B, and V
  arch-test families as those extensions land.
- [ ] **Why:** whole-program differential can hide a bug two instructions cancel;
  lockstep and the official per-extension suites are the stronger nets.
- [ ] **Done when:** the lockstep fuzzer runs in CI; the new arch-test families
  pass for each implemented extension.
- [ ] **Commits:** `test: add differential instruction fuzzing`,
  `test: extend arch-test coverage`.

---

## Capability track — completeness, realism, speed

### Stage 5 — Finish the ISA (M20–M21, M26)

## M20 — RV32/64 F and D floating point (the missing "G") (DONE)

Quanta was IMAC; the F and D extensions close the gap to "GC". Because IEEE-754
is unforgiving and a host-independent result cannot lean on the host FPU (which
may keep 80-bit x87 intermediates, contract `a*b+c`, or round differently), the
core is a **from-scratch softfloat** (`src/softfloat.{h,c}`) — no Berkeley
SoftFloat dependency, keeping the no-dependency contract. One generic core
parameterised by a small format descriptor serves both binary32 and binary64:
`unpack → operate → round`, where `round` normalises, applies the rounding mode,
and detects overflow/underflow/inexact via the packToF additive-carry trick.
Wide intermediates (a 106-bit double product, a divided/rooted significand, and
the exact fused-multiply-add sum that can cancel across ~160 bits) use two-word
128-bit and four-word 256-bit helpers rather than a compiler `__int128`, staying
portable C11. Division and square root are exact integer long-division / bit-wise
`isqrt` (no reciprocal tables). The library was validated **before** wiring it in
by an exhaustive host-FPU oracle — 90M random+edge cases across add/sub/mul/div/
sqrt/fma/conversions in the four host-mappable rounding modes, bit-exact on
results and the NV/DZ/OF/NX flags — then the RISC-V-specific semantics (the
canonical NaN, saturating float→int, RMM, NaN-boxing, and the exact flags
including UF) were pinned end-to-end against qemu-riscv64. `cpu.c` gained the
`f0`–`f31` register file (single-precision NaN-boxed into the high half), the
`fcsr`/`frm`/`fflags` CSR views, `exec_load_fp`/`exec_store_fp`/`exec_fp`/
`exec_fmadd`, and dynamic rounding-mode resolution; `rvc.c` expands the
compressed float loads/stores; `misa` advertises F and D.

- [x] **Build:** a float register file (`f0`–`f31`, 32×64-bit for D, single-
  precision NaN-boxed into the low half), `fcsr` (the `frm` rounding mode and the
  `fflags` accrued-exception bits), and a self-contained `src/softfloat.{h,c}`
  implementing add/sub/mul/div/sqrt, fused multiply-add (single rounding), int↔
  float conversions with correct saturation, sign-injection, NaN-aware min/max,
  compares, and classify — honouring all five rounding modes, subnormals,
  signalling-vs-quiet NaNs, the canonical NaN, and the five exception flags.
- [x] **ISA:** RV32F, RV32D, RV64F, RV64D — the load/store, arithmetic, FMA,
  FCVT, FSGNJ, FMIN/MAX, FEQ/FLT/FLE, FCLASS, FMV families (plus RV64's
  FCVT.L/LU), and the RV32C/RV64C float loads/stores. `misa` advertises `fd`; the
  boot DTB isa string is left `rv64imac` on purpose so the imac-built xv6/Linux
  guests are not perturbed into probing float (a gc guest supplies its own DTB).
- [x] **Concept:** IEEE-754 semantics; NaN-boxing; the hard-float ABI; why a
  software float implementation is needed for a deterministic, host-independent
  emulator.
- [x] **Done when:** the float programs agree with qemu-riscv64 bit-for-bit — the
  hand-written `tests/rv64/test_rv64_fpu.S` (44 checks spanning arithmetic, FMA,
  conversions/saturation, min/max/compare NaN rules, classify, moves, flags, and
  rounding modes) runs green under `make check-rv64`'s qemu differential, backed
  by the 90M-case host-FPU oracle. (Extending `make check-arch` to the official
  F/D arch-test families is E16; a full `-march=rv64gc` distribution boot — which
  also needs FS gating and a gc DTB — is the Stage-6 M24 trophy.)
- [x] **Commits:** `feat: add rv32/64 f and d floating point`.

A deliberate leniency: Quanta tracks `mstatus.FS` (a float write marks it Dirty +
SD) but does **not** trap when FS is Off. Gating would force the conformance test
to set `mstatus.FS` — an M-mode CSR user-mode qemu rejects — losing the golden
differential net, the single most valuable check for IEEE edge cases. Permissive
execution keeps the test user-mode and qemu-checkable while a full-system guest
still sees the dirty state it needs for lazy context switching.

## M21 — Bit manipulation (Zba/Zbb/Zbs/Zbc) (DONE)

The bit-manipulation extensions modern toolchains and the kernel increasingly
emit — mostly self-contained ALU work, a cheap and high-value follow-on to the
float milestone. They reuse the existing OP / OP-IMM / OP-32 / OP-IMM-32 opcodes
and are picked out of the base decode by their funct7 / funct6 / funct3 (and, for
the unary Zbb ops, the rs2 field), so rather than a new decoder each width gets
four small `exec_bitmanip_*` intercepts in `cpu.c` that run before the base
switch and return 0 to fall through — including the funct7 == 0x20 slots Zbb's
andn/orn/xnor share with the base SUB/SRA. The core is width-agnostic (results
run through `reg_write`, which re-sign-extends to XLEN); only the ops whose
definition names a width — the *W word ops, the `.uw` zero-extends, and the
whole-register scans clz/ctz/cpop/rev8/orc.b — branch on `cpu->xlen`. Carry-less
multiply uses a portable two-word 128-bit product (`bm_clmul128`, no `__int128`),
from which clmul/clmulh/clmulr slice the low/high/reversed field. The
disassembler mirrors the decode and matches binutils exactly (pinned by
`check-disasm`), and `misa` now advertises `B`.

- [x] **Build:** Zba (`sh1add`/`sh2add`/`sh3add`, `add.uw`, `sh{1,2,3}add.uw`,
  `slli.uw`), Zbb (`andn`/`orn`/`xnor`, `clz`/`ctz`/`cpop`(+`w`), `min`/`max`(`u`),
  `sext.b`/`sext.h`/`zext.h`, `rol`/`ror`(`i`) and the `w` forms, `orc.b`, `rev8`),
  Zbs (`bclr`/`bext`/`binv`/`bset` and immediates), and Zbc (carry-less multiply
  `clmul`/`clmulh`/`clmulr`), mirrored in the disassembler and advertised in the
  isa string.
- [x] **ISA:** Zba, Zbb, Zbs, Zbc (RV32 and RV64 widths).
- [x] **Concept:** why bit manipulation is a common extension — the crypto,
  hashing, and bignum kernels it accelerates.
- [x] **Done when:** a hand-written suite passes and agrees with qemu — the RV64
  suite (`tests/rv64/test_rv64_bitmanip.S`, 43 checks, including the *W/.uw forms
  and 6-bit shift immediates) runs green under `make check-rv64`'s qemu-riscv64
  differential, and the RV32 suite (`tests/test_bitmanip.S`, 32 checks) is pinned
  by `make check`, the qemu-riscv32 differential (`make check-diff`), and objdump
  (`make check-disasm`). (Extending `make check-arch` to the official B family is
  E16.)
- [x] **Commits:** `feat: add zba/zbb/zbs/zbc bit manipulation`.

### Stage 6 — Platform realism, boot a distribution (M22–M24)

## M22 — SMP under firmware (SBI HSM) (DONE)

The `--bios` firmware path parked the secondary harts (M19); booting Linux SMP
under OpenSBI needs the harts to come up through the SBI hart-start protocol —
now done, so **Linux 6.6 boots SMP on Quanta** (`--harts=4`, all four CPUs
online, the scheduler running across them).

- [x] **Build:** the SBI **HSM** extension
  (`hart_start`/`hart_stop`/`hart_suspend`/`hart_get_status`) in Quanta's own
  `sbi.c`, so a from-scratch SMP kernel that uses Quanta-as-firmware brings its
  secondaries down and up. A stopped hart's round-robin slot is a no-op until
  `hart_start` re-enters it in S-mode (a0=hartid, a1=opaque, satp=Bare, SIE off);
  `hart_stop` parks the caller; `hart_get_status` reports the state machine. This
  also flushed out a real RV64 SBI bug: error returns must be **sign-extended**
  XLEN longs (`sbi_return` cast a0 to `int32_t`→`uint32_t`, so an RV64 caller saw
  `0x00000000fffffffd`, not `-3`).
- [x] **Build (Linux SMP under firmware):** on the `--bios` firmware path, bring
  **every** hart into the firmware at reset (a0=hartid, a1=DTB, a2=fw_dynamic_info)
  instead of parking the secondaries — OpenSBI's boot-hart lottery cold-boots one
  and the rest fall into its HSM wait loop, released by Linux via SBI `hart_start`
  (a CLINT IPI). The one machine-model gap this exposed was **AIA `mtopi`/`stopi`**
  (Smaia/Ssaia top-interrupt CSRs): qemu's prebuilt OpenSBI detects these CSRs
  present (our lenient CSR file answered them instead of trapping) and drives its
  **entire M-mode interrupt dispatch off `mtopi`** — read the top interrupt,
  service it, re-read until zero. With `mtopi` unimplemented it read 0, so every
  inter-processor interrupt (the machine software interrupt that wakes/reschedules
  a secondary) was seen as "nothing pending": OpenSBI returned without clearing the
  CLINT `msip`, and the MSI re-fired forever — the "livelock" (really an IPI
  storm). It was invisible on a uniprocessor because a single hart takes no IPI and
  uses Sstc for its timer, so the `mtopi` dispatch path was dead code. Implementing
  `mtopi`/`stopi` as read-only views of the highest-priority pending-and-enabled
  interrupt (AIA default priority, IID in bits [27:16]) fixed it, matching qemu.
- [x] **Build (SBI HSM):** the SBI **HSM** extension
  (`hart_start`/`hart_stop`/`hart_suspend`/`hart_get_status`) in Quanta's own
  `sbi.c`, so a from-scratch SMP kernel that uses Quanta-as-firmware brings its
  secondaries down and up. A stopped hart's round-robin slot is a no-op until
  `hart_start` re-enters it in S-mode (a0=hartid, a1=opaque, satp=Bare, SIE off);
  `hart_stop` parks the caller; `hart_get_status` reports the state machine. This
  also flushed out a real RV64 SBI bug: error returns must be **sign-extended**
  XLEN longs (`sbi_return` cast a0 to `int32_t`→`uint32_t`, so an RV64 caller saw
  `0x00000000fffffffd`, not `-3`).
- [x] **ISA:** the AIA Smaia/Ssaia `mtopi`/`stopi` CSRs; the hart bring-up / HSM
  protocol.
- [x] **Concept:** hart bring-up, the boot-hart lottery, the HSM state machine, and
  how firmware feature-detection (a lenient CSR file mis-answering a probe) can
  steer the whole interrupt path.
- [x] **Done when:** Linux boots SMP under OpenSBI (`--harts=4`) with every hart
  online and the scheduler running across them — `smp: Brought up 1 node, 4 CPUs`,
  and the test `/init`'s `cpuinfo` (procfs) lists all four. The SBI HSM half is
  pinned by `tests/rv64/test_rv64_hsm.S` / `make check-hsm`: four harts, secondaries
  park via `hart_stop`, hart 0 restarts each at a worker via `hart_start`, checks
  the status transitions and the error codes.
- [x] **Commits:** `feat: add sbi hsm hart management`,
  `feat: boot linux smp under opensbi`.

## M23 — virtio-net and a network path

A network is the biggest missing capability. The device is a second virtio-mmio
model; the hard part is the host backend, done in stages so a testable version
lands first.

**The virtio-net device has landed (first of three commits).** `device.c` gains a
modern (v2) virtio-net model on the second virtio slot (`VIRTIO_NET_BASE`
0x10002000, PLIC source 2): two split virtqueues (0 = receive, 1 = transmit), a
config-space MAC (`VIRTIO_NET_F_MAC`), and a small receive FIFO. It negotiates
only `VIRTIO_F_VERSION_1` + `VIRTIO_NET_F_MAC`, so the virtio-net header is 12
bytes. On a transmit notify `net_process_tx` gathers the chain, strips the header,
and hands the frame to a host backend (`plat_net_set_backend`); `net_deliver_rx`
writes buffered frames into the driver's posted receive buffers. The virtqueue
mechanics are shared with the block device (the `V_*` offsets, an extracted
`vq_chain` walker, `dma_ptr`). With **no backend attached the device loops
transmitted frames straight back to receive**, which needs no host networking and
is what the deterministic test drives: `tests/rv64/test_rv64_virtio_net.S` (23
checks — identity, feature handshake, MAC read, both queues, and a
transmit→loopback→receive round trip with the interrupt) runs under `make
check-virtnet`, quanta-only like the block test.

**The usermode network stack has landed (second commit).** `src/netstack.{h,c}` is
a from-scratch, no-dependency, no-privilege virtual gateway on 10.0.2.0/24 (the
qemu-slirp layout), attached to the device by the new `--netdev=user` flag. It is
a pure ethernet-frame processor (`netstack_input` in, replies out through a
callback, no CPU/platform coupling), answering **ARP** (for the gateway/DNS IPs),
**ICMP echo** (ping the gateway), and **DHCP** (DISCOVER→OFFER, REQUEST→ACK,
handing the guest 10.0.2.15 with the gateway/DNS/mask). It is pinned two ways:
`tests/net_test.c` drives the stack standalone (ARP/ICMP/DHCP and the IP/ICMP
checksums), and `tests/rv64/test_rv64_net.S` ARPs the gateway through the real
virtio-net device under `--netdev=user` (`make check-net`), proving the full
device↔backend↔stack↔CPU path. The `main.c` bridge (`net_backend_tx` /
`net_deliver_to_guest`) is synchronous — a reply is produced during the guest's
transmit — so no background pump is needed yet.

**The usermode NAT stack has landed (third commit).** `netstack.c` now does
outbound **NAT** for traffic past the virtual gateway: **UDP** datagrams, a
from-scratch minimal **TCP** bridge (the stack terminates the guest's connection —
SYN→connect, SYN-ACK on connect, bidirectional data with the advertised windows
for flow control, FIN/RST teardown — and streams bytes to/from a host socket; the
virtio link is lossless, so it needs no retransmission timers), and a **DNS relay**
(packets to 10.0.2.3 go to the host's real resolver from `/etc/resolv.conf`). The
stack stays host-independent: all socket I/O is delegated to an injected `NetIo`
vtable whose POSIX implementation (`net_io_*` + a `net_pump` in the run loop) lives
in `main.c`, so `tests/net_test.c` drives the whole NAT path against a **mock**
backend (UDP/TCP/DNS, `make check-net`) — the deterministic net for the protocol
logic. A `virtio,mmio` **DTB node** (gated on `--netdev`) lets a guest Linux
discover the device. Validating against a real guest surfaced two bugs: the boot
DTB now advertises the net device, and the virtio **ring capacity was raised from 8
to 256** — Linux's virtio-net stops its TX queue when fewer than `2 + MAX_SKB_FRAGS`
(≈19) descriptors are free, so the 8-entry ring (xv6's size) wedged the TX queue on
the first frame and never restarted it. With those, **mainline Linux 6.6 brings up
`eth0` and reaches the network through the NAT.**

Still to come: the **Linux TAP backend** (`feat: add a tap backend`) — it needs
`/dev/net/tun` + `CAP_NET_ADMIN`, so it can't be tested deterministically here and
is a manual milestone like the OS boots.

- [ ] **Build:** a virtio-net device (modern, RX/TX virtqueues) in `device.c`, and
  a host backend in tiers: first a Linux **TAP** backend behind a flag, then a
  from-scratch **usermode NAT/SLIRP** stack (ARP/IP/ICMP/UDP/TCP + DHCP + a DNS
  relay) so networking needs no privileges and no dependency — the from-scratch
  answer that fits the project's ethos. A loopback/packet-capture backend backs
  the deterministic test.
- [ ] **ISA:** none new — a second bus-master virtio device.
- [ ] **Concept:** virtio-net, packet DMA, the device/backend split, usermode
  networking.
- [ ] **Done when:** a guest Linux gets a link, DHCPs an address, and reaches the
  host (ping / `wget` through the NAT).
- [ ] **Commits:** `feat: add virtio-net device`, `feat: add a tap backend`,
  `feat: add a usermode nat network stack`.

## M24 — Boot a stock distribution with a persistent root filesystem

The integration trophy beyond the initramfs: a real distribution booting from a
writable disk with a real init and userland.

- [ ] **Build:** a **writable** virtio-blk backend (write-back to the image file,
  with flush semantics) replacing the read-into-memory disk, support for more than
  one disk, and whatever device-tree/console glue a distribution's init expects.
  With M20's float in place, its float-using binaries run.
- [ ] **ISA:** none new — the integration milestone for Stages 5–6.
- [ ] **Concept:** a real distribution's boot (its init system, userland, package
  manager) and on-disk persistence.
- [ ] **Done when:** buildroot or Alpine RV64 boots from a virtio disk to a login
  shell, runs stock binaries, and persists across a reboot.
- [ ] **Commits:** `feat: make the virtio disk writable`,
  `feat: boot a stock riscv distribution`.

### Stage 7 — Performance (M25)

## M25 — From interpreter to dynamic binary translation

Quanta is a switch-dispatched interpreter; this stage makes it fast while keeping
the interpreter as the bit-exact reference. It splits into a cheap win and an
ambitious capstone.

- [ ] **Build (M25a — decode cache + threaded dispatch):** pre-decode each
  instruction into an internal micro-op the first time its PC is executed, cache
  it per address, and dispatch through computed-goto / function-pointer threading
  instead of the central `switch`. A large speedup at modest complexity, still
  portable.
- [ ] **Build (M25b — basic-block JIT):** a from-scratch dynamic binary translator
  that compiles hot basic blocks to host x86-64 (no LLVM/libjit — a small
  hand-written code generator, in keeping with the no-dependency contract), with
  hot-path detection and a fallback to the interpreter for cold or unsupported
  paths.
- [ ] **ISA:** none new — an execution-engine rewrite behind the same semantics.
- [ ] **Concept:** interpreter dispatch overhead; dynamic binary translation; hot-
  path detection; register allocation for a JIT.
- [ ] **Done when:** a measurable speedup (e.g. Linux boot time cut several-fold)
  with results **bit-identical** to the interpreter, differential-tested against it.
- [ ] **Commits:** `perf: add a decoded-instruction cache`,
  `perf: thread the interpreter dispatch`, `feat: add a basic-block jit`.

### Stage 8 — Vector (M26, long-horizon capstone)

## M26 — RVV vector extension

The ambitious ISA capstone — the vector-length-agnostic programming model, layered
on the M20 float support. Scoped to grow from a subset.

- [ ] **Build:** the vector register file, `vtype`/`vl`/`vstart`/`vlenb` CSRs, the
  `vset{i}vl{i}` configuration instructions, vector loads/stores (unit-stride,
  strided, indexed), integer/fixed-point/floating vector arithmetic, reductions,
  masking, and permutes — starting from an embedded subset (e.g. Zve32x) and
  widening.
- [ ] **ISA:** RVV (a chosen, growing subset).
- [ ] **Concept:** vector-length-agnostic (VLA) programming; SIMD vs. vector;
  masked execution.
- [ ] **Done when:** the targeted RVV arch-test subset passes and agrees with qemu;
  a vector-compiled kernel runs.
- [ ] **Commits:** `feat: add rvv configuration and load/store`,
  `feat: add rvv arithmetic and reductions`, `test: rvv conformance subset`.

*(Optional side-quest M27 — the Hypervisor "H" extension: two-stage translation and
the VS/VU modes, to run a nested guest. Large; deferred unless a use case appears.)*

---

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
