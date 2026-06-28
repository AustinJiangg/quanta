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

All of M0–M7 (the learning arc) are complete, and Part II — making Quanta
production-grade on the way to booting an OS — is well under way: the capability
track runs through Sv32 virtual memory (M12) and the engineering track through
release engineering (E1–E8), with the first release tagged `v0.1.0`. The
fetch/decode/execute core runs the full RV32I base integer instruction set plus
the RV32M multiply/divide extension, loads real ELF32
executables (`quanta program.elf`), and services system calls — programs print with `write` and terminate with `exit`,
whose status the emulator returns as its own exit code. A hand-written
conformance suite (`make check`) covers every instruction group, and the
official RISC-V architectural tests (`make check-arch`) pin RV32I, RV32M, and
Zifencei against the suite's own reference signatures. A built-in
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
hazards. Part II then adds an engineering track (a `libquanta` engine split, CI,
sanitizer and fuzzing builds, differential testing against qemu, coverage and
static-analysis gates, and versioned releases with a man page and `make install`)
and a capability track: Zicsr/Zifencei CSR access (M8), the M/S/U privileged
architecture with exception/trap handling (M9), RV32A atomics (M10), Sv32
virtual memory (M12), and a full-system device platform with interrupt delivery —
a CLINT timer/IPI, a PLIC, and a 16550 UART reached over MMIO (M13). A GDB remote
stub (`quanta --gdb=PORT`, E9) lets a stock `gdb` attach over TCP to read and
write registers and memory, set breakpoints, single-step, and continue. On top of
those, the loader hands the guest a flattened device tree at boot (M14), Quanta
services an S-mode guest's `ecall`s as M-mode SBI firmware (console, timer, system
reset, M15), and a small from-scratch teaching kernel (`tests/os/`) boots on all
of it — Sv32 paging, an `stvec` trap handler, timer preemption, and a userspace
process making `write`/`exit` syscalls — to userspace (`make check-os`, M16). Next
is the RV64GC transition that unlocks mainstream operating systems.

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

Debug a guest with a real `gdb` — `--gdb` starts a remote stub on a TCP port
(default 1234), bound to localhost, and waits for the debugger to attach:

```sh
./quanta --gdb path/to/program.elf          # listen on :1234
./quanta --gdb=4321 path/to/program.elf     # listen on :4321
```

Then, from another terminal, drive it with stock `gdb` over the remote protocol:

```sh
riscv64-unknown-elf-gdb path/to/program.elf
(gdb) target remote :1234
(gdb) break _start
(gdb) stepi
(gdb) info registers
(gdb) continue
```

Boot the small teaching kernel — it brings up Sv32 paging, drops to a userspace
process, is preempted by the supervisor timer, and shuts down via the SBI.
`--memory` sizes the guest RAM region beyond the kernel image so it has space to
manage (bytes, with a `K`/`M`/`G` suffix):

```sh
make tests/os/kernel.elf
./quanta --memory=8M tests/os/kernel.elf
```

Print the version:

```sh
./quanta --version          # quanta 0.1.0
```

Other targets:

```sh
make               # build ./quanta
make debug         # build with -g -O0 for gdb
make tests         # build the sample RISC-V programs (needs the cross-toolchain)
make check         # build and run the RV32I conformance suite
make check-arch    # run the official riscv-arch-test conformance suite
make check-disasm  # cross-check the disassembler against objdump
make check-cache   # check the cache model on a locality workload
make check-pipeline # check the pipeline model on a hazard workload
make check-gdb     # drive the gdb remote stub with a self-contained client
make check-devices # check the MMIO devices and interrupt delivery (M13)
make check-diff    # differential-test against qemu-riscv32
make coverage      # gcov/lcov line-coverage report
make analyze       # cppcheck + clang-tidy static analysis
make install       # install quanta, libquanta, headers, and the man page
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
│   ├── memory.h / memory.c    # flat little-endian address space + MMIO dispatch
│   ├── device.h / device.c    # MMIO devices: CLINT, PLIC, 16550 UART (M13)
│   ├── elf.h / elf.c          # minimal ELF32 loader
│   ├── syscall.h / syscall.c  # ECALL handling: write + exit syscalls
│   ├── gdbstub.h / gdbstub.c  # GDB remote-protocol stub over TCP (--gdb)
│   └── main.c                 # driver: load an ELF (or demo), --trace/--cache/--gdb/--memory/--signature
├── tests/
│   ├── hello.S                # arithmetic demo (mirrors the built-in program)
│   ├── hello_world.S          # syscall demo: prints via write, then exits
│   ├── test_framework.h       # CHECK/exit-code harness for conformance tests
│   ├── test_*.S               # RV32I conformance suite (run by `make check`)
│   ├── os/                    # M16 teaching kernel: boots to userspace (make check-os)
│   ├── hazard_slow.S / hazard_fast.S  # pipeline hazard demo (make check-pipeline)
│   ├── check_disasm.sh        # disassembler vs objdump (run by `make check-disasm`)
│   ├── check_cache.sh         # cache model checks (run by `make check-cache`)
│   ├── check_pipeline.sh      # pipeline model checks (run by `make check-pipeline`)
│   ├── check_gdb.sh / gdb_client.py  # GDB-stub RSP client checks (make check-gdb)
│   ├── check_devices.sh       # MMIO device + interrupt checks (make check-devices)
│   ├── check_sbi.sh           # bare-metal S-mode SBI checks (make check-sbi)
│   ├── check_os.sh            # boot the M16 teaching kernel (make check-os)
│   ├── check_arch.sh          # official riscv-arch-test conformance (make check-arch)
│   └── arch/                  # Quanta target for riscv-arch-test (model_test.h, link.ld)
├── docs/quanta.1            # man page (installed by `make install`)
├── Makefile
├── README.md
├── CHANGELOG.md             # release history (Keep a Changelog)
├── ROADMAP.md               # milestone-based development plan / learning path
├── CLAUDE.md                # context for Claude Code sessions
└── .claude/                 # pre-approved commands + /commit helper
```

## Roadmap

Development proceeds in milestones, each a runnable step that also teaches one
architecture concept. M0–M7 (the learning arc) are all complete — core loop, ELF
loader, system calls, RV32I conformance, disassembler + trace mode, RV32M
extension, cache model, and pipeline timing model. Part II then advances two
tracks toward a production-grade, OS-booting emulator: an engineering track
(`libquanta` split, CI, sanitizers, fuzzing, differential testing, coverage and
static-analysis gates, versioned releases, and a GDB remote stub — E1–E9 done)
and a capability track (Zicsr/Zifencei M8, the M/S/U privileged architecture M9,
RV32A atomics M10, Sv32 virtual memory M12, platform devices and interrupts M13,
with a device tree and a bare-metal SBI next). See
[ROADMAP.md](ROADMAP.md) for the full plan, acceptance criteria, and learning
path.

## License

MIT — see [LICENSE](LICENSE).
