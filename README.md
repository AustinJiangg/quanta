# Quanta

A RISC-V (RV32I) instruction-set emulator written from scratch in C, built as
a hands-on way to learn computer architecture. Quanta fetches, decodes, and
executes RV32I machine code one instruction at a time, modelling the full
architectural state of a hart: 32 general-purpose registers, a program
counter, and a flat memory.

The name nods to the instruction cycle: each step advances the machine by one
discrete quantum of work.

## Status

Milestone M4. The fetch/decode/execute core runs the full RV32I base integer
instruction set, loads real ELF32 executables (`quanta program.elf`), and
services system calls — programs print with `write` and terminate with `exit`,
whose status the emulator returns as its own exit code. A hand-written
conformance suite (`make check`) covers every instruction group. A built-in
demo runs when no ELF is given, so the emulator stays usable without the
cross-toolchain. A disassembler and a `--trace` mode make execution
observable: `quanta --trace program.elf` narrates each instruction with its
disassembly and the registers it changed, and `make check-disasm` pins that
disassembly to `objdump`. Up next (see Roadmap): the RV32M multiply/divide
extension.

## Tech stack

- **C (C11, standard library only)** — no third-party dependencies. The
  emulator core is pure pointer/bit/struct work, which is the point: nothing
  hides how instructions execute.
- **RISC-V RV32I** — the 32-bit base integer ISA being emulated.
- **Make** — build orchestration.
- **RISC-V cross-toolchain** (`gcc-riscv64-unknown-elf`) — optional, used to
  build real test programs for the emulator to run. Not needed to build or run
  the emulator itself.
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

Trace execution one instruction at a time — PC, disassembly, and the registers
each step changes — printed to stderr:

```sh
./quanta --trace path/to/program.elf
```

Other targets:

```sh
make               # build ./quanta
make debug         # build with -g -O0 for gdb
make tests         # build the sample RISC-V programs (needs the cross-toolchain)
make check         # build and run the RV32I conformance suite
make check-disasm  # cross-check the disassembler against objdump
make clean         # remove build artifacts
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
│   ├── cpu.h / cpu.c          # CPU state + fetch/decode/execute core
│   ├── decode.h               # shared field + immediate decode helpers
│   ├── disasm.h / disasm.c    # RV32I disassembler (objdump-style output)
│   ├── memory.h / memory.c    # flat little-endian address space
│   ├── elf.h / elf.c          # minimal ELF32 loader
│   ├── syscall.h / syscall.c  # ECALL handling: write + exit syscalls
│   └── main.c                 # driver: load an ELF (or demo), optional --trace
├── tests/
│   ├── hello.S                # arithmetic demo (mirrors the built-in program)
│   ├── hello_world.S          # syscall demo: prints via write, then exits
│   ├── test_framework.h       # CHECK/exit-code harness for conformance tests
│   ├── test_*.S               # RV32I conformance suite (run by `make check`)
│   └── check_disasm.sh        # disassembler vs objdump (run by `make check-disasm`)
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
model. M0–M4 are done (core loop, ELF loader, system calls, RV32I conformance,
disassembler + trace mode). See [ROADMAP.md](ROADMAP.md) for the full plan,
acceptance criteria, and learning path.

## License

MIT — see [LICENSE](LICENSE).
