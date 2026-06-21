# Quanta

[![CI](https://github.com/AustinJiangg/quanta/actions/workflows/ci.yml/badge.svg)](https://github.com/AustinJiangg/quanta/actions/workflows/ci.yml)

A RISC-V (RV32I) instruction-set emulator written from scratch in C, built as
a hands-on way to learn computer architecture. Quanta fetches, decodes, and
executes RV32I machine code one instruction at a time, modelling the full
architectural state of a hart: 32 general-purpose registers, a program
counter, and a flat memory.

The name nods to the instruction cycle: each step advances the machine by one
discrete quantum of work.

## Status

Milestone M7 — the final roadmap milestone; all of M0–M7 are now complete. The
fetch/decode/execute core runs the full RV32I base integer instruction set plus
the RV32M multiply/divide extension, loads real ELF32
executables (`quanta program.elf`), and services system calls — programs print with `write` and terminate with `exit`,
whose status the emulator returns as its own exit code. A hand-written
conformance suite (`make check`) covers every instruction group. A built-in
demo runs when no ELF is given, so the emulator stays usable without the
cross-toolchain. A disassembler and a `--trace` mode make execution
observable: `quanta --trace program.elf` narrates each instruction with its
disassembly and the registers it changed, and `make check-disasm` pins that
disassembly to `objdump`. Loaded programs run with a loader-initialised stack
(sp set to the top of their memory image), so they can call functions and use
locals. An optional `--cache` flag models a configurable set-associative L1 over
the run's data accesses and reports hit/miss statistics, without changing what
the program computes. A `--pipeline` flag adds a 5-stage timing overlay that
estimates cycle count and CPI from the instruction stream's load-use and control
hazards. With M7 done the numbered milestones are complete; see the roadmap's
"Beyond" section for open directions (floating point, a privileged/CSR subset, a
branch predictor, a self-hosted assembler).

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

Model a set-associative L1 cache over the run's data accesses and print a
hit/miss summary at exit (geometry is `SIZE:WAYS:BLOCK`, all powers of two):

```sh
./quanta --cache path/to/program.elf            # defaults: 1024:2:32
./quanta --cache=4096:4:64 path/to/program.elf  # 4 KiB, 4-way, 64 B blocks
```

Estimate cycles and CPI for a classic 5-stage pipeline (a timing overlay; it
composes with `--cache` and `--trace`):

```sh
./quanta --pipeline path/to/program.elf
```

Other targets:

```sh
make               # build ./quanta
make debug         # build with -g -O0 for gdb
make tests         # build the sample RISC-V programs (needs the cross-toolchain)
make check         # build and run the RV32I conformance suite
make check-disasm  # cross-check the disassembler against objdump
make check-cache   # check the cache model on a locality workload
make check-pipeline # check the pipeline model on a hazard workload
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
│   ├── cache.h / cache.c      # optional set-associative cache model (--cache)
│   ├── pipeline.h / pipeline.c # optional 5-stage timing model (--pipeline)
│   ├── memory.h / memory.c    # flat little-endian address space
│   ├── elf.h / elf.c          # minimal ELF32 loader
│   ├── syscall.h / syscall.c  # ECALL handling: write + exit syscalls
│   └── main.c                 # driver: load an ELF (or demo), --trace/--cache
├── tests/
│   ├── hello.S                # arithmetic demo (mirrors the built-in program)
│   ├── hello_world.S          # syscall demo: prints via write, then exits
│   ├── test_framework.h       # CHECK/exit-code harness for conformance tests
│   ├── test_*.S               # RV32I conformance suite (run by `make check`)
│   ├── hazard_slow.S / hazard_fast.S  # pipeline hazard demo (make check-pipeline)
│   ├── check_disasm.sh        # disassembler vs objdump (run by `make check-disasm`)
│   ├── check_cache.sh         # cache model checks (run by `make check-cache`)
│   └── check_pipeline.sh      # pipeline model checks (run by `make check-pipeline`)
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
model. M0–M7 are all complete (core loop, ELF loader, system calls, RV32I
conformance, disassembler + trace mode, RV32M extension, cache model, pipeline
timing model); the roadmap's "Beyond" section lists open directions. See
[ROADMAP.md](ROADMAP.md) for the full plan, acceptance criteria, and learning
path.

## License

MIT — see [LICENSE](LICENSE).
