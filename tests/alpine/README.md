# Booting Alpine Linux on Quanta (M24)

The M24 trophy: a **stock RISC-V distribution booting from a writable virtio
disk** to a login shell, running its own unmodified binaries, and **persisting
changes across a reboot** — the payoff of the writable virtio-blk backend and the
block device's boot-DTB node.

Upstream **Alpine Linux (edge, riscv64)** boots on Quanta through OpenSBI: the
kernel finds `/dev/vda` (Quanta's virtio-mmio block device, discovered from the
device tree), mounts its **ext4 root read-write**, runs BusyBox init to a getty
login on `ttyS0`, and a file written in one boot is still there in the next — the
guest's writes are written through to the `--disk` image on the host.

Like the xv6 and Linux-initramfs boots, this is a **manual milestone**: the
kernel `Image` and the OpenSBI firmware are external artifacts, and a full boot
runs billions of instructions, so there is no `make check` target. `mkrootfs.sh`
builds the root filesystem reproducibly; the boot recipe is below.

## What you need

- **OpenSBI** `fw_dynamic` firmware (qemu ships one at
  `/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.elf`).
- A **Linux kernel `Image`** built `rv64imac` (Quanta has no F/D in the boot DTB
  isa string) with virtio-blk and ext4 built in — the same kernel the M18 Linux
  boot uses is fine; it just needs `CONFIG_VIRTIO_BLK=y`, `CONFIG_VIRTIO_MMIO=y`,
  `CONFIG_EXT4_FS=y`, and `CONFIG_DEVTMPFS_MOUNT=y` (a defconfig has all four).
- The build host needs `curl`, `fakeroot`, and `mke2fs` (e2fsprogs ≥ 1.43, for
  the `-d` populate-from-directory flag). No root.

## Build the root filesystem

```sh
make alpine-rootfs                      # -> build/alpine/alpine.ext4 (256 MiB)
# or directly, with an explicit path/size:
tests/alpine/mkrootfs.sh build/alpine/alpine.ext4 256M
```

`mkrootfs.sh` downloads the latest Alpine riscv64 minirootfs, sets a known root
password (`root`), enables a getty on `ttyS0`, drops in a self-validating init
(`/qtest.sh`), and packs an ext4 image under `fakeroot` so the tree is owned by
`uid 0`. (Set `ALPINE_BRANCH=v3.21` etc. to pin a stable release instead of
edge.)

## Boot it

Deterministic self-test (`init=/qtest.sh` prints the release and a persistence
marker, then powers off — no interaction, exits 0):

```sh
OPENSBI=/usr/share/qemu/opensbi-riscv64-generic-fw_dynamic.elf
KERNEL=path/to/arch/riscv/boot/Image
./quanta --bios="$OPENSBI" --kernel="$KERNEL" --disk=build/alpine/alpine.ext4 \
    --memory=256M --max-steps=0 \
    --append="root=/dev/vda rw console=ttyS0 init=/qtest.sh"
```

Run it **twice on the same image**: the first boot prints `prev-marker: NONE`,
the second prints `prev-marker: quanta-run-marker-persisted` — the write from the
first boot survived the poweroff, through the write-through to `alpine.ext4`.
(Use `--disk-ro=` instead and the marker never persists — the image is left
byte-for-byte untouched.)

Interactive root shell — the most reliable way in is `init=/bin/sh`, a BusyBox
root shell straight on the console (the ext4 root is already mounted rw, so you
can edit files, install nothing, and `poweroff -f` to leave):

```sh
./quanta --bios="$OPENSBI" --kernel="$KERNEL" --disk=build/alpine/alpine.ext4 \
    --memory=256M --max-steps=0 \
    --append="root=/dev/vda rw console=ttyS0 init=/bin/sh"
```

Full init to a getty login — drop the `init=` and let BusyBox init bring the
system up to a `login:` prompt on `ttyS0`; log in as `root` with password `root`:

```sh
./quanta --bios="$OPENSBI" --kernel="$KERNEL" --disk=build/alpine/alpine.ext4 \
    --memory=256M --max-steps=0 --append="root=/dev/vda rw console=ttyS0"
```

## Notes

- **rv64imac kernel, rv64gc userland.** Alpine's binaries are hard-float
  (`rv64gc`), but the kernel is built integer-only. Quanta executes float
  instructions regardless of `mstatus.FS` (M20's permissive FP), so stock binaries
  run; the caveat is that an FPU-less kernel does not save/restore `f0`–`f31`
  across a context switch, so concurrent float-heavy processes could interfere.
  For a login shell and ordinary commands this is not observed. A kernel built
  with `CONFIG_FPU=y` avoids it entirely (and needs the boot DTB to advertise
  `fd`).
- **No OpenRC.** The minirootfs ships an inittab that calls `/sbin/openrc`, which
  it does not include, so those sysinit lines fail harmlessly; BusyBox init still
  reaches the getty. Installing a fuller userland (`apk add openrc`, etc.) needs
  networking — see `--netdev=user` (M23).
- **Driving it.** For a hands-off run use the `init=/qtest.sh` path above (it
  needs no input and exits 0). For interaction, `init=/bin/sh` is the most robust —
  type at the shell once boot settles (~10 s guest time). The full-init getty
  `login:` is reached too, but authenticating over the modeled 16550 serial line
  can be finicky; the `init=/bin/sh` root shell avoids the login layer entirely.
  Piped stdin can race the boot, so type (or delay your input) until the prompt
  appears.
