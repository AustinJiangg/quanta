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

Currently at MVP (milestone M0): runs a hardcoded program and dumps register
state. No ELF loading or syscalls yet. The full milestone plan and learning
path live in `ROADMAP.md` — consult it for what comes next and tick boxes there
as milestones land.

## Build / run / debug

```sh
make          # build ./quanta (native host binary)
make run      # build and run the MVP demo
make debug    # build with -g -O0 for gdb
make tests    # build tests/hello.elf (needs RISC-V cross-toolchain)
make clean
```

Debugging the emulator: `make debug && gdb ./quanta`. Note the two-level
structure — gdb debugs the emulator (x86), which internally "runs" a RISC-V
program.

## Two toolchains — don't confuse them

- **Host `gcc`** builds the emulator itself (a native x86-64 binary).
- **`riscv64-unknown-elf-gcc`** builds RISC-V programs that the emulator will
  load and run. Used only by `make tests`; not required for the MVP.

When writing test programs for RV32I, always pass `-march=rv32i -mabi=ilp32`.

## Code layout

- `src/memory.{h,c}` — flat address space; little-endian load/store helpers.
- `src/cpu.{h,c}` — CPU state and the instruction core. Field-extraction and
  immediate-decoding helpers live at the top of `cpu.c`; each instruction
  group has its own `exec_*` function.
- `src/main.c` — MVP driver with the hardcoded program.
- `tests/hello.S` — sample RV32I assembly, mirrors the hardcoded demo.

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

## .claude/

- `settings.json` — pre-approves build/run/test and common git commands
  (including `push`, so a manual push isn't gated; pre-approval does not make
  it run automatically).
- `commands/commit.md` — `/commit` writes a Conventional Commits message and
  commits locally (never pushes).
