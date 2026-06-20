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
check · **Commits** = suggested commit breakdown.

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

## M6 — Memory hierarchy: a cache model

Cross from "correct execution" into *performance* architecture — the area most
relevant to real optimisation work.

- [ ] **Build:** an optional cache model in front of memory (start with a single
  direct-mapped L1, then set-associative with configurable size/associativity/
  block size and LRU replacement). Count hits and misses; report a summary.
  Keep it as an observability layer — it must not change program results.
- [ ] **ISA:** none new — this wraps LOAD/STORE.
- [ ] **Concept:** the memory hierarchy; temporal and spatial locality; how
  associativity, block size, and replacement policy trade off; why cache
  behaviour dominates real-world performance.
- [ ] **Done when:** running a program reports a hit/miss breakdown, and
  changing cache parameters measurably changes the miss rate on a
  locality-sensitive workload.
- [ ] **Commits:** `feat: add direct-mapped L1 cache model`,
  `feat: support set-associative cache with LRU`,
  `feat: report cache hit/miss statistics`.

---

## M7 — A simple pipeline model (stretch)

Model time, not just outcomes. This is the conceptual capstone.

- [ ] **Build:** a classic 5-stage pipeline view (IF, ID, EX, MEM, WB) with
  cycle counting, detecting data hazards and modelling stalls/forwarding and
  control-hazard penalties on branches. Functional results stay identical;
  this estimates cycles.
- [ ] **ISA:** none new — a timing overlay on the existing core.
- [ ] **Concept:** instruction-level parallelism; pipeline hazards (data,
  control, structural); forwarding and stalls; CPI as a performance metric.
- [ ] **Done when:** the emulator reports an estimated cycle count and CPI, and
  reordering instructions to avoid a hazard visibly lowers the stall count.
- [ ] **Commits:** `feat: add 5-stage pipeline timing model`,
  `feat: model data hazards with forwarding`,
  `feat: report cycle count and CPI`.

---

## Beyond

Open-ended directions once the above is solid: RV32F/D floating point, a
privileged-mode subset with traps and CSRs, a branch predictor for the pipeline
model, or a tiny self-hosted assembler. Pick whatever the optimisation work at
hand makes you curious about.

## How to use this roadmap

- Do one milestone at a time; keep `main` runnable at every step.
- Write the test before or alongside the feature where M3+ calls for it.
- When a milestone is done, tick its boxes and note anything surprising in
  `CLAUDE.md` so future sessions inherit it.
- The concept line is the point — if a milestone works but the concept hasn't
  clicked, that's the signal to slow down and read the spec section before
  moving on.
