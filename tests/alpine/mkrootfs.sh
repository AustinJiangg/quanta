#!/bin/sh
# Build a bootable Alpine Linux RV64 root filesystem image for Quanta (M24).
#
# Downloads the official Alpine riscv64 *minirootfs* tarball, tweaks it for a
# headless serial-console boot (a getty on ttyS0, a known root password), embeds a
# small self-validating init script, and packs it all into a writable ext4 image
# the virtio-mmio block device serves as /dev/vda.
#
# No root needed: fakeroot records uid 0 ownership and the device nodes so the
# image looks like a real system, and mke2fs(1) 1.43+ populates the filesystem
# from a directory (-d). Extraction and mke2fs must share one fakeroot session
# (the faked-ownership state is per-process), so they run under a single heredoc.
# The kernel Image and OpenSBI firmware are external artifacts supplied at boot
# time (see README.md) — this only builds the rootfs.
#
# Usage: tests/alpine/mkrootfs.sh [OUTPUT.ext4] [SIZE]
#   OUTPUT  defaults to build/alpine/alpine.ext4
#   SIZE    ext4 image size, defaults to 256M
set -eu

MIRROR=${ALPINE_MIRROR:-https://dl-cdn.alpinelinux.org/alpine}
BRANCH=${ALPINE_BRANCH:-edge}
ARCH=riscv64
OUT=${1:-build/alpine/alpine.ext4}
SIZE=${2:-256M}
WORK=$(dirname "$OUT")

# md5-crypt of the password "root" (fixed salt, reproducible; md5 is the crypt
# format every BusyBox build supports). The demo image ships a real, known
# password rather than an unlocked-but-empty root, which BusyBox login refuses on
# a serial line. (The guaranteed-interactive path is init=/bin/sh — see README.)
ROOT_HASH='$1$quantaal$Oq4Cp1A9dXwICaCFKb82v0'

for t in curl fakeroot mke2fs; do
    command -v "$t" >/dev/null 2>&1 || { echo "mkrootfs: need '$t' on PATH"; exit 1; }
done
mkdir -p "$WORK"

# 1. Fetch the latest minirootfs tarball for the branch (cached under $WORK).
listing=$(curl -fsSL "$MIRROR/$BRANCH/releases/$ARCH/" 2>/dev/null) \
    || { echo "mkrootfs: cannot reach $MIRROR/$BRANCH/releases/$ARCH/"; exit 1; }
TAR=$(printf '%s\n' "$listing" \
    | grep -oE "alpine-minirootfs-[0-9]+-$ARCH\.tar\.gz" | sort -u | tail -1)
[ -n "$TAR" ] || { echo "mkrootfs: no minirootfs found in listing"; exit 1; }
if [ ! -f "$WORK/$TAR" ]; then
    echo "mkrootfs: downloading $TAR"
    curl -fsSL -o "$WORK/$TAR" "$MIRROR/$BRANCH/releases/$ARCH/$TAR"
fi

# 2. A deterministic init (boot with init=/qtest.sh) that proves the writable
#    disk: it prints the release, reads any marker a previous boot left, writes a
#    fresh one, syncs (flushing the write-through file), and powers off. Two runs
#    on the same image show the marker surviving a reboot.
cat > "$WORK/qtest.sh" <<'QT'
#!/bin/sh
echo "===QUANTA-ALPINE-BEGIN==="
cat /etc/alpine-release
uname -srm
echo "prev-marker: $(cat /marker 2>/dev/null || echo NONE)"
echo "quanta-run-marker-persisted" > /marker
sync
echo "wrote-marker: $(cat /marker)"
echo "===QUANTA-ALPINE-END==="
poweroff -f
QT

# 3. Extract, tweak, and pack in ONE fakeroot session (shared ownership state).
#    Args are passed positionally so the quoted heredoc does no expansion itself.
fakeroot sh -s "$WORK" "$TAR" "$OUT" "$SIZE" "$ROOT_HASH" <<'FAKED'
set -eu
work=$1; tar=$2; out=$3; size=$4; roothash=$5
root="$work/root"
rm -rf "$root"; mkdir -p "$root"
tar -xzf "$work/$tar" -C "$root"
# Serial console: enable the ttyS0 getty (shipped commented out) and drop its
# carrier wait ("-L 0"); set a known root password ("root") for the getty login.
sed -i 's|^#ttyS0::respawn:/sbin/getty -L 115200 ttyS0 vt100|ttyS0::respawn:/sbin/getty -L 0 ttyS0 vt100|' "$root/etc/inittab"
sed -i "s|^root:[^:]*:|root:$roothash:|" "$root/etc/shadow"
echo quanta-alpine > "$root/etc/hostname"
cp "$work/qtest.sh" "$root/qtest.sh"; chmod +x "$root/qtest.sh"
truncate -s "$size" "$out"
mke2fs -q -F -t ext4 -d "$root" -L alpineroot "$out"
rm -rf "$root"
FAKED

echo "mkrootfs: built $OUT ($SIZE, from $TAR)"
