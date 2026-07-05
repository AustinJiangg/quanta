#!/usr/bin/env python3
# A self-contained GDB remote-serial-protocol client that exercises Quanta's
# --gdb stub end to end, without needing a riscv `gdb` installed. It speaks the
# same wire protocol gdb does: frame "$payload#cc", ack with "+", parse replies.
#
# It drives tests/hello.elf, whose instruction sequence is fixed and known:
#
#   80000000  addi a0, zero, 5     -> a0 = 5
#   80000004  addi a1, zero, 37    -> a1 = 37
#   80000008  add  a2, a0, a1      -> a2 = 42
#   8000000c  sub  a3, a1, a0      -> a3 = 32
#   80000010  li   a7, 93          -> a7 = exit
#   80000014  li   a0, 0           -> status 0
#   80000018  ecall                -> exit(0)
#
# so single-step, breakpoint+continue, register and memory access all have
# predictable, asserted outcomes. Usage: gdb_client.py ./quanta tests/hello.elf
import socket
import subprocess
import sys
import time

# RISC-V register indices in the g/G packet (x0..x31 then pc at 32).
A0, A1, A2, A3, A7, T0, PC = 10, 11, 12, 13, 17, 5, 32

failures = []


def check(cond, msg):
    if cond:
        print(f"  ok   {msg}")
    else:
        print(f"  FAIL {msg}")
        failures.append(msg)


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Rsp:
    def __init__(self, sock):
        self.sock = sock

    def _readn(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise EOFError("connection closed")
            buf += chunk
        return buf

    def send(self, data):
        csum = sum(data.encode()) & 0xFF
        self.sock.sendall(b"$" + data.encode() + b"#" + f"{csum:02x}".encode())
        # read the stub's ack ('+'), tolerating a leading '-' retry request
        while True:
            b = self._readn(1)
            if b == b"+":
                break
            if b == b"-":
                self.sock.sendall(
                    b"$" + data.encode() + b"#" + f"{csum:02x}".encode())

    def recv(self):
        while self._readn(1) != b"$":
            pass
        data = b""
        while True:
            b = self._readn(1)
            if b == b"#":
                break
            data += b
        self._readn(2)  # checksum
        self.sock.sendall(b"+")
        return data.decode()

    def cmd(self, data):
        self.send(data)
        return self.recv()


def reg(regs_hex, idx):
    """Decode register `idx` from a 'g' reply (8 little-endian hex chars each)."""
    h = regs_hex[idx * 8:idx * 8 + 8]
    return int.from_bytes(bytes.fromhex(h), "little")


def main():
    quanta, elf = sys.argv[1], sys.argv[2]
    port = free_port()
    proc = subprocess.Popen([quanta, f"--gdb={port}", "--quiet", elf],
                            stdout=subprocess.DEVNULL)
    try:
        # Connect, retrying until the stub's listening socket is up.
        sock = None
        for _ in range(100):
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=5)
                break
            except OSError:
                time.sleep(0.05)
        if sock is None:
            print("  FAIL could not connect to the stub")
            return 1
        sock.settimeout(5)
        g = Rsp(sock)

        # Capability negotiation and the target description.
        sup = g.cmd("qSupported:swbreak+;hwbreak+")
        check("PacketSize" in sup, "qSupported advertises PacketSize")
        xml = g.cmd("qXfer:features:read:target.xml:0,1000")
        check(xml[:1] in ("l", "m") and "riscv:rv32" in xml,
              "target.xml describes riscv:rv32")

        # Initial stop and starting state.
        stop = g.cmd("?")
        check(stop.startswith(("S", "T")), f"? -> stop reply ({stop})")
        regs = g.cmd("g")
        check(len(regs) == 33 * 8, "g returns 33 registers")
        check(reg(regs, PC) == 0x80000000, "pc starts at the entry point")
        check(reg(regs, A0) == 0, "a0 starts at 0")

        # Memory read: the first instruction word, little-endian on the wire.
        mem = g.cmd("m80000000,4")
        check(mem == "13055000", f"m reads the first instruction word ({mem})")

        # Memory write then read-back, past the code.
        check(g.cmd("M80000020,4:efbeadde") == "OK", "M writes memory")
        check(g.cmd("m80000020,4") == "efbeadde", "written memory reads back")

        # Register write then read-back via a fresh 'g'.
        check(g.cmd("P5=78563412") == "OK", "P writes t0")
        check(reg(g.cmd("g"), T0) == 0x12345678, "written register reads back")

        # Single step: executes addi a0,zero,5.
        st = g.cmd("s")
        check(st.startswith(("S05", "T05")), f"s -> SIGTRAP ({st})")
        regs = g.cmd("g")
        check(reg(regs, PC) == 0x80000004, "pc advanced one instruction")
        check(reg(regs, A0) == 5, "a0 == 5 after the first step")

        # Breakpoint at the li a7 instruction, then continue to it.
        check(g.cmd("Z0,80000010,4") == "OK", "Z0 sets a breakpoint")
        st = g.cmd("c")
        check(st.startswith(("S05", "T05")), f"c -> stop at breakpoint ({st})")
        regs = g.cmd("g")
        check(reg(regs, PC) == 0x80000010, "stopped at the breakpoint address")
        check(reg(regs, A2) == 42, "a2 == 42 at the breakpoint")
        check(reg(regs, A3) == 32, "a3 == 32 at the breakpoint")

        # --- reverse execution (E10): the snapshot/replay time machine ---
        check("ReverseStep" in sup and "ReverseContinue" in sup,
              "qSupported advertises reverse execution")

        # bs: step back over the `sub a3,a1,a0` — a3 is un-written again, a2 stays.
        st = g.cmd("bs")
        check(st.startswith(("S05", "T05")), f"bs -> SIGTRAP ({st})")
        regs = g.cmd("g")
        check(reg(regs, PC) == 0x8000000c, "bs steps pc back to the sub")
        check(reg(regs, A3) == 0, "a3 un-written after reverse-step")
        check(reg(regs, A2) == 42, "a2 still 42 after reverse-step")

        # A second bs backs over `add a2,a0,a1` — now a2 is un-written too.
        g.cmd("bs")
        regs = g.cmd("g")
        check(reg(regs, PC) == 0x80000008, "second bs steps back to the add")
        check(reg(regs, A2) == 0, "a2 un-written two steps back")

        # Stepping forward again deterministically replays the add.
        g.cmd("s")
        regs = g.cmd("g")
        check(reg(regs, PC) == 0x8000000c, "forward step replays past the add")
        check(reg(regs, A2) == 42, "a2 == 42 again after replay")

        # bc: reverse-continue to an earlier breakpoint (the second instruction),
        # rewinding across several instructions at once.
        check(g.cmd("Z0,80000004,4") == "OK", "Z0 sets an earlier breakpoint")
        st = g.cmd("bc")
        check(st.startswith(("S05", "T05")), f"bc -> SIGTRAP ({st})")
        regs = g.cmd("g")
        check(reg(regs, PC) == 0x80000004, "bc rewound to the earlier breakpoint")
        check(reg(regs, A0) == 5, "a0 == 5 at the rewound breakpoint")
        # a1 holds the boot DTB pointer here, not 0 — the point is it is not yet
        # the 37 that `addi a1,zero,37` writes one instruction later.
        check(reg(regs, A1) != 37, "a1 not yet written to 37 at the rewound breakpoint")
        check(g.cmd("z0,80000004,4") == "OK", "z0 clears the earlier breakpoint")

        # Remove the first breakpoint and run forward to the exit syscall.
        check(g.cmd("z0,80000010,4") == "OK", "z0 clears the breakpoint")
        st = g.cmd("c")
        check(st == "W00", f"c -> clean exit, code 0 ({st})")

        # Kill: the stub ends and the process exits 0.
        g.send("k")
        sock.close()
        try:
            rc = proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            rc = None
        check(rc == 0, f"quanta exits 0 after the session ({rc})")
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5)

    if failures:
        print(f"check-gdb: FAILED ({len(failures)} assertion(s))")
        return 1
    print("check-gdb: all GDB stub assertions passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
