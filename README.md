# Quanta

A RISC-V (RV32I) instruction-set emulator written from scratch in C, built as
a hands-on way to learn computer architecture. Quanta fetches, decodes, and
executes RV32I machine code one instruction at a time, modelling the full
architectural state of a hart: 32 general-purpose registers, a program
counter, and a flat memory.

The name nods to the instruction cycle: each step advances the machine by one
discrete quantum of work.

## Status

MVP. The core fetch/decode/execute loop runs a hardcoded RV32I program and
prints the resulting register state. ELF loading and system calls are the next
milestones (see Roadmap).

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

Other targets:

```sh
make          # build ./quanta
make debug    # build with -g -O0 for gdb
make tests    # build tests/hello.elf with the RISC-V cross-toolchain
make clean    # remove build artifacts
```

### Building the sample RISC-V program

This needs the cross-toolchain. On Debian/Ubuntu (including WSL2):

```sh
sudo apt install gcc-riscv64-unknown-elf
make tests
riscv64-unknown-elf-objdump -d tests/hello.elf   # inspect the machine code
```

## Project structure

```
quanta/
├── src/
│   ├── cpu.h / cpu.c        # CPU state + fetch/decode/execute core
│   ├── memory.h / memory.c  # flat little-endian address space
│   └── main.c               # MVP driver (runs a hardcoded program)
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
