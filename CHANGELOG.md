# Changelog

All notable changes to Quanta are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). The version is defined
once in `src/quanta.h` (`QUANTA_VERSION_*`) and surfaced by `quanta --version`.

## [Unreleased]

### Added

- **SMP multi-hart (`--harts=N`)** — Quanta now models up to 8 harts sharing one
  memory and platform (`--harts=N`, or `quanta_set_harts` in the engine API). A
  deterministic single-threaded round-robin scheduler interleaves the harts one
  instruction at a time, so runs stay reproducible while the guest still sees real
  interleaving. The CLINT gains per-hart `msip`/`mtimecmp` (a hart IPIs another by
  writing its `msip`); the PLIC gains per-hart M/S contexts driving each hart's
  MEIP/SEIP; `mhartid` reports each hart's id (delivered in `a0` at boot); and the
  boot device tree describes one `cpu@h` node per hart. The direct ELF/image boot
  brings up every hart at the entry (the qemu `-bios none` convention); the
  firmware `--bios` path parks the secondaries (SMP Linux under OpenSBI, which
  needs SBI HSM, is future work). Pinned by `make check-smp`
  (`tests/rv64/test_rv64_smp.S`): four harts verify their `mhartid`, contend on one
  shared counter with LR/SC (the total must reach `NHARTS*ITERS` with no lost
  updates), sync on a barrier, and pass a CLINT IPI hart-to-hart as a real machine
  software interrupt. And **upstream xv6-riscv boots SMP** (`--harts=3`) to its
  shell. (M19)

- **Boot Linux to an interactive userspace shell** — a mainline **Linux 6.6**
  kernel (built rv64imac, no float/vector) boots on Quanta all the way to a real
  userspace process: OpenSBI hands off to the kernel, which brings up SBI
  (TIME/IPI/RFENCE/SRST/HSM), the console, Sv39 paging, and memory zones, unpacks
  an **initramfs**, and runs `/init` as PID 1 — `Machine model: quanta,virt`,
  `Run /init as init process`, a `quanta$` prompt that echoes typed commands
  through the kernel's serial tty, and a clean power-down. The rootfs is supplied
  by the new **`--initrd=FILE`** flag, which stages a cpio archive in RAM below
  the device tree and points the kernel at it via `/chosen`
  `linux,initrd-start`/`-end` (the way qemu's `-initrd` does). The userspace is a
  freestanding `/init` (`tests/linux/init.c`: no libc, raw Linux syscalls, a tiny
  line shell that powers off via the reboot syscall → SBI SRST) packed by a
  self-contained newc-cpio builder (`tests/linux/mkcpio.c`, which synthesises the
  `/dev/console` device node the kernel opens as PID 1's console — no `cpio` tool
  or root needed). `make linux-initramfs` builds the image; run it with
  `quanta --bios=<opensbi-fw_dynamic> --kernel=Image --initrd=initramfs.cpio
  --memory=128M --max-steps=0
  --append="earlycon=uart8250,mmio,0x10000000 console=ttyS0"`. The **`--append`**
  flag (also added here) sets the kernel command line (the device tree's `/chosen`
  bootargs). Getting the kernel this far surfaced one genuine CPU bug — the JALR
  base/link ordering (see Fixed) — that only a binary large enough to need far
  `call` thunks exercises. Like xv6, it is a manual milestone (external kernel and
  firmware, long runs); no `make` target boots it — see `tests/linux/README.md`.
  (M18)
- **OpenSBI firmware boot (`--bios` / `--kernel`)** — Quanta can now boot the way
  a real RISC-V machine does: a real M-mode firmware runs first and hands off to
  an S-mode OS. `quanta --bios=FILE --kernel=FILE` (via the new
  `quanta_load_firmware` engine API) loads an M-mode firmware ELF — upstream
  **OpenSBI's fw_dynamic** build, which qemu ships prebuilt — at 0x80000000 and a
  raw S-mode OS image at 0x80200000 (the qemu `virt` kernel address a Linux
  `Image` also uses), and enters the firmware with `a0`=hartid, `a1`=DTB, and
  `a2` = a `fw_dynamic_info` descriptor directing it into the OS in S-mode. The
  boot DTB is placed with headroom (OpenSBI expands the FDT in place). **Upstream
  OpenSBI v1.3 boots on Quanta** and hands off to an S-mode payload that prints
  through the SBI console and powers off cleanly — Quanta's own SBI is bypassed
  (OpenSBI is the firmware), so this exercises Quanta purely as an M-mode machine.
  A **SiFive test finisher** device (qemu `virt`'s poweroff/reboot at 0x100000)
  gives OpenSBI's SRST (and a future Linux's poweroff) a clean exit. Pinned by
  `make check-opensbi` (skips without an OpenSBI binary, like `check-diff` without
  qemu). The groundwork for booting Linux. (M18)
- **Raw-mode interactive console** — when stdin is a terminal, a run now puts it
  in raw mode (mirroring qemu's `-nographic` console: character-at-a-time input,
  no host echo, and Ctrl-C plus flow-control keys delivered to the guest as
  bytes), so an interactive full-system guest (xv6 at its shell) reads and echoes
  each keystroke exactly once with no line buffering. A `Ctrl-A` prefix is the
  escape — `Ctrl-A x` quits the emulator, `Ctrl-A Ctrl-A` sends a literal
  `Ctrl-A`. Output processing is left untouched so a bare `\n` still displays as
  CR-LF, and the terminal is restored on every exit path (after the run, via
  `atexit`, and from `SIGINT`/`SIGTERM`/`SIGQUIT`/`SIGHUP` handlers that restore
  then re-raise), so a killed emulator never leaves the user's shell broken. A
  pipe or file is not a tty, so it is read verbatim and every existing test is
  unaffected. Pinned by `make check-console`, a pty-based test. (M18)
- **Boot xv6-riscv** — upstream `mit-pdos/xv6-riscv` boots to an interactive
  shell on Quanta (built integer-only, `rv64imac_zicsr`, `CPUS=1`; run
  `./quanta --memory=128M --max-steps=0 --disk=fs.img kernel/kernel`): `ls` reads
  the filesystem off the virtio disk, processes fork/exec through Sv39 paging, and
  the console echoes host input. Beyond the M18 devices this took four changes — a
  genuine RV64 bug fix (`exec_branch` compared only the low 32 bits, deciding an
  xv6 page-table-teardown `bltu`/`bgeu` over high user VAs wrong; it now compares
  the full XLEN), the PLIC's **S-mode context** (context 1) driving **SEIP** so an
  S-mode OS claims/completes device interrupts, the 16550 UART's THR-empty
  interrupt becoming a one-shot (so an always-empty transmitter does not storm an
  OS that leaves TX interrupts on), and a `--max-steps=N` flag (0 = uncapped) to
  run an interactive guest past the runaway guard. Pinned indirectly by
  `tests/rv64/test_rv64_plic.S` (the S-mode interrupt path) and the high-address
  branch checks in `tests/rv64/test_rv64.S`. (M18)
- **virtio-mmio block device** — a modern (version 2) block device
  (`src/device.c`) with one split virtqueue on the qemu `virt` first slot
  (`VIRTIO_BASE` 0x10001000, PLIC source 1), serving the `--disk` image as an OS's
  root filesystem — Quanta's first bus-master device, so the platform now carries
  a guest-RAM pointer (`plat_attach_ram`) for bounds-checked DMA. The driver
  brings it up through the mmio register file (status/feature handshake, queue
  addresses, `QUEUE_READY`) and kicks it with `QUEUE_NOTIFY`; `virtio_notify` then
  walks the available ring and services each descriptor chain synchronously —
  DMAing sectors to/from the disk image, writing the used ring, and asserting its
  PLIC interrupt. Inert until a guest programs it. Pinned by
  `tests/rv64/test_rv64_virtio.S` and `make check-virtio`. (M18)
- **UART receive and a `--disk` backend** — the 16550 UART gains a *source* for
  its receive path: `plat_uart_rx` (exposed as `quanta_uart_input`) buffers one
  host byte and raises the RX interrupt, and `main.c`'s console pump feeds host
  stdin through it during the run, giving a full-system guest a keyboard. Readiness
  is a zero-timeout `select`, so stdin's shell-shared flags are never mutated.
  `--disk=FILE` reads a raw block-device image wholly into an engine-owned buffer
  (`Platform.disk`) that the virtio device serves. Pinned by `make check-uart-rx`.
  (M18)
- **Sstc supervisor-timer extension** — the other way to reach a supervisor timer
  interrupt (STIP), for an OS that owns M-mode and wants no firmware round-trip
  (xv6 booted `-bios none`). When `menvcfg.STCE` (bit 63) is set, `sstc_tick`
  makes STIP a hardware shadow of the `stimecmp` CSR (0x14D) — pending exactly
  while `time >= stimecmp` — and writing `stimecmp` arms the next tick. `STCE`
  gates the whole mechanism, so it never fights the SBI timer relay; a guest uses
  one or the other. Pinned by `tests/rv64/test_rv64_sstc.S`. (M18)
- **Sv39 virtual memory** — the three-level page-table scheme RV64 needs, closing
  the "RV64 runs Bare" gap left by M17. Rather than a second walker, `mmu.c`'s
  Sv32 walk was generalised: one loop now serves both schemes, parameterised by a
  descriptor (table depth, PTE width, VPN-field width, PPN mask) chosen from
  `satp.MODE` — Sv32 is 2 levels / 4-byte PTEs, Sv39 is 3 levels / 8-byte PTEs —
  with the superpage merge, TLB, permission, A/D-writeback, and page-fault paths
  shared unchanged. Sv39 adds a canonical-VA check (bits [63:39] a sign-extension
  of bit 38), and `satp.MODE` is now enforced WARL (`mmu_satp_supported` drops a
  write selecting an unsupported Sv48/Sv57). Every RV32 net stayed bit-for-bit
  green. Pinned by `tests/rv64/test_rv64_vm.S`. (M18)
- **RV64 transition (RV64IMAC)** — the core is now width-parameterised and runs
  RV64 as well as RV32, selected per program from the ELF class rather than by a
  separate build. XLEN is a runtime property (`cpu->xlen`): all state is stored in
  64-bit fields, and a Spike-style `sext_xlen` invariant (RV32 registers hold the
  sign-extension of their 32-bit value) keeps the executor mostly width-agnostic —
  only the shifts, the `*W` word ops, and the loads carry an explicit width
  branch. Adds RV64I (the `*W` ops, LD/SD/LWU, 6-bit shifts), RV64M and RV64A
  (`.D` doublewords), and RV64C (C.ADDIW/C.LD/C.SD/C.LDSP/C.SDSP/C.SUBW/C.ADDW,
  6-bit shifts) via the same expand-to-32-bit path; the CSRs become XLEN-wide and
  the mcause interrupt bit moves to bit 63; the ELF64 loader, disassembler, GDB
  stub (`riscv:rv64`, 64-bit register packets), and boot device tree are all
  width-aware. Every RV64-only encoding traps illegal in RV32, so the RV32 suite
  is bit-for-bit unchanged. RV64 runs Bare (Sv39 paging is deferred), and RV32F/D
  stay deferred, so this is RV64IMAC not the full GC. New `tests/rv64/`
  conformance suite and `make check-rv64`, differential-tested against
  `qemu-riscv64`. (M17)
- **Boot a small RV32 OS** — a from-scratch teaching kernel (`tests/os/`) that
  boots on Quanta and runs a userspace process, the integration of everything
  M8–M15 built. An M-mode boot stub delegates user traps to Supervisor mode and
  `mret`s into a C kernel that reads its RAM from the device tree (M14), hands out
  physical pages, builds an Sv32 address space (M12 — a megapage identity map for
  the kernel and the CLINT/UART MMIO, plus user code/stack pages mapped low),
  installs an `stvec` trap handler, arms preemption through SBI `set_timer` (M15),
  and `sret`s into a U-mode process (M9). The user prints with the `write`
  syscall, is preempted by the supervisor timer (M13/M15), and `exit`s, after
  which the kernel shuts down via SBI `system_reset`. Console output drives the
  mapped 16550 UART, proving MMIO through Sv32. Pinned by `make check-os`. (M16)
- **`--memory=SIZE` flag** — size the guest RAM region independently of the ELF
  image (`quanta --memory=8M program.elf`, suffixes K/M/G), so an OS-style guest
  has spare RAM above its image to manage. The spare lands above the load image
  and the boot device tree's `/memory` node reports the true size. Surfaced in the
  engine as `quanta_load_elf_ex`; `--memory` omitted is the previous image-sized
  behaviour. (M16)
- **RV32C compressed instructions** — the compressed extension, handled by
  expanding each 16-bit instruction to the 32-bit one it abbreviates (`rvc.c`),
  so the existing decode/execute and disassembly run unchanged. The fetch is now
  variable-length (a halfword decides the length, the upper half of a 32-bit
  instruction translated separately for page straddles), the PC advances by the
  true instruction length — fixing the branch fall-through and JAL/JALR link to
  use it rather than a hardcoded `+4` — alignment relaxes to IALIGN=16, and
  `misa` advertises C. The disassembler prints the expanded mnemonics objdump
  shows. Pinned by `make check` and `make check-disasm`, and differential-tested
  against qemu via `make check-diff`. (M11)
- **SBI supervisor-timer delivery** — `sbi_set_timer` now drives a real
  supervisor timer interrupt: the firmware records the deadline and, when the
  CLINT reaches it, raises the supervisor timer pending bit (STIP) for the OS to
  take at `stvec` (once it has delegated via `mideleg` and enabled
  `sie.STIE`/`sstatus.SIE`) — the machine-timer-to-supervisor relay real firmware
  performs, without a literal M-mode trap round-trip. Inert unless a guest calls
  SBI `set_timer`. Pinned by `make check` with a `test_stimer` tick loop. (M15)
- **SBI firmware interface** — Quanta now plays M-mode firmware for a guest that
  drops to Supervisor mode: an S-mode `ecall` with no guest M-mode handler is
  serviced as a Supervisor Binary Interface call. The implementation covers the
  Base extension (version/probe), console putchar/getchar, the TIME `set_timer`,
  HSM `hart_get_status`, and SRST `system_reset`/shutdown. M/U-mode `ecall`s
  still reach the newlib syscall layer, so existing programs are unchanged.
  Pinned by `make check` and a new `make check-sbi` with a bare-metal S-mode
  `test_sbi` program that prints through the SBI console and shuts down. (M15)
- **Device tree and boot protocol** — the loader now hands an ELF guest a
  flattened device tree the way RISC-V firmware does: a freshly generated DTB
  (describing the RAM and the CLINT/PLIC/UART, built from scratch with no external
  `dtc`) is placed at the top of guest memory, and the guest is entered with
  `a0` = boot hart id and `a1` = the DTB's physical address. So a kernel can
  discover its memory layout and devices instead of assuming fixed addresses. The
  blob serialiser is `dtb_build` in a new `dtb.h`; `quanta_dtb_addr()` and the CLI
  banner report where the tree landed. Pinned by `make check` with a `test_dtb`
  program that parses the tree back out of `a1`. (M14)
- **Platform devices and interrupts** — a full-system device layer reached
  through MMIO: a CLINT (`mtime`/`mtimecmp` timer and `msip` software-interrupt),
  a PLIC (external-interrupt priority/enable/threshold and claim/complete), and a
  16550 UART whose transmit register prints to the console. The memory layer
  dispatches device-window accesses, and the CPU now delivers machine timer,
  software, and external interrupts (priority order, `mstatus`/`mideleg` gating,
  vectored `mtvec`). Pinned by `make check` and `make check-devices`. (M13)
- **GDB remote stub** — `quanta --gdb[=PORT]` (default port 1234) serves the GDB
  remote serial protocol over TCP, so a stock `gdb` attaches to a guest to read
  and write registers and memory, set breakpoints, single-step, and continue. It
  is built entirely on the public engine API and exposed to embedders as
  `quanta_gdb_serve()` in a new `gdbstub.h`. Verified end to end by `make
  check-gdb` with a self-contained RSP client (no riscv `gdb` required), and run
  under the sanitizer and coverage builds. (E9)

### Fixed

- **A successful `sc` did not void other harts' reservations** — the store-
  conditional path cleared only the issuing hart's LR/SC reservation, not the
  reservations sibling harts held on the same word, so under SMP two harts could
  each `sc` successfully over the other's read-modify-write and lose an update
  (a plain store and the AMOs already broke sibling reservations; `sc` did not).
  A successful `sc` now breaks every hart's reservation to that word, like any
  other store. Invisible before SMP (a single hart has no siblings); caught by the
  new `make check-smp` contended-counter test. (M19)
- **JALR clobbered its base when the link register aliased it** — `OP_JALR` wrote
  the link (`rd = pc + ilen`) before computing the target from `rs1`, so a
  `jalr rd, off(rd)` with `rd == rs1` jumped to `pc + ilen + off` instead of the
  intended address. That is exactly the `call` far-thunk a linker emits
  (`auipc ra,hi; jalr ra,lo(ra)`) when a call is too far for a single `jal`, so it
  only bites binaries larger than a couple of MiB — which is why the conformance
  suite, OpenSBI, and xv6 were unaffected but a 22 MiB Linux kernel jumped to
  garbage on its cross-section calls. The target is now read before the link is
  written; pinned by an `rd == rs1` case in `test_jumps.S`.

## [0.1.0] - 2026-06-28

First tagged release: a correct, observable RV32 emulator with a reusable engine
library and a production-grade test/CI harness. Roadmap milestones M0–M12 and
E1–E8.

### Added

- **Core ISA** — RV32I base integer set, RV32M multiply/divide, RV32A atomics
  (LR/SC and the AMOs), and Zicsr/Zifencei (CSR access, `fence.i`). (M0–M5, M8,
  M10)
- **Privileged architecture** — M/S/U privilege levels, the trap CSRs,
  exception entry with delegation, and `mret`/`sret`, with a built-in SEE
  fallback when the guest installs no trap handler. (M9)
- **Sv32 virtual memory** — a two-level page-table walker, a software TLB,
  A/D-bit handling, and precise page faults; the identity until a guest sets
  `satp`. (M12)
- **ELF loader and system calls** — loads static little-endian RV32 `ET_EXEC`
  images and services `write`/`exit` through the ECALL path. (M1–M2)
- **Disassembler and `--trace`** — objdump-style output that shares the
  executor's decode tables, plus per-instruction trace narration. (M4)
- **Performance overlays** — a set-associative LRU cache model (`--cache`) and a
  5-stage pipeline timing estimate (`--pipeline`), both pure observability layers
  that never change results. (M6–M7)
- **`libquanta`** — the engine as a static library behind an opaque `Quanta *`
  handle with no host-killing `exit()`; the CLI is a thin client. A `--version`
  flag and a `quanta_version()` accessor. (E1, E8)
- **Test and CI harness** — a hand-written conformance suite (`make check`), the
  official riscv-arch-test signatures (`make check-arch`), a disassembler/objdump
  cross-check, cache/pipeline checks, qemu differential testing, ASan/UBSan,
  libFuzzer harnesses, gcov/lcov coverage, and cppcheck/clang-tidy/scan-build
  static analysis — all run in GitHub Actions. (M3, E2–E7)
- **Packaging** — a `CHANGELOG`, a `quanta.1` man page, and a PREFIX-based
  `make install`. (E8)

[Unreleased]: https://github.com/AustinJiangg/quanta/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/AustinJiangg/quanta/releases/tag/v0.1.0
