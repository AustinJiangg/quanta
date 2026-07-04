#!/bin/sh
# mkinitramfs.sh — build the M18 Linux initramfs: compile the freestanding
# userspace /init and pack it (plus a /dev/console node) into a newc cpio the
# kernel can unpack. Self-contained: needs only a host C compiler and the
# riscv64-linux-gnu cross compiler — no cpio tool, no root, no kernel tree.
#
# Usage: tests/linux/mkinitramfs.sh [output.cpio]
#   default output: build/linux/initramfs.cpio
#
# Boot it with (OpenSBI + a rv64imac Linux Image supplied separately):
#   quanta --bios=<opensbi-fw_dynamic.elf> --kernel=<Image> \
#          --initrd=build/linux/initramfs.cpio --memory=128M --max-steps=0 \
#          --append="earlycon=uart8250,mmio,0x10000000 console=ttyS0"
set -eu

here=$(dirname "$0")
root=$(cd "$here/../.." && pwd)
outdir="$root/build/linux"
out=${1:-"$outdir/initramfs.cpio"}

CROSS=${CROSS_COMPILE:-riscv64-linux-gnu-}
HOSTCC=${CC:-cc}

mkdir -p "$outdir"

# The kernel has no RV64F/D (Quanta models integer-only): build rv64imac, static,
# non-PIE (ET_EXEC, no runtime loader), no startfiles (raw _start), no build-id.
"${CROSS}gcc" -march=rv64imac -mabi=lp64 -static -no-pie -nostdlib \
    -ffreestanding -fno-stack-protector -Os -Wl,--build-id=none \
    -o "$outdir/init" "$here/init.c"

# The packer is a host program (writes the newc cpio format directly).
"$HOSTCC" -std=c11 -Wall -Wextra -O2 -o "$outdir/mkcpio" "$here/mkcpio.c"

"$outdir/mkcpio" "$outdir/init" > "$out"
echo "initramfs: $out ($(wc -c < "$out") bytes)"
