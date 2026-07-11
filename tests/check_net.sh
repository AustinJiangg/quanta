#!/bin/sh
# Usermode network stack (M23): drive the virtio-net device against the stack.
#
# The guest (tests/rv64/test_rv64_net.S) brings the virtio-net device up, posts a
# receive buffer, and transmits an ARP request for the gateway (10.0.2.2). With
# --netdev=user the CLI attaches the usermode network stack as the device's
# backend, so the request flows device -> stack -> back into the receive buffer,
# and the guest checks the ARP reply (from the gateway MAC, for 10.0.2.2). A clean
# exit 0 means every CHECK passed; a non-zero code is the failing check id.
#
# Machine-mode + MMIO + virtio, so it is quanta-only. Needs no host networking
# (the stack is self-contained). Needs the cross-toolchain to build the ELF.
set -u

QUANTA=./quanta
ELF=tests/rv64/test_rv64_net.elf

if [ ! -x "$QUANTA" ]; then
    echo "check-net: build quanta first (run 'make')"
    exit 1
fi
if [ ! -f "$ELF" ]; then
    echo "check-net: $ELF missing (run 'make tests')"
    exit 1
fi

"$QUANTA" --quiet --netdev=user "$ELF" </dev/null >/dev/null 2>&1
rc=$?

if [ "$rc" -ne 0 ]; then
    echo "check-net: FAILED — $ELF exited $rc (network check $rc failed)"
    exit 1
fi

echo "OK    usermode network stack: ARP request -> reply through virtio-net"
echo "usermode network stack behaves as expected"
