#!/bin/sh
# virtio-net device (M23): drive the modern (v2) network device end to end.
#
# The guest (tests/rv64/test_rv64_virtio_net.S) brings the device up through the
# status/feature handshake, reads the MAC, sets up a receive and a transmit
# virtqueue, posts a receive buffer, and transmits a frame — which the device's
# internal loopback backend delivers straight back into the posted receive buffer.
# It then checks both used rings advanced, the received length and payload match,
# and the device raised its PLIC interrupt. A clean exit 0 means every CHECK
# passed; a non-zero code is the failing check id.
#
# Machine-mode + MMIO + virtio, so it is quanta-only (out of the qemu-riscv64
# differential). The loopback backend needs no host networking, so no extra flags
# are required. Needs the cross-toolchain to build the ELF, like the other device
# checks.
set -u

QUANTA=./quanta
ELF=tests/rv64/test_rv64_virtio_net.elf

if [ ! -x "$QUANTA" ]; then
    echo "check-virtnet: build quanta first (run 'make')"
    exit 1
fi
if [ ! -f "$ELF" ]; then
    echo "check-virtnet: $ELF missing (run 'make tests')"
    exit 1
fi

"$QUANTA" --quiet "$ELF" </dev/null >/dev/null 2>&1
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "check-virtnet: FAILED — $ELF exited $rc (virtio-net check $rc failed)"
    exit 1
fi

echo "OK    virtio-net device: identity, feature handshake, MAC, queue setup"
echo "OK    transmit -> loopback -> receive DMA, and interrupt asserted"
echo "virtio-net device behaves as expected"
