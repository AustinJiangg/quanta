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

Currently at milestone M3: the full RV32I base integer set is implemented and
pinned by a hand-written conformance suite (`make check`). Quanta loads ELF32
executables (`quanta program.elf`), services `write`/`exit` system calls through
the ECALL path, and returns the guest's exit status as its own. A hardcoded
built-in demo runs when no ELF is given. The full milestone plan and learning
path live in `ROADMAP.md` — consult it for what comes next and tick boxes there
as milestones land.

## Build / run / debug

```sh
make          # build ./quanta (native host binary)
make run      # build and run the built-in demo program
make debug    # build with -g -O0 for gdb
make tests    # build the sample RISC-V programs (needs cross-toolchain)
make check    # build and run the RV32I conformance suite (needs cross-toolchain)
make clean
```

Run a compiled program with `./quanta <program.elf>`; with no argument the
built-in demo runs instead.

Debugging the emulator: `make debug && gdb ./quanta`. Note the two-level
structure — gdb debugs the emulator (x86), which internally "runs" a RISC-V
program.

## Two toolchains — don't confuse them

- **Host `gcc`** builds the emulator itself (a native x86-64 binary).
- **`riscv64-unknown-elf-gcc`** builds RISC-V programs that the emulator will
  load and run. Used only by `make tests`; not required to build or run the
  emulator itself, since the built-in demo needs no ELF.

When writing test programs for RV32I, always pass `-march=rv32i -mabi=ilp32`.

## Code layout

- `src/memory.{h,c}` — flat address space; little-endian load/store helpers.
- `src/cpu.{h,c}` — CPU state and the instruction core. Field-extraction and
  immediate-decoding helpers live at the top of `cpu.c`; each instruction
  group has its own `exec_*` function.
- `src/elf.{h,c}` — minimal ELF32 loader: parses the header and program
  headers, copies `PT_LOAD` segments to their virtual addresses, returns the
  entry point. Fields are read with explicit little-endian helpers (no struct
  overlay), so it stays host-endianness-independent.
- `src/syscall.{h,c}` — the system-call layer (the "kernel" side of ECALL):
  dispatches on the `a7` syscall number and implements `write` and `exit` per
  the RISC-V Linux/newlib ABI.
- `src/main.c` — driver: loads an ELF named on the command line, or runs the
  built-in demo program when none is given.
- `tests/hello.S` — sample RV32I assembly, mirrors the built-in demo.
- `tests/hello_world.S` — syscall demo: prints a string with `write`, then
  `exit`s.
- `tests/test_framework.h` + `tests/test_*.S` — the RV32I conformance suite:
  per-group assertion programs that exit 0 on success or the failing check's
  id. `make check` runs them and reads quanta's propagated exit code.

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
  sign-extended; the `imm_*` helpers are the single source of truth. Re-deriving
  them by hand is the easiest way to introduce bugs.
- Memory is little-endian; multi-byte access assembles bytes low-first.
- The ELF loader only accepts static, little-endian RV32 `ET_EXEC` images
  (build with `-nostdlib -nostartfiles -Ttext=0x80000000`). PIE/`ET_DYN`
  output won't load — there's no relocation handling. The loader sizes guest
  memory to span the program's `PT_LOAD` range; note the linker places the
  first segment a page *below* `-Ttext` (≈`0x7ffff000`, it carries the ELF
  headers), and the entry stays at `0x80000000`. There's no stack room yet.
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
- FENCE (MISC-MEM opcode `0x0f`) is a no-op — a single in-order hart has
  nothing to reorder. CSR instructions (Zicsr) and FENCE.I (Zifencei) are
  outside base RV32I: CSRs currently halt as "unimplemented SYSTEM", and
  that's why conformance uses the hand-written `make check` suite rather than
  the official `riscv-tests` (whose `-p` environment needs CSR/trap support).

## .claude/

- `settings.json` — pre-approves build/run/test and common git commands
  (including `push`, so a manual push isn't gated; pre-approval does not make
  it run automatically).
- `commands/commit.md` — `/commit` writes a Conventional Commits message and
  commits locally (never pushes).
