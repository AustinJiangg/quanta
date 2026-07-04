# Booting Linux to a userspace shell (M18)

A mainline **Linux 6.6** kernel boots on Quanta — through real OpenSBI, over the
emulated `virt` platform, on Sv39 paging — and runs a real userspace process as
PID 1. This directory holds that userspace: a hand-written `init` and the tools
to pack it into the initramfs Quanta hands the kernel.

```
OpenSBI (M-mode firmware)  ->  Linux 6.6 (S-mode)  ->  /init (U-mode, PID 1)
        --bios                     --kernel                --initrd
```

## What's here

- **`init.c`** — the userspace `/init`. Freestanding: no C library, every action
  is a raw RISC-V Linux syscall (`ecall`). Built static, non-PIE, `rv64imac`
  (Quanta has no RV64F/D). It runs a tiny line-oriented shell over the serial
  console (`help`, `echo <text>`, `poweroff`) and powers the machine off — via the
  reboot syscall, which Linux turns into an SBI SRST call — on `poweroff` or a
  console EOF.
- **`mkcpio.c`** — a minimal initramfs builder. Writes the kernel's "newc" cpio
  format directly, so the whole path builds with just a C compiler: no `cpio`
  tool and no root, which matters because the archive must contain a
  `/dev/console` *device node* (the kernel opens it as PID 1's stdin/stdout) that
  an unprivileged `cpio` cannot create.
- **`mkinitramfs.sh`** — builds `init` (cross compiler) and `mkcpio` (host
  compiler) and packs `build/linux/initramfs.cpio`.

## Build the initramfs

```sh
tests/linux/mkinitramfs.sh          # -> build/linux/initramfs.cpio
```

Needs the `riscv64-linux-gnu-` cross compiler (override with `CROSS_COMPILE=`) and
a host `cc`. This is all Quanta's tree can build on its own.

## Supply a kernel + firmware and boot

The kernel `Image` and OpenSBI firmware are external artifacts (large, and built
from other source trees), so there is no `make` target — this is a manual
milestone, like the xv6 boot.

- **OpenSBI**: qemu ships one at
  `/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.elf` (the fw_dynamic build).
- **Linux Image**: build mainline 6.6 integer-only (Quanta has no F/D/V). From a
  `defconfig` tree, disable the FPU and the ISA extensions we don't advertise,
  then build with a **linux-gnu** toolchain (the bare-metal newlib linker cannot
  `-shared` the kernel's VDSO):

  ```sh
  make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- defconfig
  scripts/config -d FPU -d RISCV_ISA_V -d RISCV_ISA_ZICBOM -d RISCV_ISA_ZICBOZ
  make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- olddefconfig
  make ARCH=riscv CROSS_COMPILE=riscv64-linux-gnu- -j$(nproc) Image
  ```

Then boot:

```sh
quanta --bios=/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.elf \
       --kernel=/path/to/arch/riscv/boot/Image \
       --initrd=build/linux/initramfs.cpio \
       --memory=128M --max-steps=0 \
       --append="earlycon=uart8250,mmio,0x10000000 console=ttyS0"
```

`--initrd` stages the cpio in RAM below the device tree and points the kernel at
it via `/chosen` `linux,initrd-start`/`-end`; `--max-steps=0` lifts the runaway
guard (a full boot is hundreds of millions of instructions). You reach:

```
[    9.848] Run /init as init process

[ quanta ] userspace reached: PID 1 running, no libc, raw syscalls.
commands: help, echo <text>, poweroff (or Ctrl-A x to quit Quanta)
quanta$ echo hello from quanta userspace
hello from quanta userspace
quanta$ poweroff
[ quanta ] powering off.
[   11.207] reboot: Power down
```

The console is interactive: type at the `quanta$` prompt and the kernel's tty
line discipline echoes it, just like a real serial console. `Ctrl-A x` quits
Quanta itself.
