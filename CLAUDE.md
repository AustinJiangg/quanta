# CLAUDE.md

## TL;DR (non-negotiable)

- **Everything in English** — code, comments, docs, commits. The repo is public.
- **Conventional Commits**, kept short: `<type>: <imperative summary>`,
  lowercase, no trailing period, max 50 chars. Types: feat, fix, docs,
  refactor, chore, test, perf.
- **One commit per feature.** Land a feature as a single commit — code, tests,
  and docs together — not separate feat/test/docs/chore commits. Split only when
  the changes are genuinely independent units of work.
- **Commit locally, push manually.** Never run `git push` on the user's
  behalf — they decide when to push.

## What this is

Quanta is a from-scratch RISC-V (RV32I) instruction-set emulator in C, built to
learn computer architecture. It models a single hart: 32 registers, a PC, and a
flat little-endian memory. The core is a fetch/decode/execute loop.

All roadmap milestones (M0–M7) are complete, and Part II is under way: the full
RV32I base integer set, the RV32M multiply/divide extension, Zicsr/Zifencei (CSR
access and `fence.i`, M8), the privileged architecture (M/S/U privilege levels
with exception/trap handling, M9), RV32A atomics (M10), and Sv32 virtual memory
(M12) are implemented and pinned by a hand-written conformance suite (`make
check`) plus the official RISC-V architectural tests (`make check-arch`, run
offline against the suite's own committed reference signatures), an optional
cache model sits in front of memory, and a `--pipeline` timing model estimates
cycles and CPI.
Quanta loads ELF32
executables (`quanta program.elf`), services `write`/`exit` system calls through
the ECALL path — the built-in SEE that runs until a guest installs its own trap
handler — and returns the guest's exit status as its own. A hardcoded
built-in demo runs when no ELF is given. A disassembler plus a `--trace` flag
make execution observable, and `make check-disasm` pins the disassembly to
`objdump`. An optional `--cache` flag models a configurable set-associative L1
over the run's data accesses and reports hit/miss statistics, and `--pipeline`
adds a 5-stage timing overlay estimating cycles and CPI from load-use and control
hazards — both pure overlays that never change results (`make check-cache`,
`make check-pipeline`). A `--gdb[=PORT]` flag starts a GDB remote stub so a stock
`gdb` attaches over TCP to step, break, and inspect a guest (`make check-gdb`;
embeddable as `quanta_gdb_serve` in `gdbstub.h`, E9). The full milestone plan and
learning path live in `ROADMAP.md` — consult it for what comes next and tick
boxes there as milestones land.

## Build / run / debug

```sh
make          # build ./quanta (native host binary)
make run      # build and run the built-in demo program
make embed    # build and run the libquanta embedding example
make debug    # build with -g -O0 for gdb
make tests    # build the sample RISC-V programs (needs cross-toolchain)
make check    # build and run the RV32I conformance suite (needs cross-toolchain)
make check-disasm  # cross-check the disassembler against objdump (needs cross-toolchain)
make check-cache   # check the cache model on a locality workload (needs cross-toolchain)
make check-pipeline # check the pipeline model on a hazard workload (needs cross-toolchain)
make check-gdb     # drive the gdb remote stub with a self-contained client (needs python3)
make check-diff    # differential-test against qemu-riscv32 (needs qemu-user-static)
make check-arch    # official riscv-arch-test conformance (needs cross-toolchain + clone)
make sanitize      # build with ASan+UBSan and run the suite (needs cross-toolchain)
make fuzz          # build the libFuzzer harnesses (needs clang)
make fuzz-replay   # run the harnesses over the corpus under gcc (needs cross-toolchain)
make coverage      # gcov-instrument, run the suite, report line coverage (needs cross-toolchain; lcov optional)
make analyze       # static analysis: cppcheck + clang-tidy (CI adds scan-build)
make install       # install the CLI, libquanta, headers, and man page (PREFIX=/usr/local)
make clean
```

Run a compiled program with `./quanta <program.elf>`; with no argument the
built-in demo runs instead. Add `--trace` (`./quanta --trace <program.elf>`) to
narrate each executed instruction — PC, disassembly, and changed registers — to
stderr. Add `--quiet` to suppress all driver output (banner, summary, register
dump), leaving only the guest's own stdout — used by `make check-diff`. Add `--cache[=SIZE:WAYS:BLOCK]` (e.g. `--cache=1024:2:32`) to model a
set-associative L1 over the run's data accesses and print a hit/miss summary at
exit, and/or `--pipeline` to print a 5-stage cycle/CPI estimate. The overlays
compose. Add `--gdb[=PORT]` (default 1234) to start a GDB remote stub and wait
for a debugger to `target remote :PORT`; it drives execution itself, so it does
not combine with `--trace`/`--pipeline`. Add `--signature=FILE` to dump the
architectural-test signature region (the words between the
`begin_signature`/`end_signature` ELF symbols, in the suite's reference format) —
what makes Quanta a drop-in `make check-arch` target.

Debugging the emulator: `make debug && gdb ./quanta`. Note the two-level
structure — gdb debugs the emulator (x86), which internally "runs" a RISC-V
program.

## Two toolchains — don't confuse them

- **Host `gcc`** builds the emulator itself (a native x86-64 binary).
- **`riscv64-unknown-elf-gcc`** builds RISC-V programs that the emulator will
  load and run. Used only by `make tests`; not required to build or run the
  emulator itself, since the built-in demo needs no ELF.

When writing test programs for RV32I, always pass `-march=rv32i -mabi=ilp32`
(the RV32M test uses `-march=rv32im`, the CSR test `-march=rv32i_zicsr_zifencei`,
the M9/M12 privilege and paging tests `-march=rv32i_zicsr`, and the RV32A test
`-march=rv32ia`; the Makefile overrides `RVCFLAGS` for just those ELFs).

## Code layout

- `src/memory.{h,c}` — flat address space; little-endian load/store helpers.
- `src/decode.h` — shared instruction decoding: field-extraction and
  immediate-decoding helpers, the opcode map, and ABI register names, all
  `static inline`. The executor and the disassembler decode through this, so
  they can't disagree about an instruction's layout.
- `src/cpu.{h,c}` — CPU state and the instruction core. Each instruction group
  has its own `exec_*` function; decoding comes from `decode.h`. RV32M
  (multiply/divide) shares the OP opcode and lives in `exec_muldiv`, selected by
  `funct7 == 0x01`. Zicsr (CSR access) lives in `exec_csr`, reached from
  `exec_system` when SYSTEM carries a non-zero funct3; `csr_read`/`csr_write`
  are the choke point the CSR file flows through, now with privilege and
  read-only checks. The M9 privileged architecture also lives here: a `priv`
  field (M/S/U), the trap CSRs, and `raise_trap` — the single point exceptions
  (ECALL/EBREAK/illegal/misaligned/access-fault) funnel through to vector into a
  handler or, when no handler is installed, fall back to the built-in SEE.
  `exec_mret`/`exec_sret` pop the stacked privilege to return. RV32A atomics
  (LR/SC and the AMO*s) are `exec_amo` under the dedicated AMO opcode, backed by
  a single-word LR reservation (`reserve_valid`/`reserve_addr`) that a matching
  store voids. Under M12, every fetch and data address is run through
  `mmu_translate` before it reaches memory.
- `src/mmu.{h,c}` — Sv32 virtual memory (M12): the two-level page-table walker,
  a small software TLB (in the CPU struct), permission and A/D-bit handling, and
  the page-fault decision. `mmu_translate(cpu, va, acc, &pa)` returns 0 or a
  page-fault cause; `cpu.c` calls it in the fetch and load/store/AMO paths and
  raises the cause as a trap. Translation is the identity in M-mode or Bare mode,
  so it is inert until a guest writes `satp`. `mmu_flush` (called on `sfence.vma`
  and satp writes) drops the TLB.
- `src/disasm.{h,c}` — RV32I disassembler: turns an instruction word back into
  objdump-style assembly (ABI names, common pseudo-instructions, absolute
  branch/jump targets). Mirrors `cpu_step`'s opcode switch over `decode.h`.
- `src/cache.{h,c}` — optional set-associative LRU cache model. A pure
  observability layer: `cpu_step`'s load/store paths feed it data addresses, it
  tallies hits/misses, but the bytes still come from `memory.c`, so results are
  untouched. Off unless `--cache` is given (`cpu->cache == NULL`).
- `src/pipeline.{h,c}` — optional 5-stage pipeline *timing* model. Another
  overlay: `main.c`'s run loop feeds it each retired instruction word and whether
  control redirected, and it estimates cycles/CPI by charging load-use and
  control-hazard stalls. Driven from the loop, not the core, so `cpu.c` stays
  untouched. Off unless `--pipeline` is given.
- `src/elf.{h,c}` — minimal ELF32 loader: parses the header and program
  headers, copies `PT_LOAD` segments to their virtual addresses, returns the
  entry point. Fields are read with explicit little-endian helpers (no struct
  overlay), so it stays host-endianness-independent. A separate, defensively
  bounds-checked `elf_symbol` pass reads the section + symbol tables to resolve a
  symbol by name (running an image never needs it) — used by `--signature` to
  locate `begin_signature`/`end_signature`, and surfaced as `quanta_elf_symbol`.
- `src/syscall.{h,c}` — the system-call layer (the "kernel" side of ECALL):
  dispatches on the `a7` syscall number and implements `write` and `exit` per
  the RISC-V Linux/newlib ABI.
- `src/quanta.{h,c}` — the public `libquanta` engine API: an opaque `Quanta *`
  handle wrapping CPU + memory + the optional cache, with lifecycle, ELF/raw-image
  loading, `quanta_step`/`quanta_run`, and register/memory accessors. The engine
  core never calls `exit()` on its host — every stop is a `HaltReason` (an
  out-of-range access becomes `HALT_MEM_FAULT`), surfaced through the public
  `QuantaStatus`/`QuantaHalt` enums. Built as `libquanta.a`; the CLI and
  `examples/embed.c` are clients of it.
- `src/gdbstub.{h,c}` — a GDB remote-serial-protocol server over TCP (E9):
  `quanta_gdb_serve(q, port)` listens on localhost, accepts one debugger, and
  drives the machine through the public `quanta.h` API alone — registers, memory,
  single-step, halt reason — answering the `g`/`G`/`m`/`M`/`p`/`P`/`c`/`s`/`vCont`/
  `Z`/`z`/`qXfer` packets and serving an RV32 target description. Breakpoints are
  tracked here and enforced in the continue loop, so guest memory is never
  patched. Reached via `--gdb` from `main.c`, and embeddable. It is the project's
  one piece of OS-specific code, so the POSIX-sockets feature macro is local to
  the `.c`.
- `src/main.c` — the CLI driver, a thin client over `quanta.h`: argument parsing,
  the `--trace` narration, the `--pipeline`/`--cache` overlays, the `--gdb` stub
  hand-off, and the `--signature` arch-test dump, all driving the machine through
  the public API (no engine internals). Loads an ELF named on the command line,
  or runs the built-in demo when none is given.
- `examples/embed.c` — minimal embedding example: ~30 lines that load and run a
  guest through `libquanta` (`make embed`).
- `tests/hello.S` — sample RV32I assembly, mirrors the built-in demo.
- `tests/hello_world.S` — syscall demo: prints a string with `write`, then
  `exit`s.
- `tests/test_framework.h` + `tests/test_*.S` — the RV32I conformance suite:
  per-group assertion programs that exit 0 on success or the failing check's
  id. `make check` runs them and reads quanta's propagated exit code.
- `tests/test_stack.S` — exercises the loader-initialised stack (a non-leaf
  function spilling `ra` and callee-saved registers) and a small array-traversal
  workload; part of `make check`, and a seed for the M6 cache benchmark.
- `tests/test_trap.S` + `tests/test_priv.S` — the M9 privilege suite. `test_trap`
  installs an `mtvec` handler and takes three M-mode exceptions (ecall, ebreak,
  illegal), checking `mcause`/`mepc` and `mret` resume; `test_priv` walks
  M→U→S→U→M through delegation (`medeleg`), an S-mode handler, and `sret`. Both
  use machine CSRs, so they assemble with `-march=rv32i_zicsr` and stay out of
  `make check-diff`.
- `tests/test_atomic.S` — the M10 RV32A suite: every AMO (old value + stored
  result, signed-vs-unsigned min/max) plus LR/SC success and a broken-reservation
  failure. Atomics are user-mode, so it assembles `-march=rv32ia` and *is*
  differential-tested against qemu.
- `tests/test_vm.S` — the M12 Sv32 suite: builds a page table by hand, enables
  paging, and drops to S-mode, proving non-identity translation (two VAs aliased
  to one frame), hardware dirty-bit update, and a precise load page fault caught
  by an M-mode handler. Uses satp/supervisor CSRs, so `-march=rv32i_zicsr` and
  out of `make check-diff`.
- `tests/check_disasm.sh` — runs each sample ELF under `--trace` and diffs the
  disassembly against `objdump` (`make check-disasm`).
- `tests/check_cache.sh` — runs `test_stack` under two cache geometries and
  asserts results are unchanged and a smaller cache misses more
  (`make check-cache`).
- `tests/check_diff.sh` — differential test: runs each sample ELF under
  `quanta --quiet` and a reference simulator (qemu-riscv32 by default, override
  with `REF=`) and asserts they agree on stdout and exit code (`make
  check-diff`). Skips cleanly if the reference is absent. The privileged tests
  (`test_csr`, `test_trap`, `test_priv`, `test_vm`) are excluded — their
  machine-mode CSR, trap, and paging use trips user-mode qemu's own supervisor;
  `make check` pins them instead.
- `tests/arch/` + `tests/check_arch.sh` — the official architectural conformance
  (`make check-arch`, E6). `check_arch.sh` clones the pinned riscv-arch-test
  `old-framework-2.x` branch into `build/`, builds each test with the framework
  header plus `tests/arch/`'s `model_test.h` (SEE-`exit` halt) and `link.ld`,
  runs it under `quanta --signature`, and diffs against the suite's *committed*
  reference signatures — so no Sail/Spike reference model is needed. Covers the
  families Quanta passes fully (RV32I, RV32M, Zifencei); skips cleanly without the
  toolchain or network. See `tests/arch/README.md` for the scope and exclusions.
- `fuzz/fuzz_elf.c`, `fuzz/fuzz_decode.c` — libFuzzer harnesses over the ELF
  loader and the decode/execute path; `fuzz/standalone.c` is a plain-main driver
  so they replay over a corpus under gcc (`make fuzz` / `make fuzz-replay`).
- `tests/hazard_slow.S` + `tests/hazard_fast.S` — the same array sum scheduled
  with and without a load-use hazard; `tests/check_pipeline.sh` runs both under
  `--pipeline` and asserts the reorder cut stalls and cycles without changing the
  result (`make check-pipeline`).
- `tests/check_gdb.sh` + `tests/gdb_client.py` — exercise the GDB stub (`--gdb`)
  end to end with a self-contained RSP client (no riscv `gdb` needed): it
  attaches, reads/writes registers and memory, single-steps, sets a breakpoint
  and continues to exit on `tests/hello.elf`, asserting the known outcomes
  (`make check-gdb`). Skips cleanly without python3; also run under `make
  sanitize` and `make coverage`.
- `tests/coverage.sh` — collects gcov line coverage after an instrumented build
  (`make coverage`): prefers lcov (HTML under `build/coverage`) and falls back to
  plain gcov. Observability only, like the cache/pipeline overlays.
- `tests/analyze.sh` + `.clang-tidy` + `tests/cppcheck-suppress.txt` — static
  analysis (`make analyze`): cppcheck and clang-tidy over `src/`, each skipping
  cleanly when absent. The `.clang-tidy` check list and the cppcheck suppressions
  are the baseline that keeps the analyzers passing clean; CI adds scan-build.

## Code style

- C11, standard library only — do not add third-party dependencies.
- Build must stay clean under `-Wall -Wextra`.
- Use fixed-width types (`uint32_t`, `int32_t`) everywhere that machine width
  matters; never rely on the width of plain `int` for guest state.
- Keep instruction logic readable: one `exec_*` per opcode group, decode via
  the shared field helpers rather than re-deriving shifts/masks inline.

## Gotchas

- Register `x0` is hardwired to zero — enforced in `reg_write`. Don't bypass it.
- RV32I immediates are bit-scrambled across the instruction word and mostly
  sign-extended; the `imm_*` helpers in `decode.h` are the single source of
  truth, shared by the executor and the disassembler. Re-deriving them by hand
  is the easiest way to introduce bugs.
- Memory is little-endian; multi-byte access assembles bytes low-first.
- The ELF loader only accepts static, little-endian RV32 `ET_EXEC` images
  (build with `-nostdlib -nostartfiles -Ttext=0x80000000`). PIE/`ET_DYN`
  output won't load — there's no relocation handling. The loader sizes guest
  memory to span the program's `PT_LOAD` range; note the linker places the
  first segment a page *below* `-Ttext` (≈`0x7ffff000`, it carries the ELF
  headers), and the entry stays at `0x80000000`. Above the image the loader
  reserves a 64 KiB stack block (`GUEST_STACK_SIZE`), and `main.c` sets `sp` to
  the top of the region (16-byte aligned) — for both the demo and an ELF — so
  programs can call functions and spill locals.
- The built-in demo uses a fixed 64 KiB region at `0x80000000`
  (`MEM_BASE`/`MEM_SIZE` in `main.c`); an ELF gets a region sized to its load
  image instead. BSS (`p_memsz > p_filesz`) reads back as zero because the
  region is zero-initialised at `mem_init`.
- ECALL/EBREAK now raise real exceptions (M9), but with a fallback: when the
  guest has installed no trap handler (the resolved `*tvec` is still 0), they
  fall back to the built-in SEE — the same `a7`-dispatched syscall layer as
  before (`write`=64, `exit`=93, `exit_group`=94 — RISC-V Linux/newlib numbers;
  args in `a0`–`a2`, result in `a0`), with EBREAK / unknown-syscall / illegal-
  instruction stopping the machine. Once a guest sets `mtvec` (or `stvec` for a
  delegated trap) these vector into its handler instead. So bare programs still
  terminate by calling `exit`; a stale-`a7` `ecall` (or running off the code)
  still trips an "unknown syscall" halt — which is why the demo and
  `tests/hello.S` end with an explicit `exit`. Quanta returns the guest's exit
  code as its own process status (abnormal stops return 1), which is how `make
  check` tells pass from fail.
- `run_until_halt` (in `main.c`) caps a run at 100M instructions as a runaway
  guard: a program that never calls `exit` stops there, reports the limit, and
  returns 1. Raise the cap if a workload legitimately needs more.
- The cache model (`--cache`) is observability only: it sees data load/store
  addresses (not instruction fetches), uses write-allocate so a store miss
  installs the block, and must never alter results — keep it that way. It's off
  by default, so `make check`/`make check-disasm` are unaffected; geometry is
  `SIZE:WAYS:BLOCK` with all three powers of two and `SIZE = sets*ways*block`.
- The pipeline model (`--pipeline`) is likewise observability only, fed from
  `main.c`'s run loop (`pipeline_observe` per retired instruction). Its numbers
  are estimates under explicit assumptions — full forwarding (only load-use
  stalls), predict-not-taken control penalties (JAL 1, taken branch/JALR 2), no
  structural or cache-miss penalties — not a cycle-accurate simulation.
- FENCE (MISC-MEM opcode `0x0f`) is a no-op — a single in-order hart has
  nothing to reorder — and so is FENCE.I (Zifencei), its instruction-stream
  cousin, for the same reason (no modelled icache to flush). WFI is likewise a
  nop (no interrupt sources to wait for yet). CSR instructions (Zicsr, M8) run
  `csrrw/s/c` and their immediate forms through `exec_csr`: most CSRs are plain
  WARL storage; the unprivileged counters (`cycle`/`time`/`instret` and their
  high halves) read back the retired-instruction count. `exec_csr` enforces the
  access privilege encoded in CSR bits [9:8] and raises illegal-instruction on a
  write to a read-only CSR (bits [11:10] == `0b11`) — except a `csrrs/csrrc` with
  an `x0` source, which performs no write and so never trips that check.
- Privilege and traps (M9): the hart tracks a current mode (`PRIV_M`/`S`/`U`,
  resets to M) and `raise_trap` is the one path every synchronous exception
  takes. It resolves the target mode via `medeleg` (a trap in S/U delegates to
  S; a trap in M never delegates), stacks `MIE`/`MPIE`+`MPP` (or the S mirrors)
  into `mstatus`, writes `*epc`/`*cause`/`*tval`, and vectors to `*tvec` (direct
  mode; vectored is for interrupts, M13). `mret`/`sret` pop that state. The
  S-mode CSRs `sstatus`/`sie`/`sip` are masked *views* of `mstatus`/`mie`/`mip`,
  not separate storage. Key design point: **if the resolved `*tvec` is still 0,
  no guest handler exists and the trap falls back to the built-in SEE** — which
  is what keeps every pre-M9 program (none of which set `mtvec`) running
  unchanged. Not yet modelled: interrupts (no devices until M13), `mcounteren`
  gating of counter access from lower privilege, and Sv32 translation (`satp` is
  stored, not walked, until M12). Conformance is pinned by the hand-written `make
  check` and the official architectural tests (`make check-arch`, E6 — see the
  arch-test gotcha below).
- RV32M (M5) was the first extension wired in: it shares the OP opcode and
  is selected by `funct7 == 0x01` (`exec_muldiv` in `cpu.c`, mirrored in
  `disasm.c`). Divide-by-zero and the `INT_MIN / -1` signed overflow return
  *defined* values rather than trapping. Its test must be assembled with
  `-march=rv32im`, so the Makefile overrides `RVCFLAGS` for
  `tests/test_muldiv.elf` only.
- RV32A (M10) atomics live in `exec_amo` under the AMO opcode (`0x2f`, funct3
  `0x2`), mirrored in `disasm.c`. The aq/rl ordering bits are no-ops on a single
  in-order hart. LR.W holds a word-granularity reservation
  (`reserve_valid`/`reserve_addr`) that SC.W consumes and any store to the same
  word voids; SC returns 0 on success, 1 on failure. Atomics fault on a
  misaligned address (base load/store still handle misalignment silently). The
  test is user-mode (`-march=rv32ia`) and differential-tested against qemu — its
  SC-failure case overwrites the reserved word with a *different* value, so an
  address-based reservation (ours) and a value-based one (qemu-user) agree.
- Sv32 paging (M12) lives in `mmu.c`, not `memory.c` — translation needs CPU
  state (satp/priv/mstatus), while `memory.c` stays a dumb physical array. Key
  points: translation is the identity in M-mode or Bare mode, so paging is inert
  until a guest sets `satp` (every pre-M12 test is unaffected). Page tables are
  read *physically* by the walker, so they need no mapping. A/D bits are set in
  hardware (A on any access, D on a store); the TLB therefore serves only
  fetches and loads — stores always walk so the dirty bit lands on the real PTE
  — and is flushed by `sfence.vma` and any `satp` write. `mstatus.MPRV` lets an
  M-mode load/store translate as MPP; SUM/MXR gate S-mode access to user pages.
  A walk failure (missing PTE, bad permission, misaligned superpage) returns the
  page-fault cause, which `cpu.c` raises as a trap with the faulting VA in
  `*tval`. Not modelled: the access-fault-vs-page-fault distinction for a
  page-table read that leaves RAM (treated as a page fault), and ASID-scoped
  flushes (`sfence.vma` drops the whole TLB). Like the other privileged tests,
  `test_vm` can't run under user-mode qemu, so it has no differential safety
  net — lean on `--trace` when changing the walker.
- `--trace` writes to stderr, leaving the guest's own stdout (`write`) clean;
  "changed registers" are recovered by diffing a register snapshot taken around
  `cpu_step`, so the core isn't instrumented. The disassembler prints the common
  pseudo-instructions (`li`/`mv`/`j`/`ret`/`beqz`/…) so its output lines up with
  `objdump -d`, which `make check-disasm` enforces; sharing `decode.h` with the
  executor keeps the two from drifting apart.
- Official conformance (`make check-arch`, E6) deliberately does **not** use the
  full RISCOF + Sail/Spike flow — none of which is installable here. Instead it
  pins riscv-arch-test's frozen `old-framework-2.x` branch, which *commits* the
  golden reference signatures, so the check is offline (only the cross-compiler +
  a one-time clone). Non-obvious build facts when touching `tests/check_arch.sh`:
  the framework needs **`-DXLEN=32`** (the bare `XLEN` macro it keys `MASK` off,
  distinct from the builtin `__riscv_xlen`) and **`_zicsr`** in `-march` (its
  startup touches CSRs). Excluded, by design: the `privilege` family (its
  `misalign-*` tests expect a trap, but Quanta handles misaligned access in
  hardware — a spec-permitted choice — so the signatures differ), C/F/K
  (unimplemented, M11), and `jalr-01` (`la x0,5b`, a binutils wart). `--signature`
  self-resolves `begin_signature`/`end_signature` from the ELF, so the halt only
  has to exit cleanly. Full rationale in `tests/arch/README.md`.
- Coverage (`make coverage`) instruments the *host* emulator, not the guest
  ELFs — the same split as `make sanitize` — and is observability only. Two tool
  quirks bit once and are worked around in `tests/coverage.sh`: lcov 2.0's
  per-file `--list` table miscomputes rates (use `--summary`, which is correct),
  and gcov's grand-total line has no `File` header (don't misattribute it).
- Static analysis (`make analyze`) is kept *clean*, not just run: `.clang-tidy`
  disables only justified noise (the Annex K `*_s` nag — glibc has none —
  include-cleaner, and missing-default on the exhaustive decode switches), with
  `WarningsAsErrors` gating the rest; real findings are fixed in code, not
  suppressed. cppcheck rejects a bare `#` line in its suppressions file, so keep
  comment lines non-empty in `tests/cppcheck-suppress.txt`. The reserved-`funct3`
  decode cases (e.g. RV32 LOAD/STORE widths that are RV64-only) currently fall
  through as no-ops rather than trapping illegal-instruction — a known, untested
  leniency, not yet tightened.
- Versioning/release (E8): the version lives once in `src/quanta.h`
  (`QUANTA_VERSION_*` + `quanta_version()`), surfaced by `quanta --version`; bump
  it together with `CHANGELOG.md` and the `vX.Y.Z` git tag. `make install` is
  `PREFIX`/`DESTDIR`-based; `libquanta.a` is archived with `ar D` so a rebuild is
  byte-identical (the objects embed no `__DATE__`/`__TIME__`) — don't reintroduce
  build timestamps.
- The GDB stub (`--gdb`, E9) is built only on the public `quanta.h` API and
  speaks the standard RSP, so a stock `gdb` attaches with `target remote :PORT`
  (it binds localhost only). Breakpoints are stub-side: the continue loop stops
  when the PC reaches a `Z0`/`Z1` address, so guest memory is never patched with
  trap words. The packet buffer is `calloc`'d so a read past a matched prefix is a
  defined 0 — which also keeps clang-analyzer/scan-build from flagging an
  uninitialised read. It is the lone POSIX dependency: `gdbstub.c` defines
  `_DEFAULT_SOURCE` itself (so `make analyze`'s clang-tidy, which compiles with a
  bare `-std=c11`, still sees the socket decls) under a local NOLINT for the
  reserved-identifier check. `--gdb` takes over execution, so it does not combine
  with `--trace`/`--pipeline`; `make check-gdb` drives it with a pure-python RSP
  client (`tests/gdb_client.py`, no riscv `gdb` needed) and it also runs under
  `make sanitize`/`make coverage`.

## .claude/

- `settings.json` — pre-approves build/run/test and common git commands
  (including `push`, so a manual push isn't gated; pre-approval does not make
  it run automatically).
- `commands/commit.md` — `/commit` writes a Conventional Commits message and
  commits locally (never pushes).
