# CLAUDE.md

## TL;DR (non-negotiable)

- **Everything in English** — code, comments, docs, commits. The repo is public.
- **Conventional Commits**, kept short: `<type>: <imperative summary>`,
  lowercase, no trailing period, max 50 chars. Types: feat, fix, docs,
  refactor, chore, test, perf.
- **One commit per feature.** Land a feature as a single commit — code, tests,
  and docs together — not separate feat/test/docs/chore commits. Split only when
  the changes are genuinely independent units of work.
- **Commit then push.** After a local commit lands, push it automatically
  (`git push`, setting `-u origin <branch>` if no upstream exists) — no need to
  wait for the user to ask.

## What this is

Quanta is a from-scratch RISC-V (RV32 and RV64) instruction-set emulator in C,
built to learn computer architecture. It models a single hart: 32 XLEN-wide
registers (32 or 64 bits, chosen per program from the ELF class), a PC, and a
flat little-endian memory. The core is a fetch/decode/execute loop.

All roadmap milestones (M0–M7) are complete, and Part II is under way: the full
RV32I base integer set, the RV32M multiply/divide extension, the RV32C compressed
extension (M11), Zicsr/Zifencei (CSR
access and `fence.i`, M8), the privileged architecture (M/S/U privilege levels
with exception/trap handling, M9), RV32A atomics (M10), Sv32 virtual memory
(M12), a full-system device platform with interrupt delivery (a CLINT
timer/IPI, a PLIC, and a 16550 UART reached through MMIO, M13), and a flattened
device tree handed to the guest at boot per the RISC-V `a0`=hartid/`a1`=DTB
convention (M14), an SBI firmware interface that services an S-mode guest's
`ecall`s (console, timer, system reset, M15), and a small from-scratch teaching
kernel that boots to userspace on all of it (`tests/os/`, `make check-os`, M16),
and the **RV64 transition** — a runtime-XLEN core that also runs RV64IMAC,
selected per program from the ELF class (M17, `tests/rv64/`, `make check-rv64`,
differential-tested against `qemu-riscv64`; RV32F/D stay deferred), and the start
of **M18** — **Sv39 virtual memory** on RV64 (the three-level page-table walk,
sharing the walker with Sv32, `tests/rv64/test_rv64_vm.S`), the **Sstc**
supervisor-timer extension (`stimecmp`/`menvcfg.STCE`, `tests/rv64/test_rv64_sstc.S`),
and a **virtio-mmio block device** (modern/v2, one split virtqueue, serving the
`--disk` image by DMA, `tests/rv64/test_rv64_virtio.S`, `make check-virtio`), plus
the PLIC **S-mode interrupt context** (SEIP, `tests/rv64/test_rv64_plic.S`) — with
which **upstream xv6-riscv now boots to an interactive shell** on Quanta (built
`rv64imac_zicsr`, `CPUS=1`; `ls` reads the virtio disk, processes run through
Sv39), and — through real **OpenSBI** firmware (`--bios`/`--kernel`) plus a
**cpio initramfs** (`--initrd`, `tests/linux/`) — **mainline Linux 6.6 now boots
to an interactive userspace shell**, and **SMP multi-hart** (`--harts=N`, up to 8
harts on a deterministic round-robin scheduler, M19) on which upstream **xv6 boots
across 3 harts**, the **RV32/64 F and D floating-point extensions** (M20) on a
from-scratch, correctly-rounded IEEE-754 software float (`src/softfloat.{h,c}`, no
dependency; validated bit-for-bit against qemu-riscv64, `tests/rv64/test_rv64_fpu.S`),
and the **RV32/64 bit-manipulation extensions** — Zba, Zbb, Zbs, and Zbc
carry-less multiply (M21, `tests/test_bitmanip.S` / `tests/rv64/test_rv64_bitmanip.S`,
differential-tested against qemu) — are implemented
and pinned by a hand-written conformance suite (`make check`) plus the official
RISC-V architectural tests (`make check-arch`, run offline against the suite's
own committed reference signatures), an optional cache model sits in front of
memory, and a `--pipeline` timing model estimates cycles and CPI.
Quanta loads static little-endian ELF32/ELF64
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
embeddable as `quanta_gdb_serve` in `gdbstub.h`, E9). A `--memory=SIZE` flag sizes
the guest RAM region independently of the ELF image, so an OS-style guest has
spare RAM above its image to manage (the boot DTB's `/memory` node reports the
true size). The full milestone plan and
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
make check-console # exercise the raw-mode interactive console over a pty (needs python3 + cross-toolchain)
make check-opensbi # boot OpenSBI firmware handing off to an S-mode payload (needs an OpenSBI binary)
make check-devices # check the MMIO devices and interrupt delivery (needs cross-toolchain)
make check-sbi     # check the SBI on a bare-metal S-mode program (needs cross-toolchain)
make check-uart-rx # check UART receive (piped stdin) and the --disk backend (M18)
make check-virtio  # check the virtio-mmio block device on a --disk image (M18)
make check-virtnet # check the virtio-mmio network device via loopback (M23)
make check-net     # check the usermode network stack (ARP/ICMP/DHCP) (M23)
make check-smp     # check SMP: 4 harts, contended LR/SC, a CLINT IPI (M19)
make check-hsm     # check SBI HSM: park/restart secondary harts (M22)
make check-snapshot # check machine snapshot/restore replays a run bit-for-bit (E10)
make check-replay  # check --snapshot/--restore file round-trip and resume (E10)
make check-os      # boot the M16 teaching kernel to userspace (needs cross-toolchain)
make linux-initramfs # build the Linux initramfs (tests/linux/) for the OpenSBI->Linux boot (M18)
make check-rv64    # RV64IMAC conformance (tests/rv64/), diff vs qemu-riscv64 (M17)
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
not combine with `--trace`/`--pipeline`. Add `--memory=SIZE` (bytes, with an
optional `K`/`M`/`G` suffix, e.g. `--memory=8M`) to grow the guest RAM region
beyond its ELF image — spare RAM lands above the image for an OS-style guest to
manage, and the boot DTB advertises the real size (`tests/os/` needs it). Add
`--disk=FILE` to attach a raw block-device image (read wholly into memory) that
the virtio-mmio block device serves as an OS's root filesystem (M18). Add
`--netdev=user` to attach the from-scratch usermode network stack to the
virtio-net device: it presents a virtual gateway on 10.0.2.0/24 (the qemu-slirp
layout) the guest can DHCP against and ping, with no host privileges (M23). With the
`--bios`/`--kernel` firmware-boot path, add `--initrd=FILE` to stage a cpio
initramfs in RAM as the kernel's root filesystem (advertised via the DTB
`/chosen` `linux,initrd-start`/`-end`) — how Linux reaches its `/init` (M18). Add
`--max-steps=N` (a count with an optional `K`/`M`/`G`/`T` suffix, `0` = no cap)
to raise or remove the 100M-instruction runaway guard — an interactive
full-system guest (a booting OS) legitimately runs billions of instructions. Add
`--harts=N` (1..8) to model an SMP machine: N harts share the memory and devices,
interleaved by a deterministic round-robin scheduler (M19); the direct ELF/image
boot brings them all up, and xv6 boots on `--harts=3`. Add `--snapshot=FILE` to
write the whole machine state to a file when the run ends, and `--restore=FILE`
to rebuild a machine from such a file and resume it (standalone — no program
needed); together they checkpoint and resume a long run
(`--max-steps=N --snapshot=FILE …` then `--restore=FILE`), the deterministic
scheduler making the resumed tail bit-identical (E10). Add
`--signature=FILE` to dump the
architectural-test signature region (the words between the
`begin_signature`/`end_signature` ELF symbols, in the suite's reference format) —
what makes Quanta a drop-in `make check-arch` target. While a guest runs, host
stdin is pumped into the UART's receive path (checked by a zero-timeout `select`,
so the shared stdin flags are never mutated), giving a full-system guest a
keyboard; when stdin is a terminal the run puts it in raw mode (mirroring qemu's
`-nographic` console: character-at-a-time, no host echo, signal/flow-control keys
delivered to the guest, `Ctrl-A x` to quit) and restores it on any exit path,
while a pipe or file is read verbatim.

Debugging the emulator: `make debug && gdb ./quanta`. Note the two-level
structure — gdb debugs the emulator (x86), which internally "runs" a RISC-V
program.

## Two toolchains — don't confuse them

- **Host `gcc`** builds the emulator itself (a native x86-64 binary).
- **`riscv64-unknown-elf-gcc`** builds RISC-V programs that the emulator will
  load and run. Used only by `make tests`; not required to build or run the
  emulator itself, since the built-in demo needs no ELF.

When writing test programs for RV32I, always pass `-march=rv32i -mabi=ilp32`
(the RV32M test uses `-march=rv32im`, the RV32C test `-march=rv32ic`, the CSR
test `-march=rv32i_zicsr_zifencei`, the M9/M12 privilege and paging tests
`-march=rv32i_zicsr`, and the RV32A test `-march=rv32ia`; the Makefile overrides
`RVCFLAGS` for just those ELFs).

## Code layout

- `src/memory.{h,c}` — flat address space; little-endian load/store helpers.
  With a `Platform` attached (M13), physical-address windows are carved out for
  MMIO: each `mem_read*`/`mem_write*` checks `plat_contains(addr)` first and
  dispatches device accesses to `device.c`, otherwise hits the flat RAM array.
- `src/device.{h,c}` — the MMIO device models (M13): a CLINT (`mtime`/`mtimecmp`
  timer + `msip` IPI, both **per-hart arrays** on the qemu virt map so any hart
  IPIs another and each has its own timer, M19), a PLIC (priority/enable/threshold
  + claim/complete, with **per-hart contexts `2h`→MEIP / `2h+1`→SEIP** so an S-mode
  OS routes device interrupts to the right hart, M18/M19; `plat_mip_bits(p, hart)`
  is the per-hart pull), and a 16550 UART (transmit prints to
  stdout; receive via `plat_uart_rx`, which buffers a host byte and — with RX
  interrupts enabled — raises the UART's PLIC source, the input half of the
  console the CLI pumps stdin into, M18; its THR-empty interrupt is a one-shot
  `thre_ip`, so an always-empty transmitter does not storm an OS leaving TX
  interrupts on). M18 adds a
  **virtio-mmio block device** (`virtio_*`, modern/v2, one split virtqueue,
  PLIC source 1): the driver programs it through the mmio register file and kicks
  it with `QUEUE_NOTIFY`, on which `virtio_notify` walks the available ring and
  services each descriptor chain synchronously — DMAing sectors between the guest
  buffers and the `--disk` image, writing the used ring, and asserting its PLIC
  interrupt. The register files sit on the qemu `virt` address map with no CPU
  dependency — the memory layer dispatches accesses here, and the CPU pulls
  `plat_mip_bits()` (MTIP/MSIP/MEIP) each step. `plat_tick` advances `mtime` one
  tick per CPU step (deterministic). The `Platform` also holds a `Disk` — the raw
  block-device image (`--disk`, owned by the engine) the virtio device serves —
  and, unlike the other (register-only) devices, a pointer to guest RAM
  (`plat_attach_ram`) so the virtio block device can DMA (it is a bus master).
  M23 adds a **virtio-mmio network device** (`VirtioNet`, `virtio_net_*`, second
  virtio slot `0x10002000`, PLIC source 2): two split virtqueues (0 = receive,
  1 = transmit), a config-space MAC (`VIRTIO_NET_F_MAC`), and a small receive
  FIFO. On a transmit `QUEUE_NOTIFY` `net_process_tx` gathers each chain, strips
  the 12-byte virtio-net header, and hands the frame to a host backend
  (`plat_net_set_backend`) — or, with none attached, loops it straight back to
  receive; `net_deliver_rx` writes buffered frames (header + payload) into the
  driver's posted receive buffers and advances the used ring. `plat_net_rx` is the
  host-input side a backend calls. It shares the virtqueue mechanics with the block
  device (the `V_*` register offsets, the extracted `vq_chain` chain walker,
  `dma_ptr`). The TAP and usermode-NAT backends are the next M23 steps.
- `src/dtb.{h,c}` — the flattened device-tree (FDT) generator (M14): `dtb_build`
  serialises a standard `.dtb` blob (big-endian header, memory-reservation block,
  structure block, deduplicated strings) from a `DtbConfig` describing the RAM
  and the M13 devices — no external `dtc`. A pure serialiser with no machine
  state; the boot handoff that *uses* it lives in `quanta.c`'s `setup_boot`.
- `src/netstack.{h,c}` — the usermode network backend for virtio-net (M23): a
  from-scratch, no-dependency, no-privilege network stack presenting a virtual
  gateway on 10.0.2.0/24 (the qemu-slirp layout). A pure ethernet-frame processor
  — `netstack_input(ns, frame, len)` parses a guest frame and emits replies
  through the callback given at `netstack_new`, holding no reference to the CPU or
  platform (so `tests/net_test.c` drives it standalone). Answers ARP for the
  gateway/DNS IPs, ICMP echo to the gateway, and DHCP (DISCOVER→OFFER,
  REQUEST→ACK, handing the guest 10.0.2.15). `main.c`'s `--netdev=user` bridges it
  to the device (device transmit → `netstack_input`, replies → `quanta_net_rx`).
  Outbound UDP/TCP NAT to host sockets and a DNS relay are the next M23 step.
- `src/decode.h` — shared instruction decoding: field-extraction and
  immediate-decoding helpers, the opcode map, and ABI register names, all
  `static inline`. The executor and the disassembler decode through this, so
  they can't disagree about an instruction's layout.
- `src/rvc.{h,c}` — the RV32C compressed-instruction expander (M11):
  `rvc_expand(uint16_t)` widens a 16-bit instruction to the exact 32-bit
  instruction it abbreviates (or `RVC_ILLEGAL` for a reserved encoding), so
  the existing decode/execute and disassembly paths run it unchanged. The single
  source of truth for what a compressed instruction means, shared by `cpu.c`
  (fetch) and `disasm.c` (which prints the same expanded mnemonic objdump shows).
  M20 added the compressed float loads/stores — `C.FLD`/`C.FSD` (both widths) and,
  on RV32, `C.FLW`/`C.FSW` (the funct3 slots RV64 uses for `C.LD`/`C.SD`), plus
  the stack-pointer forms `C.FLDSP`/`C.FSDSP`/`C.FLWSP`/`C.FSWSP` — expanding to
  the `OP_LOAD_FP`/`OP_STORE_FP` opcodes.
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
  `mmu_translate` before it reaches memory. M13 adds interrupt delivery: at the
  top of each step the hart ticks the platform timer and calls `take_interrupt`,
  which pulls the device-driven `mip` bits (`effective_mip`), applies the
  `mstatus`/`mideleg` gates, and vectors the highest-priority enabled interrupt
  through `enter_trap` (the trap-entry path now shared with `raise_trap`, with
  vectored-`*tvec` support). M20 adds the F/D float layer: the `fregs[32]`
  register file, `exec_load_fp`/`exec_store_fp` (FLW/FLD/FSW/FSD), `exec_fp` (the
  OP-FP arithmetic/convert/move/compare family, dispatched by `funct7`), and
  `exec_fmadd` (the four fused ops); `fread_s`/`fwrite_s` do the single-precision
  NaN-boxing, `resolve_rm` picks the rounding mode, and `fcsr`/`frm`/`fflags` are
  CSR views — the IEEE arithmetic itself lives in `softfloat.c`. M21 adds the
  bit-manipulation extensions (Zba/Zbb/Zbs/Zbc): four `exec_bitmanip_*` intercepts
  (one per OP / OP-IMM / OP-32 / OP-IMM-32 opcode) run before the base ALU switch
  and return 0 to fall through, so the base decode is untouched — including the
  funct7 == 0x20 slots Zbb's andn/orn/xnor share with SUB/SRA; small `bm_*`
  helpers (clz/ctz/cpop, rotate, orc.b, rev8, and a portable two-word carry-less
  product `bm_clmul128` for clmul/clmulh/clmulr) do the work.
- `src/softfloat.{h,c}` — from-scratch, correctly-rounded IEEE-754 binary32/
  binary64 (M20). One format-parameterised core serves both widths
  (`unpack → operate → round`); results are host-independent (no host FPU, no
  `__int128` — wide intermediates use two-word 128-bit and four-word 256-bit
  helpers). Add/sub/mul/div/sqrt, fused multiply-add (single rounding), the
  comparisons, min/max, classify, and all int↔float and f32↔f64 conversions with
  RISC-V semantics (the canonical NaN for any NaN result, saturating float→int).
  Every op takes a rounding mode and ORs the exceptions it raises into a flags
  word. Pure and dependency-free; `cpu.c` is its only caller.
- `src/mmu.{h,c}` — virtual memory: Sv32 (M12) and Sv39 (M18). One
  descriptor-parameterised page-table walker serves both — a two-level walk with
  4-byte PTEs for Sv32, a three-level walk with 8-byte PTEs for Sv39 — plus a
  small software TLB (in the CPU struct), permission and A/D-bit handling, and
  the page-fault decision. `mmu_translate(cpu, va, acc, &pa)` returns 0 or a
  page-fault cause; `cpu.c` calls it in the fetch and load/store/AMO paths and
  raises the cause as a trap. `satp.MODE` picks the scheme (and is WARL —
  `mmu_satp_supported` drops a write selecting a mode we don't model, so a guest
  probing Sv57/Sv48/Sv39 sees the unsupported ones not stick). Translation is the
  identity in M-mode or Bare mode, so it is inert until a guest writes `satp`.
  `mmu_flush` (called on `sfence.vma` and satp writes) drops the TLB.
- `src/disasm.{h,c}` — RV32I disassembler: turns an instruction word back into
  objdump-style assembly (ABI names, common pseudo-instructions, absolute
  branch/jump targets). Mirrors `cpu_step`'s opcode switch over `decode.h`. A
  compressed (RV32C) halfword is detected by its low two bits, expanded via
  `rvc_expand`, and disassembled as the 32-bit form — objdump prints the same
  expanded mnemonic (with one compressed-only twist: `c.mv` shows as `mv`). M20
  adds `disasm_fp` for the OP-FP / load-FP / store-FP / fused-op families
  (best-effort for `--trace` readability; the float suite is RV64, out of the
  RV32-only `check-disasm`). M21 adds `disasm_bitmanip` for the Zba/Zbb/Zbs/Zbc
  families across the four ALU opcodes; it matches binutils exactly, so the RV32
  bit-manip test *is* pinned by `check-disasm`.
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
  `elf_load` takes a `min_size`: the region spans at least that many bytes, so a
  caller (the `--memory` flag, via `quanta_load_elf_ex`) can leave spare RAM above
  the image for an OS-style guest to manage; the DTB `/memory` node reports it.
- `src/syscall.{h,c}` — the system-call layer (the "kernel" side of ECALL):
  dispatches on the `a7` syscall number and implements `write` and `exit` per
  the RISC-V Linux/newlib ABI. Reached by the SEE for `ecall`s from M/U mode; an
  S-mode `ecall` goes to the SBI instead (see `sbi.c`).
- `src/sbi.{h,c}` — the SBI firmware interface (M15): the "firmware" side of an
  S-mode `ecall`. `sbi_call` dispatches on the extension id (`a7`) and function
  id (`a6`) and implements the Base extension (probe/version), console
  putchar/getchar, TIME `set_timer`, the full HSM extension (M22:
  `hart_start`/`hart_stop`/`hart_suspend`/`hart_get_status`), and SRST
  `system_reset`/shutdown (which halts the machine). Self-contained — it
  reads/writes the calling hart's registers and, for `set_timer`, the CLINT; HSM
  reaches sibling harts through `cpu->mem->plat->harts` (M19's shared array) to
  park/restart them. The SEE in `cpu.c` routes S-mode `ecall`s here when no guest
  M-mode handler is installed.
- `src/quanta.{h,c}` — the public `libquanta` engine API: an opaque `Quanta *`
  handle wrapping CPU + memory + the optional cache, with lifecycle, ELF/raw-image
  loading, `quanta_step`/`quanta_run`, and register/memory accessors. The engine
  core never calls `exit()` on its host — every stop is a `HaltReason` (an
  out-of-range access becomes `HALT_MEM_FAULT`), surfaced through the public
  `QuantaStatus`/`QuantaHalt` enums. The M14 boot handoff lives here too:
  `setup_boot` (run only on the ELF path) builds the device tree via `dtb.c`,
  places it atop guest memory, and sets `a0`=hartid/`a1`=DTB/`sp`-below-DTB;
  `quanta_dtb_addr` reports where it landed. The **snapshot/restore** primitive
  (E10) lives here too: `quanta_snapshot` deep-copies the whole mutable machine
  into an opaque `QuantaSnapshot`, `quanta_restore` puts it back and re-wires the
  borrowed pointers, and `quanta_save_snapshot`/`quanta_load_snapshot` serialise
  that to/from a self-describing file (the `--snapshot`/`--restore` flags) — the
  foundation for record/replay and reverse debugging (see the snapshot gotchas).
  Built as `libquanta.a`; the CLI and `examples/embed.c` are clients of it.
- `src/gdbstub.{h,c}` — a GDB remote-serial-protocol server over TCP (E9):
  `quanta_gdb_serve(q, port)` listens on localhost, accepts one debugger, and
  drives the machine through the public `quanta.h` API alone — registers, memory,
  single-step, halt reason — answering the `g`/`G`/`m`/`M`/`p`/`P`/`c`/`s`/`vCont`/
  `Z`/`z`/`qXfer` packets and serving an RV32 target description. Breakpoints are
  tracked here and enforced in the continue loop, so guest memory is never
  patched. **Reverse execution** (E10) rides on the snapshot primitive: the stub
  keeps a monotonic step count and a ring of machine snapshots, and answers `bs`/
  `bc` (reverse-step/-continue) by restoring the nearest checkpoint and replaying
  forward (see the reverse-debugging gotcha). Reached via `--gdb` from `main.c`, and embeddable. Its POSIX-sockets
  feature macro is local to the `.c` — one of the project's two OS-specific
  corners (the other is `main.c`'s console input).
- `src/main.c` — the CLI driver, a thin client over `quanta.h`: argument parsing,
  the `--trace` narration, the `--pipeline`/`--cache` overlays, the `--gdb` stub
  hand-off, the `--signature` arch-test dump, `--disk` attachment, the
  `--netdev=user` network backend (creating the `netstack` and bridging it to the
  virtio-net device, M23), the
  `--snapshot`/`--restore` machine state save/resume (E10), the
  `--bios`/`--kernel` firmware-boot path (loads an M-mode firmware that hands off
  to an S-mode OS via `quanta_load_firmware`), the
  `--max-steps` runaway cap (0 = uncapped, for a booting OS), and the console
  input pump (host stdin → the UART, via a zero-timeout `select`, with a tty put
  in raw mode for the run and restored on every exit path — see the console-input
  gotcha) — all driving the machine through the public API (no engine internals).
  Its own POSIX feature macro is local to the file, mirroring `gdbstub.c`. Loads
  an ELF named on the command line, or runs the built-in demo when none is given.
- `examples/embed.c` — minimal embedding example: ~30 lines that load and run a
  guest through `libquanta` (`make embed`).
- `tests/hello.S` — sample RV32I assembly, mirrors the built-in demo.
- `tests/hello_world.S` — syscall demo: prints a string with `write`, then
  `exit`s.
- `tests/test_framework.h` + `tests/test_*.S` — the RV32I conformance suite:
  per-group assertion programs that exit 0 on success or the failing check's
  id. `make check` runs them and reads quanta's propagated exit code.
- `tests/test_rvc.S` — the M11 RV32C suite: checks each compressed instruction's
  *semantics* with explicit `c.*` instructions (the x8..x15 forms for c.sub/c.lw/
  c.beqz/…), proving the expansions' immediate scrambles and fields. Plain
  user-mode integer code, so it assembles `-march=rv32ic` and *is* differential-
  tested against qemu, which also implements C.
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
- `tests/test_bitmanip.S` — the M21 RV32 bit-manip suite (Zba/Zbb/Zbs/Zbc), 32
  checks over the RV32-applicable forms (no `*W`/`.uw`). Plain user-mode integer
  code assembled `-march=rv32i_zba_zbb_zbs_zbc`, so it is pinned by `make check`,
  differential-tested against qemu-riscv32 (`check-diff`), *and* disassembly-pinned
  against objdump (`check-disasm`). Its RV64 sibling
  `tests/rv64/test_rv64_bitmanip.S` (43 checks, adding the `*W`/`.uw` forms and
  6-bit shift immediates) is in `check-rv64`'s qemu-riscv64 differential.
- `tests/test_vm.S` — the M12 Sv32 suite: builds a page table by hand, enables
  paging, and drops to S-mode, proving non-identity translation (two VAs aliased
  to one frame), hardware dirty-bit update, and a precise load page fault caught
  by an M-mode handler. Uses satp/supervisor CSRs, so `-march=rv32i_zicsr` and
  out of `make check-diff`.
- `tests/test_irq.S` — the M13 device/interrupt suite: arms the CLINT timer,
  raises a software IPI, and routes a UART interrupt through the PLIC (claim →
  deassert → complete), asserting each machine interrupt fires exactly once with
  the right `mcause`, then prints "uart ok" through the UART. Machine-mode CSRs +
  MMIO, so `-march=rv32i_zicsr` and out of `make check-diff`; `make check` pins
  the exit code and `make check-devices` pins the UART output.
- `tests/test_dtb.S` — the M14 boot suite: plays bootloader, walking the
  flattened device tree handed over in `a1` token by token (BEGIN_NODE / PROP /
  END_NODE), reading big-endian fields by hand. It checks `a0`=hartid, the DTB
  magic/version, recovers the `/memory` reg range and asserts it contains the
  program, and finds a `uart@` device node. Plain `-march=rv32i`; relies on the
  boot DTB user-mode qemu does not provide, so it is out of `make check-diff` and
  pinned by `make check`.
- `tests/test_sbi.S` — the M15 SBI suite: a bare-metal program that `mret`s into
  Supervisor mode and then reaches the machine only through the SBI — probing the
  Base extension, arming `set_timer`, printing "sbi ok" through the SBI console,
  and shutting down via SRST `system_reset` (a clean exit). An unexpected SBI
  error falls to `ebreak` and a non-zero exit. Uses M-mode CSRs + `mret`, so
  `-march=rv32i_zicsr` and out of `make check-diff`; `make check` pins the exit
  code and `make check-sbi` pins the console output.
- `tests/test_stimer.S` — the SBI supervisor-timer suite (M15 follow-up): a boot
  shim delegates the supervisor timer (`mideleg`), installs `stvec`, and drops to
  S-mode; the S-mode loop arms SBI `set_timer` and takes three supervisor timer
  interrupts (each handler re-arms), then shuts down via SRST. Proves the
  firmware STIP relay end to end. `-march=rv32i_zicsr`, out of `make check-diff`,
  pinned by `make check`.
- `tests/os/` — the M16 teaching kernel: a small from-scratch S-mode kernel that
  boots on Quanta and runs a userspace process, integrating M8–M15. `boot.S` is
  the M-mode entry (delegate user traps to S via `medeleg`/`mideleg`, leave
  `mtvec` 0 so the kernel's own SBI `ecall`s reach Quanta, `mret` into `kmain`),
  the S-mode trap vector (`trap_entry`, a full register save/restore around the C
  `trap_handler`, swapping the kernel trap stack in via `sscratch`), and the
  user-entry trampoline (`enter_user`). `kernel.c` (C) reads RAM from the DTB,
  bump-allocates physical pages from the spare RAM above its image, builds an Sv32
  address space (megapage identity map for the kernel + CLINT/UART MMIO, a U-mode
  code and stack page mapped low), installs `stvec`, sets `sstatus.SUM` to read
  the user's buffers, arms SBI `set_timer` preemption, and `sret`s to user;
  `trap_handler` services the U-mode `write`/`exit` syscalls and counts the timer
  ticks (disarming after three). `user.S` is a position-independent U-mode blob
  the kernel copies into a page; `kernel.ld` is the flat link script at
  `0x80000000`. Built `-march=rv32imac_zicsr -nostdlib` in one `gcc` call (its own
  rule, not the `tests/%.elf` pattern). Run with `./quanta --memory=8M
  tests/os/kernel.elf`; pinned by `make check-os` (and run under `make
  sanitize`/`make coverage`), out of `make check-diff` like the other privileged
  tests.
- `tests/check_os.sh` — boots the M16 kernel under `--memory=8M` and asserts
  M16's "done when": paging came up, the userspace process printed via the `write`
  syscall, the supervisor timer preempted it the expected number of times, and the
  kernel shut down cleanly (exit 0). `make check-os`.
- `tests/check_disasm.sh` — runs each sample ELF under `--trace` and diffs the
  disassembly against `objdump` (`make check-disasm`).
- `tests/check_cache.sh` — runs `test_stack` under two cache geometries and
  asserts results are unchanged and a smaller cache misses more
  (`make check-cache`).
- `tests/check_diff.sh` — differential test: runs each sample ELF under
  `quanta --quiet` and a reference simulator (qemu-riscv32 by default, override
  with `REF=`) and asserts they agree on stdout and exit code (`make
  check-diff`). Skips cleanly if the reference is absent. The privileged tests
  (`test_csr`, `test_trap`, `test_priv`, `test_vm`, `test_irq`) are excluded —
  their machine-mode CSR, trap, paging, and MMIO use trips user-mode qemu's own
  supervisor; `test_dtb` is excluded too (it parses the boot device tree qemu
  does not supply). `make check` pins them all instead.
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
  and continues to exit on `tests/hello.elf`, and — for E10 — reverse-steps (`bs`)
  and reverse-continues (`bc`) over the known instruction sequence, asserting the
  known outcomes (`make check-gdb`). Skips cleanly without python3; also run under
  `make sanitize` and `make coverage`.
- `tests/snapshot_test.c` — the E10 snapshot/restore conformance harness
  (`make check-snapshot`): links `libquanta` directly and, for each guest, runs to
  completion as a control, then reruns taking a snapshot at the midpoint and
  replaying the tail — asserting the final registers, memory, exit, and step count
  match bit-for-bit. Guests span the state (stack/array, muldiv, atomics, and the
  device/interrupt test for device-state capture). Also run under `make sanitize`/
  `make coverage`.
- `tests/check_replay.sh` — the E10 snapshot *file* round-trip (`make check-replay`,
  `--snapshot`/`--restore`): splits a run with a mid-run snapshot and asserts the
  resumed half reproduces the whole run's output and exit, that a halted-machine
  snapshot round-trips, and that a corrupt file is rejected cleanly. Drives the CLI;
  needs the cross-toolchain. Also run under `make sanitize`/`make coverage`.
- `tests/check_devices.sh` — runs `test_irq` and asserts both halves of M13's
  "done when": a clean exit (timer, IPI, and PLIC external interrupts all fired)
  and the UART's "uart ok" reaching stdout (`make check-devices`). Also run under
  `make sanitize` and `make coverage`.
- `tests/check_sbi.sh` — runs `test_sbi` and asserts both halves of M15's "done
  when": a clean exit (the S-mode program made its SBI calls and shut down via
  SRST) and the SBI console's "sbi ok" reaching stdout (`make check-sbi`). Also
  run under `make sanitize` and `make coverage`.
- `tests/uart_echo.S` + `tests/check_uart_rx.sh` — the M18 console-input test
  (`make check-uart-rx`): pipes a known line into the `uart_echo` guest, which
  echoes each byte back through the UART, asserting host stdin reaches the guest's
  UART receive path; plus a `--disk` smoke (an image attaches; a missing file
  errors). The guest needs host input to terminate, so it is out of `make check`
  and the objdump/qemu suites. Also run under `make sanitize`/`make coverage`.
- `tests/console_pty.py` + `tests/check_console.sh` — the raw-mode interactive
  console test (`make check-console`): drives the same `uart_echo` guest over a
  real pseudo-terminal (so `isatty` is true and `console_raw_enable` engages — the
  path the piped `check-uart-rx` cannot reach), asserting the raw-mode banner,
  character-at-a-time echo, the `Ctrl-A x` quit and `Ctrl-A Ctrl-A` literal
  escapes, and the safety property that a `SIGTERM` restores the terminal it
  saved. Self-contained (no riscv `gdb`, like `gdb_client.py`); skips cleanly
  without python3. Also run under `make sanitize`/`make coverage`.
- `tests/opensbi_payload.S` + `tests/check_opensbi.sh` — the OpenSBI firmware-boot
  test (`make check-opensbi`, M18 / the road to Linux): boots the qemu-prebuilt
  OpenSBI (fw_dynamic) as `--bios` and a small rv64 S-mode payload as `--kernel`,
  asserting OpenSBI hands off to S-mode, the payload prints through the SBI console
  (an `ecall` serviced by OpenSBI running on Quanta), and the machine shuts down
  cleanly via SBI SRST (the SiFive test device → exit 0). The payload is built to
  a raw `.bin` at 0x80200000 (`-mno-relax`, like a Linux Image), so it is filtered
  out of `TEST_SRC` and has its own build rule. Skips cleanly without an OpenSBI
  binary (`$QUANTA_OPENSBI` or qemu's default path), like `check-diff` without
  qemu. Also run under `make sanitize`/`make coverage`.
- `tests/linux/` — the M18 Linux userspace (the `OpenSBI → Linux → /init` boot):
  `init.c` is a freestanding RV64 `/init` (no libc, raw Linux `ecall` syscalls, a
  tiny interactive line shell that powers off via the reboot syscall → SBI SRST);
  `mkcpio.c` is a self-contained host packer that writes the kernel's newc-cpio
  format directly (synthesising the `/dev/console` node the kernel opens as PID
  1's console, so no `cpio` tool or root is needed); `mkinitramfs.sh` builds both
  and packs `build/linux/initramfs.cpio` (`make linux-initramfs`). The kernel
  `Image` and OpenSBI firmware are external artifacts supplied at run time, so
  there is no `make` boot target — a manual milestone like xv6; `README.md` has
  the recipe.
- `tests/rv64/test_rv64_virtio.S` + `tests/check_virtio.sh` — the M18 virtio-mmio
  block-device test (`make check-virtio`): a bare-metal RV64 machine-mode driver
  probes the device identity, runs the status/feature handshake, sets up a split
  virtqueue, then reads sector 0 (checking a magic the harness wrote into the
  `--disk` image) and writes/reads-back another sector (DMA both ways), and
  confirms the device raised its PLIC interrupt. Machine-mode + MMIO + virtio and
  needing a `--disk`, so it is quanta-only (out of the qemu-riscv64 differential)
  and excluded from `check-rv64`'s generic runner (`RV64_RUN`); it is built
  `-mno-relax` so `la` on its in-RAM queue stays PC-relative (see gotchas). Also
  run under `make sanitize`/`make coverage`.
- `tests/rv64/test_rv64_plic.S` — the M18 S-mode external-interrupt test (run by
  `make check-rv64`): an M-mode shim delegates the supervisor external interrupt
  and drops to S-mode, which arms the PLIC's **S-mode context** (context 1) for
  the UART source, raises a UART interrupt, and takes it as a supervisor external
  interrupt — claiming through the context-1 registers and asserting the claimed
  source is the UART, the path a booting xv6's `virtio_disk_intr`/`uartintr`
  reach. Uses M-mode CSRs + MMIO, so it is quanta-only (excluded from the
  qemu-riscv64 differential like `*_priv`/`*_vm`/`*_sstc`).
- `tests/rv64/test_rv64_fpu.S` — the M20 F/D conformance suite (run by
  `make check-rv64`): 44 user-mode checks spanning single/double
  add/sub/mul/div/sqrt, the fused ops, sign-injection, min/max (with signed zero
  and NaN), the compares (NaN → 0, FLT/FLE signalling), classify, float↔int
  conversions (including saturation of NaN/overflow and the negative-to-unsigned
  case), the moves, the accrued exception flags, and a dynamic rounding mode. It
  moves float results to integer registers with `fmv.x.w`/`fmv.x.d` and asserts
  the exact IEEE bit patterns, so it is **differentially tested against
  qemu-riscv64** — any quanta≠qemu divergence fails one side's CHECK. Built
  `-march=rv64imafdc_zicsr` (a per-target Makefile override); plain user-mode, so
  unlike the privileged RV64 tests it stays in the differential.
- `tests/rv64/test_rv64_smp.S` + `tests/check_smp.sh` — the M19 SMP test
  (`make check-smp`, run with `--harts=4`): all four harts enter at the entry with
  `a0`=hartid; each checks `mhartid`, does 500 LR/SC increments of one shared
  counter (the round-robin scheduler makes sibling `sc` stores break a hart's
  reservation mid-sequence, so the total must still be `4*500` — a broken
  cross-hart reservation loses updates), and joins an `amoadd.d` barrier. Then
  hart 0 verifies the total and sends hart 1 a CLINT IPI, taken as a real machine
  software interrupt. A clean exit 0 means every stage passed; a non-zero code
  (16/17/18) is the failing stage, written to the SiFive test finisher. Built
  `-mno-relax` (like the virtio test — `la` on its in-RAM words must stay
  PC-relative, gp is not the global pointer). Quanta-only, excluded from
  `check-rv64`'s generic runner (`RV64_RUN`).
- `tests/rv64/test_rv64_hsm.S` + `tests/check_hsm.sh` — the M22 SBI HSM test
  (`make check-hsm`, run with `--harts=4`): all four harts enter in M-mode and drop
  to S-mode with no `mtvec` (so their `ecall`s reach Quanta's built-in SBI —
  Quanta is the firmware). The three secondaries park themselves via SBI
  `hart_stop`; hart 0 waits for each to report `STOPPED` (`hart_get_status`),
  `hart_start`s each at a `worker` that `amoadd.d`-increments a shared counter and
  stops again, then checks the counter reached `NHARTS-1`, the harts are `STOPPED`
  again, hart 0 itself is `STARTED`, and the two error cases (out-of-range hartid →
  `INVALID_PARAM`, already-started hart → `ALREADY_AVAILABLE`). Clean exit 0 via the
  SiFive test finisher; a non-zero code (16..23) is the failing stage. Quanta-only
  (multi-hart + SBI, no qemu differential), built `-mno-relax`, excluded from
  `RV64_RUN` like the SMP test.
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

- RV32 vs RV64 (M17) is a **runtime** property, not two builds: `cpu->xlen` is 32
  or 64, set from the ELF class by the loader (raw-image/demo default to 32). All
  state — registers, PC, CSRs, addresses — is stored in 64-bit fields. The
  invariant is the **Spike sign-extension convention**: in RV32 a register holds
  the sign-extension of its 32-bit value (upper half = copy of bit 31), enforced
  by `sext_xlen()` applied in `reg_write` and at the PC retire in `cpu_step`. So
  the executor is mostly width-agnostic — add/sub/compare/logic give the right
  XLEN result once `reg_write` re-sign-extends — and only the **right shifts**
  (they pull the high bits down: use `(uint32_t)a` / `(int32_t)a` in RV32),
  the **shift amount** (5 bits RV32, 6 bits RV64), and the width-defining ops
  need an `xlen` branch. `imm_*` immediates sign-extend to 64 for both widths, so
  PC/address arithmetic uses `(uint64_t)imm_*` and masks like `& ~(uint64_t)1`.
  A subtle corollary the OS-boot work exposed: **branch comparisons must use the
  full 64-bit register values** (`exec_branch` reads `uint64_t`, casts `int64_t`
  for BLT/BGE), not the low 32 bits. Truncating breaks an RV64 `bltu`/`bgeu`
  whose operands differ above bit 31 (e.g. a page-table loop bound `0x4000000000`
  vs a VA `0x3ffffff000`). Sign-extended storage means the same full-width
  compare is also correct for RV32 (sext preserves both signed and unsigned
  ordering of the low 32 bits), so one code path serves both. Pinned by the
  high-address branch checks in `tests/rv64/test_rv64.S` (qemu-verified).
- Because registers/PC are sign-extended internally, the **public API returns the
  architectural value masked to XLEN** (`xlen_val()` in `quanta.c`): RV32
  `quanta_reg`/`quanta_pc` hand back the low 32 bits (zero-extended), so a
  returned PC is a valid address for `quanta_read_u32`/memory access and displays
  as 8 hex digits; RV64 hands back the full 64. `main.c` and `quanta_dump_regs`
  pick the field width from `quanta_xlen()`.
- The MMU masks a VA to XLEN at its **single choke point** (`mmu_translate`
  first recovers the real address from a sign-extended RV32 register). One walk
  loop serves both schemes, parameterised by a small descriptor (levels, PTE
  width, VPN-field width, PPN mask): RV32 selects **Sv32** (2 levels, 4-byte
  PTEs) and RV64 selects **Sv39** (3 levels, 8-byte PTEs), by `satp.MODE`
  (M18). `misa.MXL` is 1 (RV32) / 2 (RV64); the mcause/scause
  **interrupt bit** is the top XLEN bit (bit 31 RV32, bit 63 RV64) via
  `cause_interrupt()`; the RV32-only high-half CSRs (`cycleh`/`timeh`/`instreth`/
  `mstatush`) trap illegal on RV64.
- The RV64-only instructions (OP-32/OP-IMM-32 `*W`, LD/SD/LWU, the `.D` atomics,
  and the RV64C forms via `rvc_expand(c, rv64)`) are **gated to raise illegal in
  RV32**, so every RV32 test is unaffected — the widening kept `make check`,
  `check-arch`, `check-diff`, and `check-os` bit-for-bit green (the refactor's
  safety checkpoint). M20 added F and D on top (RV64IMAFDC); the XLEN-dependent
  float ops (`FCVT.L/LU`, `FMV.X.D`/`FMV.D.X`) are likewise gated to RV32-illegal.
- Softfloat is host-independent by construction (M20): `softfloat.c` never touches
  the host FPU — a host `double` might carry 80-bit x87 intermediates, contract
  `a*b+c`, or round differently, so a from-scratch implementation is the only way
  results stay bit-identical across hosts and match qemu. It also avoids
  `__int128` (not standard C11): a double product is 106 bits, a fused-multiply-add
  sum can span ~160 bits when the product and addend cancel, so the accumulator is
  a four-word `u256` (the same reason `mulhu64` builds the high product from 32-bit
  partials). Division and square root are exact integer long-division / bitwise
  `isqrt` (no reciprocal tables — those would be committed third-party data). The
  library was validated **before** wiring in, against the host FPU as an oracle
  (90M random+edge cases, four rounding modes, bit-exact results + NV/DZ/OF/NX
  flags — the oracle in `scratchpad` is not committed); RISC-V-specific behaviour
  (canonical NaN, saturating float→int, RMM, NaN-boxing, UF) is pinned against
  qemu-riscv64 by `test_rv64_fpu.S`. When changing the walker of any float op,
  re-run an oracle-style sweep — a wrong `isqrt` trial or a lost sticky bit is a
  1-ULP bug invisible to spot checks. **isqrt gotcha**: the digit-recurrence
  trial is `4*root + 1` (the increment `(2r+1)² − (2r)²`), not `2*root + 1`.
- Float `mstatus.FS` is tracked but **not gated** (M20): a float write marks FS
  Dirty and sets SD, but Quanta never raises illegal-instruction when FS is Off.
  This is deliberate — gating would force `test_rv64_fpu.S` to write `mstatus.FS`,
  an M-mode CSR user-mode qemu rejects, which would drop the test out of the
  qemu-riscv64 differential (the strongest net for IEEE edge cases). Permissive
  execution keeps it user-mode and differentiable while a full-system guest still
  sees the dirty state it needs for lazy FP context switching. Consequence: a
  future `-march=rv64gc` OS boot that relies on an FS-Off trap would need gating
  added. Relatedly, the boot **DTB isa string stays `rv64imac`** (only `misa`
  advertises `fd`) so the imac-built xv6/Linux guests are not perturbed into
  probing float; a gc guest supplies its own DTB.
- Single-precision values are **NaN-boxed** in the 64-bit float registers: a
  single write sets the upper 32 bits to all ones (`fwrite_s`), and a single read
  of an improperly-boxed register returns the canonical NaN `0x7fc00000`
  (`fread_s`) — matching qemu. `FMV.X.W`/`FSW` move the *raw* low 32 bits (no box
  check); doubles use the full 64 bits. RV32F/D remain deferred (M11) as a target
  in the sense that no RV32 guest is float-tested, but the softfloat and exec paths
  are width-agnostic, so RV32 float works save the RV64-only moves gated above.
- The RV64 conformance suite is `tests/rv64/` (own Makefile rule, built
  `-march=rv64imac_zicsr -mabi=lp64`, so the RV32 `tests/*.S` glob never touches
  it), run by `make check-rv64` — quanta exit-0 plus a `qemu-riscv64` differential
  on the user-mode programs. The supervisor/paging programs — `*_priv`,
  `test_rv64_vm` (the M18 Sv39 test: a hand-built three-level table, aliasing,
  the hardware dirty bit, and a deep-miss load page fault, mirroring the RV32
  `test_vm.S`), and `test_rv64_sstc` (the M18 Sstc timer test: three supervisor
  timer interrupts driven by `stimecmp`, mirroring `test_stimer`) — are
  quanta-only (user-mode qemu rejects their M-mode CSRs, satp, and Sstc),
  excluded from the differential like the RV32 privileged tests are from
  `check-diff`. `test_rv64_priv.S` is built **without C** (`rv64ima_zicsr`)
  because its trap handler advances `mepc` by a fixed 4, so its `ebreak` must
  stay 4-byte (the same reason the RV32 `test_trap.S` avoids C); `test_rv64_vm.S`
  sets `mepc` directly, so it keeps C.
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
- RV32C (M11) is handled by *expansion*, not a second decoder: `rvc_expand`
  (`rvc.c`) widens a 16-bit instruction to its 32-bit equivalent, and the normal
  decode/execute and disasm run unchanged. Key consequences of variable length:
  (1) `cpu_step` fetches a **halfword first** and decides the length from its low
  two bits (`!= 0b11` is compressed); a 32-bit instruction's upper half is
  translated separately because it can straddle a page. (2) The PC advances by
  the real length `ilen` (2 or 4) — which is also why the branch fall-through and
  the JAL/JALR **link** address use `ilen`, not a hardcoded `+4` (the easy bug:
  a not-taken compressed branch must fall to `pc+2`). (3) Instruction alignment
  relaxes to IALIGN=16, so the misaligned-fetch check is `pc & 1`, not `& 3`, and
  `misa` advertises C. The disassembler prints the expanded mnemonic (objdump
  does too), special-casing only `c.mv` (→ `mv`, since a 32-bit `add rd,x0,rs`
  stays `add`). `--trace` shows compressed instructions as 4 hex digits. The test
  assembles `-march=rv32ic` and is differential-tested against qemu.
- RV32A (M10) atomics live in `exec_amo` under the AMO opcode (`0x2f`, funct3
  `0x2`), mirrored in `disasm.c`. The aq/rl ordering bits are no-ops on a single
  in-order hart. LR.W holds a word-granularity reservation
  (`reserve_valid`/`reserve_addr`) that SC.W consumes and any store to the same
  word voids; SC returns 0 on success, 1 on failure. Atomics fault on a
  misaligned address (base load/store still handle misalignment silently). The
  test is user-mode (`-march=rv32ia`) and differential-tested against qemu — its
  SC-failure case overwrites the reserved word with a *different* value, so an
  address-based reservation (ours) and a value-based one (qemu-user) agree.
- Bit-manipulation (M21, Zba/Zbb/Zbs/Zbc) is decoded by four `exec_bitmanip_*`
  intercepts in `cpu.c`, one per reused opcode (OP/OP-IMM/OP-32/OP-IMM-32), each
  called *before* the base ALU switch and returning 0 to fall through — so the
  base decode is untouched and reserved encodings stay as lenient as before. Decode
  traps to watch: (1) Zbb's andn/orn/xnor sit in the **funct7 == 0x20** group the
  base uses for SUB/SRA, and clmul* share **funct7 == 0x05** with min/max — both
  split by funct3, so the intercept returns 0 for the base/other funct3 values.
  (2) The shift-immediate ops (bclri/binvi/bseti/rori/bexti and slli.uw) are
  discriminated by **funct6** (bits [31:26]), not funct7, because their shamt is 6
  bits in RV64 (bit 25 is shamt, not funct7); the unary scans (clz/ctz/cpop/
  sext.b/sext.h and clzw/ctzw/cpopw) use the full funct7 == 0x30 plus a fixed rs2.
  (3) rev8's immediate differs by width (funct7 0x34 on RV32, 0x35 on RV64), and
  `bm_rev8`/`bm_orcb`/`bm_clz` reverse/scan `cpu->xlen` bits. Width handling
  otherwise rides the sext invariant: results go through `reg_write`, so only the
  named-width ops (`*W`, `.uw`, the whole-register scans) branch on XLEN.
  (4) Carry-less multiply avoids `__int128`: `bm_clmul128` builds the 2·XLEN-bit
  product in two words and `bm_bits128` slices the low (clmul) / high (clmulh) /
  reversed (clmulr) field — its shifts are guarded into `[0,63]` so the analyzer
  stays clean. `misa` advertises `B`; the disassembler matches binutils so the
  RV32 test is in `check-disasm`, and both tests are qemu-differential.
- Paging (Sv32, M12; Sv39, M18) lives in `mmu.c`, not `memory.c` — translation
  needs CPU state (satp/priv/mstatus), while `memory.c` stays a dumb physical
  array. **One walk loop serves both schemes**, parameterised by a descriptor
  set from `satp.MODE`: Sv32 is 2 levels / 4-byte PTEs / 10-bit VPN fields /
  22-bit PPN; Sv39 is 3 levels / 8-byte PTEs / 9-bit VPN fields / 44-bit PPN.
  The superpage merge (low `level` VPN fields come from the VA, and the PTE's
  matching PPN bits must be zero) is uniform, so it covers the Sv32 megapage and
  the Sv39 mega/gigapage with the same three lines. Sv39 adds one check the walk
  does up front: the VA must be **canonical** (bits [63:39] a sign-extension of
  bit 38) or it faults. Key points otherwise shared: translation is the identity
  in M-mode or Bare mode, so paging is inert until a guest sets `satp` (every
  pre-paging test is unaffected). Page tables are read *physically* by the
  walker, so they need no mapping. A/D bits are set in hardware (A on any access,
  D on a store); the TLB therefore serves only fetches and loads — stores always
  walk so the dirty bit lands on the real PTE — and is flushed by `sfence.vma`
  and any `satp` write. `mstatus.MPRV` lets an M-mode load/store translate as
  MPP; SUM/MXR gate S-mode access to user pages. A walk failure (missing PTE, bad
  permission, misaligned superpage, non-canonical Sv39 VA) returns the page-fault
  cause, which `cpu.c` raises as a trap with the faulting VA in `*tval`. Not
  modelled: the access-fault-vs-page-fault distinction for a page-table read that
  leaves RAM (treated as a page fault), ASID-scoped flushes (`sfence.vma` drops
  the whole TLB), the Sv39 reserved/`Svpbmt`/`Svnapot` high PTE bits (ignored,
  not faulted — no guest sets them since we don't advertise those extensions),
  and RV64 Sv48/Sv57 (their `satp.MODE` is rejected as unsupported). Like the
  other privileged tests, `test_vm` (Sv32) and `test_rv64_vm` (Sv39) can't run
  under user-mode qemu, so they have no differential safety net — lean on
  `--trace` when changing the walker.
- Platform devices and interrupts (M13) live in `device.c`; the MMIO *dispatch*
  is in `memory.c`, gated on `mem->plat` (NULL = plain RAM, e.g. a Memory not
  wired through `start_at`). The platform is attached to every loaded machine but
  inert until programmed: no pre-M13 test enables an interrupt, so all are
  unaffected. Key points: MMIO addresses are matched on the **physical** address
  (after `mmu_translate`) — `test_vm`'s `0x10000000` is a deliberately-unmapped
  *VA* that faults at translation, never reaching the UART there. `mtime` ticks
  once per `cpu_step` (deterministic, not wall-clock), and `mtimecmp` resets to
  all-ones so the timer is quiet until armed. `MTIP`/`MSIP`/`MEIP`/`SEIP` are
  read-only reflections of device state, recomputed into `mip` each step by
  `effective_mip` (software-writable `mip` bits like `SSIP`/`STIP` are left
  alone). The PLIC has **two contexts** (M18): context 0 = hart-0 M-mode → MEIP,
  context 1 = hart-0 S-mode → SEIP, each with its own enable/threshold/claim (the
  qemu virt layout: S-context enable at `0x2080`, threshold/claim at `0x201000`/
  `0x201004`). An S-mode OS (xv6) claims/completes through context 1 and takes
  the interrupt as SEIP once it delegates bit 9 via `mideleg` — `test_irq.S`
  drives context 0, `test_rv64_plic.S` context 1. Interrupts are taken
  at the instruction boundary **before** fetch, so `*epc` is the interrupted
  instruction and the handler resumes it (do **not** advance `*epc` the way a
  synchronous handler skips its trapping instruction). `enter_trap` is shared by
  `raise_trap` (synchronous) and `take_interrupt`; vectored `*tvec` applies only
  to interrupts. Addresses follow the qemu `virt` map (CLINT `0x02000000`, PLIC
  `0x0c000000`, UART `0x10000000`). The UART transmit prints straight to stdout
  (like the `write` syscall), so it composes with `--quiet`.
- Device tree and boot protocol (M14): `dtb.c` *generates* the flattened tree
  (no external `dtc`, no committed `.dtb`), and `quanta.c`'s `setup_boot` does the
  handoff — **only on the ELF path**. The raw-image/demo/embed path (`quanta_load_image`)
  is unchanged: no tree, `a0`/`a1` = 0. Key points: the tree is placed in the
  top of the loader's 64 KiB stack headroom (so no `elf.c` resizing) and `sp` is
  moved just below it; `a0` = hartid stays 0 (it already was), and the only
  register change for existing ELF tests is `a1` = DTB pointer — harmless because
  they set their own registers and never read `a1` uninitialised (verified across
  `make check`, `make check-arch`, and `make check-diff`, where Quanta still
  matches qemu despite qemu supplying no DTB). The `/memory` node reports the real
  region base — typically `0x7ffff000`, the page the linker puts below `-Ttext`,
  **not** `0x80000000` — so a reader should believe the tree, not assume the
  entry address. The root uses `#address-cells`/`#size-cells` = 2, so each `reg`
  address/size is a *pair* of cells with the high cell 0 on this 32-bit machine.
  Multi-byte FDT fields are **big-endian** (the one big-endian corner in an
  otherwise little-endian project). `quanta_dtb_addr` and the CLI banner report
  where it landed.
- SBI and the SEE split (M15): the built-in SEE (`legacy_trap` in `cpu.c`, taken
  when a trap finds no guest handler — `*tvec` still 0) now routes `ecall` by
  **originating privilege**: an **S-mode** `ecall` (cause 9) goes to `sbi_call`
  (`sbi.c`, Quanta acting as M-mode firmware), while **M/U-mode** `ecall`s
  (causes 11/8) keep going to the newlib `syscall_dispatch` (`write`/`exit`). So
  every pre-M15 program — all of which run in M-mode — is unchanged; only a guest
  that deliberately drops to S-mode reaches the SBI. Consequence of the model:
  the SBI is available **only while `mtvec` is 0**. On real hardware `mtvec`
  points at the firmware (OpenSBI), which *is* the SBI; here Quanta is that
  firmware precisely when the guest has installed no M-mode handler, so a guest
  cannot have both its own M-mode trap handler and the SBI. SBI console output
  goes straight to stdout (like the UART and the `write` syscall), so it composes
  with `--quiet`.
- SBI return values are **XLEN-wide sign-extended longs** (M22, a real RV64 bug
  fixed): `sbi_return` writes the (negative) error code into `a0` and a value into
  `a1`. It once cast the error through a `uint32_t`, which zero-extends in the
  64-bit register — so an RV64 supervisor reading `a0` as a `long` saw
  `0x00000000fffffffd` instead of `-3`, and any code comparing against a
  sign-extended `-3` mismatched. `sbi_return` now sign-extends the error
  (`(uint64_t)(int64_t)error`); the value fields are small non-negatives, so
  zero-extending them is equivalent. This was invisible for a decade of milestones
  because every earlier SBI test only checked `a0 != 0` (call failed vs. not),
  never a specific negative code — the HSM test's explicit `INVALID_PARAM`/
  `ALREADY_AVAILABLE` comparisons are the first to pin the exact value.
- SBI HSM — hart state management (M22): the firmware side of SMP hart bring-up,
  in `sbi_hsm` (`sbi.c`), reached only when Quanta is the firmware (a from-scratch
  SMP kernel on the direct boot; under OpenSBI this path is bypassed and OpenSBI
  provides HSM itself). Each hart carries an `hsm_state` (`HSM_STARTED`/`STOPPED`/…,
  initialised `STARTED` in `cpu_init`). `hart_stop` parks the calling hart
  (`halted=1`, `HSM_STOPPED`) — a stopped hart's round-robin slot is a no-op until
  restarted, and it does **not** return. `hart_start(hartid,addr,opaque)` resolves
  the sibling through `cpu->mem->plat->harts` (`hsm_hart`, bounds-checked against
  `plat->nharts`), rejects an out-of-range id (`INVALID_PARAM`) or an
  already-started hart (`ALREADY_AVAILABLE`), else `hsm_enter`s it: pc=addr,
  priv=S, a0=hartid, a1=opaque, satp=Bare, `sstatus.SIE` cleared, reservation
  dropped, TLB flushed, `HSM_STARTED`, unhalted. `hart_get_status` returns the
  state; `hart_suspend` retentive (type 0) is a nop-return (WFI is a nop here) and
  non-retentive (type 0x80000000) re-enters at the resume address. The direct-boot
  SMP path (`setup_boot`) still brings **all** harts up at reset (xv6's `-bios
  none` convention), so a kernel that expects that is unaffected; HSM is for a
  kernel that instead parks and wakes harts itself. Pinned by
  `tests/rv64/test_rv64_hsm.S` / `make check-hsm`.
- SMP under an M-mode firmware (M22): booting **Linux SMP under OpenSBI** works —
  `setup_firmware_boot` (quanta.c) now brings **every** hart into the firmware at
  reset (each with a0=hartid, a1=DTB, a2=`fw_dynamic_info`) instead of parking the
  secondaries. OpenSBI's boot-hart lottery cold-boots one and drops it into the OS;
  the rest fall into OpenSBI's HSM wait loop and are released when Linux calls SBI
  `hart_start` (which rings the target's CLINT `msip`). **Linux 6.6 boots
  `--harts=4`** — `smp: Brought up 1 node, 4 CPUs`, and the test `/init`'s `cpuinfo`
  lists all four. The single machine-model gap this needed was **AIA `mtopi`**: see
  the AIA-mtopi gotcha below — without it the inter-processor MSI storms and the
  boot "livelocks" (the symptom the earlier prototype hit and could not get past).
  `nharts==1 --bios` is byte-identical to before (the bring-up loop sets hart 0's
  a0/a1/a2 exactly as the old single-hart code did). The *direct* ELF path
  (`setup_boot`, xv6 `--harts=3`) is unchanged, and Quanta's own SBI HSM still
  serves a from-scratch SMP kernel that uses Quanta as firmware.
- AIA `mtopi`/`stopi` (Smaia/Ssaia top-interrupt CSRs, M22): qemu's prebuilt
  OpenSBI detects these CSRs present — our lenient CSR file *answers* an unknown
  CSR (returns stored 0) rather than trapping, so OpenSBI's trap-probe feature
  detection concludes Smaia is present — and then drives its **entire M-mode
  interrupt dispatch off `mtopi`** (CSR 0xfb0): read the top pending interrupt,
  service it, re-read until zero. Left unimplemented, `mtopi` read 0, so OpenSBI saw
  every machine software interrupt (the IPI that wakes/reschedules a secondary hart)
  as "nothing pending", returned **without clearing the CLINT `msip`**, and the MSI
  re-fired every instruction — an IPI storm that reads as a livelock. It was
  invisible on a uniprocessor (one hart takes no IPI, and its timer uses Sstc, so
  the `mtopi` path is dead code) — the first machine interrupt OpenSBI must dispatch
  in SMP is the wake IPI, which hit it immediately. Fix: `topi_value` in cpu.c
  implements `mtopi`/`stopi` as read-only views of the highest default-priority
  interrupt pending **and** enabled for the level (M-level = `mip & mie & ~mideleg`,
  S-level = the delegated ones), encoded AIA-style (IID in bits [27:16], a non-zero
  IPRIO in [7:0]; 0 when none), ignoring the global `mstatus.MIE` gate the way
  firmware reads it mid-handler. This matches qemu (whose virt CPU implements
  `mtopi`, which is *why* OpenSBI took this path). We model only these two CSRs, not
  the full IMSIC/APLIC — enough for OpenSBI's dispatch; Linux still uses the PLIC
  (our DTB advertises `riscv,plic0`, no APLIC/IMSIC node) and never reads `stopi`.
- SBI supervisor-timer delivery: `set_timer` routes through
  `cpu_arm_supervisor_timer` (cpu.c), which programs the CLINT comparator, clears
  any pending supervisor timer (STIP), and arms `sbi_timer_armed`. Each step
  `firmware_timer_tick` watches for the CLINT to assert MTIP and then raises STIP
  (a one-shot, re-armed by the next `set_timer`) — the firmware relaying the
  machine timer to the supervisor *without* a literal M-mode trap round-trip. The
  machine timer itself is never delivered (an SBI guest leaves `mie.MTIE` clear,
  so MTIP — perpetually asserted once mtime passes the comparator — is harmless);
  only its STIP shadow reaches S-mode, and only when the guest delegates it
  (`mideleg` bit 5) and enables `sie.STIE`/`sstatus.SIE`. The mechanism is inert
  unless a guest calls SBI `set_timer` (so `test_irq`'s machine-timer path, which
  arms the CLINT via MMIO and never touches the SBI, is unaffected). `test_stimer`
  pins the whole shuttle.
- Sstc supervisor-timer delivery (M18, xv6 enabler): the *other* way to reach
  STIP, used by an OS that owns M-mode and wants no firmware round-trip (e.g. xv6
  booted `-bios none`). When `menvcfg.STCE` (bit 63) is set, `sstc_tick` (cpu.c,
  per step) makes STIP a hardware shadow of the `stimecmp` CSR (0x14D) — pending
  exactly while `time >= stimecmp`, overriding software (under Sstc, STIP is
  read-only to S-mode). Writing `stimecmp` arms the next tick; it fires when the
  counter catches up. The clock compared is the `time` CSR (our retired-instruction
  count), so `rdtime` and the deadline share one clock. `STCE` gates the whole
  thing: clear (every pre-M18 guest, and any SBI guest) leaves `firmware_timer_tick`
  and software STIP untouched, so the SBI path and Sstc never fight — a guest uses
  one or the other. `stimecmp` is accessible from S-mode only when `STCE` is set
  (else illegal); M-mode always reaches it. The RV32 high halves (`stimecmph`
  0x15D, `menvcfgh` 0x31A) trap illegal on RV64 like the other `*h` CSRs.
  `tests/rv64/test_rv64_sstc.S` pins it (three ticks, each handler re-arming
  `stimecmp`), the Sstc analogue of `test_stimer`'s SBI shuttle.
- Console input + disk backend (M18, xv6 enablers): the 16550 UART already had
  the receive path (`rx`/`rx_have`, LSR data-ready, the RX interrupt, RBR read);
  what was missing was a *source*. `plat_uart_rx` buffers one host byte (the
  interrupt then follows automatically via `uart_asserted`/`plic_lines`), exposed
  as `quanta_uart_input`, and `main.c`'s `console_pump` feeds host stdin through
  it every ~1024 steps during the run. Readiness is a zero-timeout `select` — the
  code never sets `O_NONBLOCK` on stdin, because that flag is *shared with the
  parent shell* and a crash would leave the shell broken. When stdin is a tty,
  `console_raw_enable` puts it in raw mode for the run — the qemu `-nographic`
  recipe: clear `ICANON`/`ECHO`/`ISIG`/`ICRNL`/`IXON` so keystrokes reach the
  guest one at a time, unechoed by the host, with Ctrl-C and flow-control keys
  delivered as bytes, but **leave `c_oflag` alone** so the guest's bare `\n` still
  displays as CR-LF (ONLCR). A `Ctrl-A` prefix is the escape (`Ctrl-A x` quits,
  `Ctrl-A Ctrl-A` sends one literal `Ctrl-A`). The terminal is *always* restored —
  `console_restore` (idempotent) runs after the loop, via `atexit`, and from
  `SIGINT`/`SIGTERM`/`SIGQUIT`/`SIGHUP` handlers that restore then re-raise
  (tcsetattr/signal/raise are async-signal-safe) — so however the process dies the
  user's shell is handed back intact. A pipe or file is not a tty (`isatty` gates
  it), so it is read verbatim with no escape processing — which is what
  `check-uart-rx`'s piped stdin relies on, keeping the change inert for every test.
  `--disk=FILE` reads a raw image wholly into
  a malloc'd buffer held in `Platform.disk` (engine-owned, freed in
  `quanta_destroy`); the virtio-mmio block device below serves it. `tests/uart_echo.S`
  (a plain-rv32i echo guest) is driven by
  `check-uart-rx` with piped stdin — it is deliberately *not* named `test_*` and
  is `filter-out`'d of `TEST_SRC`, because it needs host input to terminate and
  would otherwise loop forever under `make check`/`check-disasm` and mismatch
  user-mode qemu (no UART) under `check-diff`.
- virtio-mmio block device (M18, the xv6 root-fs enabler): a modern (version 2)
  block device in `device.c` with one split virtqueue, on the qemu `virt` first
  slot (`VIRTIO_BASE` 0x10001000, PLIC source 1) — the addresses xv6 hardcodes, so
  no DTB node is needed for it (a `virtio` DTB node is deferred to the Linux work).
  It is the project's first **bus-master** device: unlike the register-only
  CLINT/PLIC/UART, it DMAs against guest RAM, so `Platform` carries a RAM pointer
  set by `plat_attach_ram` (from `start_at`) and `dma_ptr` does the bounds-checked
  guest-physical → host access (a malformed descriptor is ignored, never faulting
  the CPU — DMA stays off the `mem_*` fault path). The driver brings the device up
  through the mmio register file (status/feature handshake, queue addresses,
  `QUEUE_READY`) and kicks it with `QUEUE_NOTIFY`; `virtio_notify` then walks the
  available ring and services each chain **synchronously** — header (type+sector) →
  data descriptor(s) copied to/from the `--disk` image → a status byte → a used-ring
  completion → the PLIC interrupt raised (which the guest ACKs via `INT_ACK`,
  deasserting the line). Synchronous completion means the used index has already
  advanced when the notify store returns; it is safe for xv6, which holds
  `vdisk_lock` with interrupts off until it sleeps. We negotiate no features (bar
  advertising `VIRTIO_F_VERSION_1`) and never fail `FEATURES_OK`, so a driver that
  skips the VERSION_1 ack (xv6 does) still comes up. Only queue 0 exists; the ring
  size is capped at `V_QUEUE_MAX` (8, xv6's `NUM`). Inert until a guest programs it
  (`interrupt_status` 0 keeps it out of `plic_lines`), so every pre-M18 test is
  unaffected. `test_rv64_virtio.S` is built `-mno-relax`: it takes the address of
  its in-RAM queue with `la`, and linker relaxation would rewrite that into a
  gp-relative `addi rd, gp, off` — but the test framework uses `gp` (x3) as its
  check-id register, not `__global_pointer$`, so a relaxed address would be
  garbage (the same class of gotcha as the fixed-`+4` mepc handlers avoiding C).
- virtio-mmio network device (M23, the network-path enabler): a second modern
  (version 2) virtio device in `device.c`, on the **second** virtio slot
  (`VIRTIO_NET_BASE` 0x10002000, PLIC source 2), the qemu `virt` layout. Unlike the
  single-queue block device it has **two** virtqueues — queue 0 receive, queue 1
  transmit — so it honours `QUEUE_SEL` to route the num/ready/desc/avail/used
  writes and the `QUEUE_NOTIFY` value to the right queue (block ignored both,
  having one queue). The first commit ships a deterministic **loopback** backend:
  with no host backend attached (`net.tx == NULL`), a frame the guest transmits is
  fed straight back into the receive FIFO and delivered into the driver's posted
  receive buffers — so `test_rv64_virtio_net.S` drives it end to end with no host
  networking (`make check-virtnet`, quanta-only like the block test, `-mno-relax`
  for the same `la`/`gp` reason). Design points: it negotiates only
  `VIRTIO_F_VERSION_1` + `VIRTIO_NET_F_MAC`, so the virtio-net header is **12
  bytes** (v1, `num_buffers` at offset 10) and the driver reads the MAC
  (`52:54:00:12:34:56`, set in `plat_init`) from config space; the receive FIFO
  (`VIRTIO_NET_FIFO` frames of `VIRTIO_NET_FRAME_MAX` bytes) buffers frames until
  the guest posts buffers, and a soft reset (`net_reset`, status write 0) clears
  the programmable state but **preserves** the MAC, the backend, and the FIFO
  storage (host state a guest reset should not drop). It grew `Platform` by the
  FIFO, so snapshot files' layout signature changed (old files reject cleanly) —
  `check-snapshot`/`check-replay` stay green because the device is inert until
  programmed (`interrupt_status` 0 keeps it out of `plic_lines`). The `net.tx`
  function pointer is NULL on this path, so it round-trips through a snapshot
  harmlessly; a real backend will need it re-wired on restore (a commit-2 concern).
  A DTB `virtio,mmio` node is deferred to the Linux-networking step, as the block
  device's was — the bare-metal test hardcodes the address.
- Usermode network stack (M23, `src/netstack.c`, `--netdev=user`): the first host
  backend for virtio-net — a from-scratch, no-privilege virtual gateway on
  10.0.2.0/24 (the qemu-slirp layout: guest 10.0.2.15, gateway 10.0.2.2, DNS
  10.0.2.3, gateway MAC `52:55:0a:00:02:02`). It is a **pure ethernet-frame
  processor** with no CPU/platform dependency — `netstack_input` parses a guest
  frame and emits replies through a callback — so `tests/net_test.c` drives it
  standalone (ARP/ICMP/DHCP + checksums), the strongest net for the protocol code,
  while `tests/rv64/test_rv64_net.S` (an ARP round trip under `--netdev=user`,
  `make check-net`) proves the full device↔backend↔stack↔CPU path the loopback
  test cannot. Design points: (1) the virtual-gateway services are **synchronous**
  — a reply is produced during the guest's transmit notify (device →
  `net_backend_tx` → `netstack_input` → `net_deliver_to_guest` → `quanta_net_rx` →
  the receive FIFO), so **no main-loop pump is needed** for this commit (an async
  host-socket/TAP backend later will need one). (2) All packet parsing is
  bounds-checked against the frame length (guest-controlled input); the IP header
  length from the header is clamped to what the frame actually carries. (3) The IP
  and ICMP checksums are computed (RFC 1071); the UDP checksum is left 0 (legal on
  IPv4). (4) DHCP replies are broadcast (the client has no address yet), pad the
  BOOTP body to 300 bytes, and echo the request's xid. (5) The `main.c` bridge owns
  the `NetStack` and frees it on every exit path (gdb and normal), since
  `quanta_destroy` does not (it is external to the engine). Outbound UDP/TCP NAT to
  real host sockets, a DNS relay, a `virtio,mmio` DTB node (so Linux discovers the
  device), and a Linux TAP backend are the remaining M23 steps — the host-socket
  ones need a real guest to validate, so they are manual milestones like the OS
  boots, not deterministic checks.
- Booting an OS (M16, `tests/os/`): the teaching kernel is *entered in M-mode*
  (Quanta's loader enters every ELF in M-mode), and its `boot.S` does the
  drop-to-S itself — the same pattern `test_sbi`/`test_stimer` use — rather than
  Quanta entering it in S-mode. Two consequences a foreign kernel would trip on
  but this one is built around: (1) the kernel must leave `mtvec` 0, because the
  SBI is available only while no M-mode handler is installed (M15); its own SBI
  `ecall`s (cause 9, *not* in its `medeleg`) reach Quanta as firmware, while
  delegated U-mode `ecall`s (cause 8) and the supervisor timer (`mideleg` bit 5)
  go to its `stvec`. (2) The supervisor timer fires in U-mode regardless of
  `sstatus.SIE` (the running privilege is below S), and is masked inside the
  handler because trap entry clears `SIE` — so the kernel keeps interrupts off in
  S-mode and gets no nested traps without any extra masking. The kernel reads its
  user's `write` buffer directly through the user VAs by setting `sstatus.SUM`
  (S-mode access to U pages), so no software page-table walk is needed. RAM beyond
  the image comes from `--memory`; the free pool is `[_end, dtb)` page-aligned, so
  it never collides with the boot device tree Quanta parks at the top of RAM. It
  boots even without `--memory` (the 64 KiB stack headroom holds the four
  page-table/user pages), just with little spare RAM. `--trace` is the debugging
  tool when changing it — there is no qemu differential net for an S-mode paging
  guest.
- Booting xv6 (M18, the mainstream-OS trophy): upstream `mit-pdos/xv6-riscv`
  boots to an interactive shell on Quanta. It is entered in M-mode at
  `0x80000000` (like every ELF) and drops to S-mode itself in `start.c` — needing
  neither OpenSBI nor Quanta's SBI — so the enter-in-M-mode contract fits it.
  Build it integer-only (Quanta has no RV64F/D): override the upstream
  `-march=rv64gc` to `-march=rv64imac_zicsr_zifencei -mabi=lp64` and build
  `CPUS=1` (Quanta is single-hart). Run `./quanta --memory=128M --max-steps=0
  --disk=fs.img kernel/kernel` — `128M` matches xv6's hardcoded `PHYSTOP`
  (`0x88000000`), `--max-steps=0` lifts the runaway guard (xv6 `memset`s all
  128 MiB in `kinit` alone, ~billions of instructions), and `--disk` supplies the
  virtio root fs. The four Quanta pieces this needed beyond the M18 devices: the
  full-XLEN branch fix (above), the PLIC S-mode context/SEIP (xv6 routes device
  interrupts to S-mode), the UART THRE one-shot (xv6 leaves TX interrupts on —
  a level-asserting THRE would storm), and `--max-steps`. No `make` target boots
  xv6 (its source is external and runs are long); it is a manual milestone,
  pinned indirectly by `test_rv64_plic.S` (S-mode interrupts) and the branch
  checks. When debugging an OS boot, shrink `PHYSTOP` (e.g. to 8 MiB) so `kinit`
  is fast, and patch a guest `printk` into the failing kernel path — cheaper than
  a multi-hundred-million-instruction `--trace`.
- Booting under OpenSBI firmware (M18, the road to Linux): unlike xv6 (which owns
  M-mode itself via `-bios none`), the mainstream path runs a real M-mode firmware
  that hands off to an S-mode OS. `quanta --bios=FILE --kernel=FILE` does this
  (`quanta_load_firmware` in quanta.c): it loads the firmware ELF (OpenSBI's
  **fw_dynamic** build — qemu ships one at `/usr/share/qemu/opensbi-riscv64-
  generic-fw_dynamic.elf`) at 0x80000000 and the raw OS image at **0x80200000**
  (`base + 2 MiB`, the qemu `virt` kernel address a Linux `Image` also expects),
  and enters the firmware in M-mode with `a0`=hartid, `a1`=DTB, and `a2` = a
  **`fw_dynamic_info`** descriptor (magic `0x4942534f` "OSBI", version 2,
  `next_addr`=the OS, `next_mode`=1=S). OpenSBI reads that struct *immediately* at
  entry (`ld a0,0(a2)` in fw_base.S) — pass `a2`=0 and it faults reading null, so
  the descriptor is mandatory, not optional. Two placement rules `setup_firmware_boot`
  encodes: **(1)** the DTB is parked 2 MiB below the top of RAM (`FW_DTB_HEADROOM`),
  not jammed against it, because OpenSBI expands the FDT *in place* (`fdt_open_into`,
  a backward `memmove`) to add its own reserved-memory/PMP nodes — a top-of-RAM
  DTB overflows the last page (the first bug hit, `mtval` one past `mem_size`).
  **(2)** the `fw_dynamic_info` sits just below the DTB, read once at entry. With
  those, upstream **OpenSBI v1.3 boots on Quanta** — full platform init from our
  DTB (`uart8250`, `aclint-mtimer`, `aclint-mswi`, 64 PMP entries), then it drops
  to S-mode at 0x80200000. Quanta's own `sbi.c` is **bypassed** here (it only runs
  the SEE while `mtvec` is 0; OpenSBI sets `mtvec`), so OpenSBI *is* the SBI the
  S-mode OS calls — Quanta just has to be a correct M-mode machine, which it
  already was (no CSR/instruction gaps surfaced). `tests/opensbi_payload.S` +
  `make check-opensbi` pin the round trip (S-mode → SBI console → SBI SRST →
  clean exit).
- Booting Linux (M18, the mainstream-distro trophy): a mainline **Linux 6.6**
  `Image` boots on Quanta through OpenSBI **to an interactive userspace shell** —
  `Machine model: quanta,virt`, SBI up, `earlycon` on the UART, Sv39 paging,
  memory zones, `Unpacking initramfs`, `Run /init as init process`, a `quanta$`
  prompt, and a clean power-down. Build it rv64imac (Quanta has no F/D/V): from a
  defconfig kernel, `scripts/config -d FPU -d RISCV_ISA_V -d RISCV_ISA_ZICBOM
  -d RISCV_ISA_ZICBOZ` then `olddefconfig` (our DTB advertises only
  `rv64imac_zicsr`, so the kernel's runtime probes stay off those). Build with a
  **linux-gnu** toolchain (`CROSS_COMPILE=riscv64-linux-gnu-`), not the bare-metal
  newlib one — the newlib linker cannot `-shared` the kernel's VDSO. Run `quanta
  --bios=<opensbi-fw_dynamic> --kernel=Image --initrd=<cpio> --memory=128M
  --max-steps=0 --append="earlycon=uart8250,mmio,0x10000000 console=ttyS0"`. The
  `--append` flag sets the kernel command line via the DTB `/chosen` bootargs (see
  dtb.c's `bootargs`). No `make` target boots it (external kernel + firmware, long
  run), like xv6. Two Quanta changes it needed beyond the OpenSBI path: the **JALR
  far-call fix** below, and the **initramfs support** next — everything else
  (Sv39, SBI-via-OpenSBI, the devices) already worked.
- Linux initramfs / the `--initrd` flag (M18, the Linux-userspace enabler): Linux
  needs a root filesystem to run `/init`; `--initrd=FILE` supplies one the way
  qemu's `-initrd` does. `setup_firmware_boot` (quanta.c) parks the cpio blob
  **page-aligned just below the fw_dynamic info/DTB** — high in RAM, well clear of
  the kernel image at 0x80200000 — and `dtb.c` advertises its physical bounds in
  `/chosen` as `linux,initrd-start`/`-end` (two big-endian cells each, a zero-high
  u64; the kernel reads whatever cell count the property length implies and
  memblock-reserves the range early, so it survives boot). Only emitted when an
  initrd is present, so every non-initrd boot is byte-identical. The userspace
  lives in `tests/linux/`: `init.c` is a freestanding RV64 `/init` (no libc, raw
  Linux `ecall` syscalls — write/read/reboot; a tiny line shell that powers off
  via the reboot syscall, which Linux turns into an SBI SRST → the SiFive test
  device → `HALT_EXIT`), built static/non-PIE/`rv64imac`; `mkcpio.c` is a
  self-contained newc-cpio packer (a host program — no `cpio` tool and no root,
  which matters because the archive must carry a **`/dev/console` character-device
  node** (major 5, minor 1) that the kernel opens as PID 1's stdin/stdout, and an
  unprivileged `cpio` cannot `mknod` one). `make linux-initramfs` (→
  `build/linux/initramfs.cpio`) and `tests/linux/README.md` cover the build/boot;
  a few input bytes piped in before the console is up can race boot (a real user
  types after the prompt), but the interactive path is clean.
- SMP multi-hart (M19, `--harts=N`): the machine models up to `QUANTA_MAX_HARTS`
  (8) harts sharing one `Memory` and one `Platform`. Concurrency is **a single
  host thread, round-robin** — the engine's `quanta_run`/`quanta_step` scheduler
  steps `harts[0..nharts)` one instruction each in turn — so runs stay
  deterministic (no host threads, no wall-clock races) while the guest still sees
  real interleaving. Design points a future change must preserve: **(1)** the
  shared `mtime` ticks **once per scheduler round** (when the round-robin cursor is
  back at hart 0), not once per hart-step, so time advances at one rate whatever
  the hart count — `plat_tick` moved out of `cpu_step` into the scheduler for this.
  **(2)** Everything per-hart keys off `cpu->hartid`: the CLINT has `msip[h]`/
  `mtimecmp[h]` arrays (a hart IPIs another by writing its `msip`), the PLIC has
  contexts `2h`(M)/`2h+1`(S) driving that hart's MEIP/SEIP, and `plat_mip_bits(p,
  hart)` / the trap path read them by id; `mhartid` and boot-time `a0` carry the
  id. **(3)** Cross-hart LR/SC: a store on any hart voids every *other* hart's
  reservation to that word — `break_reservation` sweeps `plat->harts` (the engine
  points the platform at its hart array via `plat_set_harts`). A successful **`sc`
  must break siblings too** (it was the one store path that didn't — a real bug the
  SMP test caught). **(4)** The machine stops on a **global** power-off (SiFive
  test / SBI SRST via the platform) or when **every** hart has halted
  (`machine_poll_halt`); an abnormal hart reason is surfaced ahead of a clean exit
  so a secondary crash is not masked. **(5)** Boot: the direct ELF/image path
  (`setup_boot`) brings up all harts at the entry with `a0`=hartid (the qemu
  `-bios none` convention — xv6 dispatches on it); the firmware `--bios` path parks
  the secondaries (`halted=1`), because SMP under OpenSBI/Linux needs SBI HSM
  `hart_start` and all harts entering the firmware, which is **future work**. The
  DTB emits one `cpu@h` node per hart (phandle `PHANDLE_CPU_INTC(h)` = `16+h`) and
  wires every hart into the CLINT/PLIC `interrupts-extended`; `DTB_MAX_SIZE` was
  bumped to 4 KiB to hold 8 cpu nodes. `nharts==1` is byte-identical to the
  pre-M19 uniprocessor (verified across the whole suite). Pinned by
  `test_rv64_smp.S`/`make check-smp`, and xv6 boots `--harts=3` (a manual trophy,
  like the single-hart xv6/Linux boots — the same `kernel/kernel` binary, since
  xv6's `CPUS` is only a runtime qemu `-smp` flag and `NCPU`=8).
- Snapshot/restore (E10, `quanta_snapshot`/`quanta_restore` in `quanta.c`): the
  whole mutable machine is a fixed-size struct (the `harts` array — registers,
  CSRs, TLB, PC — plus the inline `Platform` device register files and the
  scheduler bookkeeping) and two heap buffers (guest RAM and the optional
  in-memory `--disk`). So a snapshot is a value copy of the inline state plus a
  deep copy of the two buffers; a restore reverses that and **re-wires the borrowed
  pointers** — each hart's `mem`/`cache`, the memory's `plat` back-pointer, and the
  platform's `ram`/`harts`/`disk.data` — to the live objects rather than the stale
  pointer values the value copy carried. Restore is **same-machine only** (it
  restores into the live RAM/disk buffers in place, so their sizes must match — it
  returns `QUANTA_ERR_INVAL` otherwise); it does not reallocate. The
  observability-only **cache is deliberately not captured** — it never changes
  results, so a restored run stays bit-identical without it. What makes the replay
  exact is the M19 determinism: `mtime` ticks once per scheduler round and there is
  no host-thread/wall-clock nondeterminism, so a machine restored to a point
  re-executes the identical instruction stream (given the same later console
  input). `tests/snapshot_test.c` pins it by replaying a guest's tail from a
  mid-run snapshot; the GDB reverse-step below builds on it.
- Snapshot *file* serialisation (E10, `--snapshot`/`--restore`, `quanta_save_
  snapshot`/`quanta_load_snapshot` in `quanta.c`): a snapshot writes to a
  self-describing file — a 72-byte little-endian header, then the **raw bytes** of
  the inline state (the harts array and the `Platform`), then guest RAM and the
  disk. Field-by-field serialisation would be enormous (the CSR file alone is 4096
  entries per hart) and no more correct on the same build, so the raw-struct dump
  is the pragmatic choice; the cost is that a file is **host-ABI specific**, so the
  header records `sizeof(CPU)`/`sizeof(Platform)`/`QUANTA_MAX_HARTS` and a load
  **rejects** any file whose signature does not match the running binary (clean
  fail, never a mis-read). The pointer members on disk are meaningless and
  `quanta_restore` re-wires them. `--restore` is **self-contained** — it sizes and
  fills RAM and the disk from the file (via `mem_init` + a disk `malloc`, then
  `quanta_restore`), so no ELF/kernel is given alongside it, and it is standalone
  (`main.c` rejects combining it with a program/`--bios`). `--snapshot` runs at the
  end of a run whatever the halt reason, so `--max-steps=N --snapshot=FILE` is the
  checkpoint idiom. The cache is not serialised (observability only). Pinned by
  `tests/check_replay.sh` / `make check-replay`.
- Reverse debugging in the GDB stub (E10, `gdbstub.c`): a stock `gdb` reverse-steps
  (`bs`) and reverse-continues (`bc`) a guest, advertised via `ReverseStep+`/
  `ReverseContinue+` in `qSupported`. The mechanism is a **monotonic step count**
  (`nsteps`, a stable time coordinate because the run is deterministic and a gdb
  session feeds no external input) plus a **ring of machine snapshots**: to reach an
  earlier step, `goto_step` restores the nearest checkpoint at or before it and
  replays forward (`raw_step`); `bs` targets `nsteps-1`, `bc` replays from the start
  tracking the last step whose PC is a breakpoint. Checkpointing is **lazy** — it
  only kicks in after the first reverse op (`reverse_used`), so plain forward-only
  sessions (and the pre-E10 behaviour) pay nothing; `cp[0]` is pinned to the attach
  point (step 0) and the rest roll, evicting the oldest so replays stay short near
  the current position. Two caveats: (1) reverse debugging replays the deterministic
  instruction stream, so manual `P`/`M`/`G` pokes are *not* part of history and are
  undone by a rewind; (2) each checkpoint is a full-machine copy, so the memory/
  latency cost scales with guest RAM (fine for the tests; heavy but inherent for a
  128 MiB Linux). Pinned by `tests/gdb_client.py` / `make check-gdb`.
- The JALR base/link ordering (the Linux-boot fix, and a real correctness bug):
  `exec`'s `OP_JALR` must compute the target from `rs1` **before** writing the
  link register `rd = pc + ilen`, because `rd` can alias `rs1`. The `call`
  pseudo-op a linker emits for a target beyond a single `jal`'s reach is
  `auipc ra,hi; jalr ra,lo(ra)` — `rd == rs1 == ra` — so writing the link first
  clobbers the base and jumps to `pc + ilen + imm` (garbage). It is invisible to
  small binaries: the linker relaxes every in-range `call` to a bare `jal`, so a
  `rd==rs1` JALR only appears for calls spanning more than ~2 MiB. That is why the
  conformance suite, OpenSBI (322 KiB), and xv6 all passed while a 22 MiB Linux
  kernel's cross-section calls (e.g. `_start_kernel` → `setup_vm`) jumped to
  garbage — so `setup_vm`/`set_satp_mode` silently never ran, paging stayed off,
  and the kernel parked before printing a byte. Pinned by an `rd==rs1` case in
  `tests/test_jumps.S` (fails on the old code; qemu-checked under `check-diff`).
- SiFive test finisher device (M18, the clean-shutdown enabler): OpenSBI's SRST
  and Linux's poweroff/reboot need a hardware reset device. `device.c` models the
  qemu `virt` one at **0x100000** (`TEST_BASE`): a 32-bit write of `0x5555`
  (PASS), `0x3333`+code (FAIL), or `0x7777` (RESET, treated as PASS since we can't
  reboot) requests power-off. A device can't stop the hart itself, so it sets
  `Platform.poweroff`/`poweroff_code` and the CPU polls `plat_poweroff_requested`
  at the top of each step (right after `plat_tick`), halting with `HALT_EXIT` and
  the code. It is advertised in the DTB as a `test@100000` node with
  `compatible = "sifive,test1\0sifive,test0"`, which OpenSBI's fdt reset driver
  binds to. Inert for every pre-M18 guest (none write 0x100000, which was
  previously just unmapped low memory).
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
  uninitialised read. It is one of the two POSIX corners: `gdbstub.c` defines
  `_DEFAULT_SOURCE` itself (so `make analyze`'s clang-tidy, which compiles with a
  bare `-std=c11`, still sees the socket decls) under a local NOLINT for the
  reserved-identifier check — the same pattern `main.c` uses for its `select`/
  `read` console input. `--gdb` takes over execution, so it does not combine
  with `--trace`/`--pipeline`; `make check-gdb` drives it with a pure-python RSP
  client (`tests/gdb_client.py`, no riscv `gdb` needed) and it also runs under
  `make sanitize`/`make coverage`.

## .claude/

- `settings.json` — pre-approves build/run/test and common git commands
  (including `push`, so a manual push isn't gated; pre-approval does not make
  it run automatically).
- `commands/commit.md` — `/commit` writes a Conventional Commits message and
  commits locally (never pushes).
