#!/bin/sh
# virtio-mmio block device (M18): drive the modern (v2) block device end to end.
#
# The guest (tests/rv64/test_rv64_virtio.S) brings the device up through the
# status/feature handshake, sets up a split virtqueue, and issues block requests:
# it reads sector 0 (checking a magic the harness wrote into the disk image),
# then writes a pattern to another sector and reads it back (proving DMA in both
# directions), and confirms the device raised its PLIC interrupt. A clean exit 0
# means every one of its CHECKs passed; a non-zero code is the failing check id.
#
# Machine-mode + MMIO + virtio, so it is quanta-only (out of the qemu-riscv64
# differential) and runs here with its own --disk image. Needs the cross-toolchain
# to build the ELF, like the other device checks.
set -u

QUANTA=./quanta
ELF=tests/rv64/test_rv64_virtio.elf

if [ ! -x "$QUANTA" ]; then
    echo "check-virtio: build quanta first (run 'make')"
    exit 1
fi
if [ ! -f "$ELF" ]; then
    echo "check-virtio: $ELF missing (run 'make tests')"
    exit 1
fi

# A small backing image: sector 0 starts with the little-endian magic 0x12345678
# (bytes 78 56 34 12) the guest verifies; the rest is zero. The guest writes the
# pattern 0x5a5a5a5a to sector 3 (byte offset 3*512 = 1536).
make_image() {
    dd if=/dev/zero of="$1" bs=1024 count=8 2>/dev/null
    printf '\170\126\064\022' | dd of="$1" bs=1 conv=notrunc 2>/dev/null
}
# The four bytes at sector 3, as space-free hex (5a5a5a5a if the write landed).
sector3() { dd if="$1" bs=1 skip=1536 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n'; }

# 1. Writable --disk: the guest's sector-3 write must persist through to the file.
diskw=$(mktemp) || { echo "check-virtio: mktemp failed"; exit 1; }
make_image "$diskw"
"$QUANTA" --quiet --disk="$diskw" "$ELF" </dev/null >/dev/null 2>&1
rc=$?
got=$(sector3 "$diskw")
rm -f "$diskw"
if [ "$rc" -ne 0 ]; then
    echo "check-virtio: FAILED — $ELF exited $rc (virtio check $rc failed)"
    exit 1
fi
if [ "$got" != "5a5a5a5a" ]; then
    echo "check-virtio: FAILED — write not persisted to --disk (sector 3 = '$got')"
    exit 1
fi

# 2. Read-only --disk-ro: the guest still round-trips the write through RAM (exit 0),
# but the backing file stays untouched (the write is a discard overlay).
diskr=$(mktemp) || { echo "check-virtio: mktemp failed"; exit 1; }
make_image "$diskr"
"$QUANTA" --quiet --disk-ro="$diskr" "$ELF" </dev/null >/dev/null 2>&1
rc=$?
got=$(sector3 "$diskr")
rm -f "$diskr"
if [ "$rc" -ne 0 ]; then
    echo "check-virtio: FAILED — $ELF exited $rc under --disk-ro (check $rc)"
    exit 1
fi
if [ "$got" != "00000000" ]; then
    echo "check-virtio: FAILED — --disk-ro persisted a write (sector 3 = '$got')"
    exit 1
fi

echo "OK    virtio-mmio block device: identity, queue setup, read/write DMA"
echo "OK    disk magic read back, sector round-trip, and interrupt asserted"
echo "OK    --disk write-through persists; --disk-ro leaves the image untouched"
echo "virtio block device behaves as expected"
