#ifndef QUANTA_GDBSTUB_H
#define QUANTA_GDBSTUB_H

#include "quanta.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * gdbstub — a GDB remote-serial-protocol server for a libquanta instance.
 *
 * Built entirely on the public engine API (quanta.h): register and memory
 * access, single-step, and the halt reason. The stub owns no engine internals,
 * so it doubles as a worked example of driving the machine from outside — and as
 * a headline embeddable feature: link libquanta, call quanta_gdb_serve(), and a
 * real gdb can attach, read/write state, set breakpoints, step, and continue.
 *
 * The protocol it speaks is the standard GDB RSP over TCP (the same wire format
 * gdbserver and qemu's -s use), so the client is stock `gdb`:
 *
 *     quanta --gdb=1234 program.elf        # in one terminal
 *     riscv64-unknown-elf-gdb program.elf  # in another
 *     (gdb) target remote :1234
 *
 * Breakpoints are managed stub-side (the Z0/z0 packets): the continue loop stops
 * when the PC reaches one, so guest memory is never patched with trap words.
 * A target description advertising the RV32 register file (x0..x31 + pc) is
 * served via qXfer, so gdb learns the layout without a hand-set architecture.
 */

/* Serve the GDB remote protocol for `q` on TCP `port`, bound to localhost.
 * Blocks: it listens, accepts one debugger connection, and drives the machine
 * on the debugger's behalf until the debugger detaches/kills or the guest exits.
 * Returns 0 on a clean session, -1 if the listening socket could not be set up
 * (the message is printed to stderr). `q` must already have a program loaded. */
int quanta_gdb_serve(Quanta *q, int port);

#ifdef __cplusplus
}
#endif

#endif /* QUANTA_GDBSTUB_H */
