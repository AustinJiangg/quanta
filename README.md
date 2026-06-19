# Quanta

A RISC-V (RV32I) instruction-set emulator written from scratch in C, built as
a hands-on way to learn computer architecture. Quanta fetches, decodes, and
executes RV32I machine code one instruction at a time, modelling the full
architectural state of a hart: 32 general-purpose registers, a program
counter, and a flat memory.

The name nods to the instruction cycle: each step advances the machine by one
discrete quantum of work.

## Status

Milestone M1. The fetch/decode/execute core runs RV32I programs and prints the
resulting register state, and it can now load real ELF32 executables
(`quanta program.elf`). A built-in demo program runs when no ELF is given, so
the emulator stays usable without the cross-toolchain. System calls (real
output and a proper exit) are the next milestone (see Roadmap).

## Tech stack

- **C (C11, standard library only)** — no third-party dependencies. The
  emulator core is pure pointer/bit/struct work, which is the point: nothing
  hides how instructions execute.
- **RISC-V RV32I** — the 32-bit base integer ISA being emulated.
- **Make** — build orchestration.
- **RISC-V cross-toolchain** (`gcc-riscv64-unknown-elf`) — optional, used to
  build real test programs for the emulator to run. Not needed to build or run
  the MVP itself.
- **GDB** — for debugging the emulator.

The emulator itself is a native host binary (built with your system `gcc`).
The cross-toolchain is only for producing RISC-V programs to feed *into* it.

## Build and run

Build the emulator and run the built-in demo:

```sh
make run
```

Or load and run a compiled RV32I ELF executable:

```sh
./quanta path/to/program.elf
```

Other targets:

```sh
make          # build ./quanta
make debug    # build with -g -O0 for gdb
make tests    # build tests/hello.elf with the RISC-V cross-toolchain
make clean    # remove build artifacts
```

### Building and running the sample RISC-V program

Loading an ELF needs a program to load, and building one needs the
cross-toolchain. On Debian/Ubuntu (including WSL2):

```sh
sudo apt install gcc-riscv64-unknown-elf
make tests                                       # -> tests/hello.elf
riscv64-unknown-elf-objdump -d tests/hello.elf   # inspect the machine code
./quanta tests/hello.elf                         # load and run it
```

`tests/hello.elf` is the same program as the built-in demo, so it halts with
`a2 = 42, a3 = 32`.

## Project structure

```
quanta/
├── src/
│   ├── cpu.h / cpu.c        # CPU state + fetch/decode/execute core
│   ├── memory.h / memory.c  # flat little-endian address space
│   ├── elf.h / elf.c        # minimal ELF32 loader
│   └── main.c               # driver: load an ELF, or run the built-in demo
├── tests/
│   └── hello.S              # sample RV32I program for the cross-toolchain
├── Makefile
├── README.md
├── ROADMAP.md               # milestone-based development plan / learning path
├── CLAUDE.md                # context for Claude Code sessions
└── .claude/                 # pre-approved commands + /commit helper
```

## Roadmap

Development proceeds in milestones (M0–M7), each a runnable step that also
teaches one architecture concept — from an ELF loader and syscalls through full
RV32I conformance, the RV32M extension, a cache model, and a pipeline timing
model. M0 (the current MVP) is done. See [ROADMAP.md](ROADMAP.md) for the full
plan, acceptance criteria, and learning path.

## License

MIT — see [LICENSE](LICENSE).
