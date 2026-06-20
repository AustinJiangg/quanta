# CLAUDE.md

## TL;DR (non-negotiable)

- **Everything in English** — code, comments, docs, commits. The repo is public.
- **Conventional Commits**, kept short: `<type>: <imperative summary>`,
  lowercase, no trailing period, max 50 chars. Types: feat, fix, docs,
  refactor, chore, test, perf.
- **Commit locally, push manually.** Never run `git push` on the user's
  behalf — they decide when to push.

## What this is

Quanta is a from-scratch RISC-V (RV32I) instruction-set emulator in C, built to
learn computer architecture. It models a single hart: 32 registers, a PC, and a
flat little-endian memory. The core is a fetch/decode/execute loop.

All roadmap milestones (M0–M7) are complete: the full RV32I base integer set
plus the RV32M multiply/divide extension are implemented and pinned by a
hand-written conformance suite (`make check`), an optional cache model sits in
front of memory, and a `--pipeline` timing model estimates cycles and CPI.
Quanta loads ELF32
executables (`quanta program.elf`), services `write`/`exit` system calls through
the ECALL path, and returns the guest's exit status as its own. A hardcoded
built-in demo runs when no ELF is given. A disassembler plus a `--trace` flag
make execution observable, and `make check-disasm` pins the disassembly to
`objdump`. An optional `--cache` flag models a configurable set-associative L1
over the run's data accesses and reports hit/miss statistics, and `--pipeline`
adds a 5-stage timing overlay estimating cycles and CPI from load-use and control
hazards — both pure overlays that never change results (`make check-cache`,
`make check-pipeline`). The full milestone plan and learning path live in `ROADMAP.md` —
consult it for what comes next and tick boxes there as milestones land.

## Build / run / debug

```sh
make          # build ./quanta (native host binary)
make run      # build and run the built-in demo program
make debug    # build with -g -O0 for gdb
make tests    # build the sample RISC-V programs (needs cross-toolchain)
make check    # build and run the RV32I conformance suite (needs cross-toolchain)
make check-disasm  # cross-check the disassembler against objdump (needs cross-toolchain)
make check-cache   # check the cache model on a locality workload (needs cross-toolchain)
make check-pipeline # check the pipeline model on a hazard workload (needs cross-toolchain)
make clean
```

Run a compiled program with `./quanta <program.elf>`; with no argument the
built-in demo runs instead. Add `--trace` (`./quanta --trace <program.elf>`) to
narrate each executed instruction — PC, disassembly, and changed registers — to
stderr. Add `--cache[=SIZE:WAYS:BLOCK]` (e.g. `--cache=1024:2:32`) to model a
set-associative L1 over the run's data accesses and print a hit/miss summary at
exit, and/or `--pipeline` to print a 5-stage cycle/CPI estimate. The overlays
compose.

Debugging the emulator: `make debug && gdb ./quanta`. Note the two-level
structure — gdb debugs the emulator (x86), which internally "runs" a RISC-V
program.

## Two toolchains — don't confuse them

- **Host `gcc`** builds the emulator itself (a native x86-64 binary).
- **`riscv64-unknown-elf-gcc`** builds RISC-V programs that the emulator will
  load and run. Used only by `make tests`; not required to build or run the
  emulator itself, since the built-in demo needs no ELF.

When writing test programs for RV32I, always pass `-march=rv32i -mabi=ilp32`
(the RV32M test uses `-march=rv32im`; the Makefile overrides `RVCFLAGS` for just
`tests/test_muldiv.elf`).

## Code layout

- `src/memory.{h,c}` — flat address space; little-endian load/store helpers.
- `src/decode.h` — shared instruction decoding: field-extraction and
  immediate-decoding helpers, the opcode map, and ABI register names, all
  `static inline`. The executor and the disassembler decode through this, so
  they can't disagree about an instruction's layout.
- `src/cpu.{h,c}` — CPU state and the instruction core. Each instruction group
  has its own `exec_*` function; decoding comes from `decode.h`. RV32M
  (multiply/divide) shares the OP opcode and lives in `exec_muldiv`, selected by
  `funct7 == 0x01`.
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
  overlay), so it stays host-endianness-independent.
- `src/syscall.{h,c}` — the system-call layer (the "kernel" side of ECALL):
  dispatches on the `a7` syscall number and implements `write` and `exit` per
  the RISC-V Linux/newlib ABI.
- `src/main.c` — driver: loads an ELF named on the command line (or runs the
  built-in demo when none is given), with an optional `--trace` flag.
- `tests/hello.S` — sample RV32I assembly, mirrors the built-in demo.
- `tests/hello_world.S` — syscall demo: prints a string with `write`, then
  `exit`s.
- `tests/test_framework.h` + `tests/test_*.S` — the RV32I conformance suite:
  per-group assertion programs that exit 0 on success or the failing check's
  id. `make check` runs them and reads quanta's propagated exit code.
- `tests/test_stack.S` — exercises the loader-initialised stack (a non-leaf
  function spilling `ra` and callee-saved registers) and a small array-traversal
  workload; part of `make check`, and a seed for the M6 cache benchmark.
- `tests/check_disasm.sh` — runs each sample ELF under `--trace` and diffs the
  disassembly against `objdump` (`make check-disasm`).
- `tests/check_cache.sh` — runs `test_stack` under two cache geometries and
  asserts results are unchanged and a smaller cache misses more
  (`make check-cache`).
- `tests/hazard_slow.S` + `tests/hazard_fast.S` — the same array sum scheduled
  with and without a load-use hazard; `tests/check_pipeline.sh` runs both under
  `--pipeline` and asserts the reorder cut stalls and cycles without changing the
  result (`make check-pipeline`).

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
- ECALL is a system call now, not a halt: it dispatches on the `a7` number
  (`write`=64, `exit`=93, `exit_group`=94 — RISC-V Linux/newlib numbers), with
  arguments in `a0`–`a2` and the result returned in `a0`. EBREAK, unknown
  syscalls, and unimplemented SYSTEM instructions stop the machine instead. So
  programs must terminate by calling `exit`; a bare `ecall` with a stale `a7`
  (or running off the end of the code) trips an "unknown syscall" halt — which
  is why the built-in demo and `tests/hello.S` end with an explicit `exit`.
  Quanta returns the guest's exit code as its own process status (abnormal
  stops return 1), which is how `make check` tells pass from fail.
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
  nothing to reorder. CSR instructions (Zicsr) and FENCE.I (Zifencei) are
  outside base RV32I: CSRs currently halt as "unimplemented SYSTEM", and
  that's why conformance uses the hand-written `make check` suite rather than
  the official `riscv-tests` (whose `-p` environment needs CSR/trap support).
- RV32M (M5) is the one extension wired in so far: it shares the OP opcode and
  is selected by `funct7 == 0x01` (`exec_muldiv` in `cpu.c`, mirrored in
  `disasm.c`). Divide-by-zero and the `INT_MIN / -1` signed overflow return
  *defined* values rather than trapping. Its test must be assembled with
  `-march=rv32im`, so the Makefile overrides `RVCFLAGS` for
  `tests/test_muldiv.elf` only.
- `--trace` writes to stderr, leaving the guest's own stdout (`write`) clean;
  "changed registers" are recovered by diffing a register snapshot taken around
  `cpu_step`, so the core isn't instrumented. The disassembler prints the common
  pseudo-instructions (`li`/`mv`/`j`/`ret`/`beqz`/…) so its output lines up with
  `objdump -d`, which `make check-disasm` enforces; sharing `decode.h` with the
  executor keeps the two from drifting apart.

## .claude/

- `settings.json` — pre-approves build/run/test and common git commands
  (including `push`, so a manual push isn't gated; pre-approval does not make
  it run automatically).
- `commands/commit.md` — `/commit` writes a Conventional Commits message and
  commits locally (never pushes).
