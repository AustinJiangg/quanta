#!/usr/bin/env python3
# Exercise the raw-mode interactive console end to end over a real pseudo-terminal
# (so isatty() is true and main.c's console_raw_enable engages) — the path the
# piped check_uart_rx.sh cannot reach. Drives tests/uart_echo.elf, which echoes
# each received byte back through the UART and exits on newline.
#
# Checks, in order:
#   1. raw-mode banner prints; a typed line is echoed; newline exits clean (0)
#   2. Ctrl-A x stops the run without the guest ever seeing a newline
#   3. Ctrl-A Ctrl-A delivers one literal Ctrl-A (0x01), which the guest echoes
#   4. a terminating signal (SIGTERM) restores the terminal it saved, so a killed
#      emulator never leaves the user's shell in raw mode
#
# Self-contained (no third-party deps); invoked by tests/check_console.sh, which
# skips cleanly when python3 is absent. Usage: console_pty.py <quanta> <elf>
import os, pty, select, signal, sys, termios, time

if len(sys.argv) != 3:
    print("usage: console_pty.py <quanta> <elf>", file=sys.stderr)
    sys.exit(2)
QUANTA, ELF = sys.argv[1], sys.argv[2]

fails = 0

def check(cond, ok_msg, fail_msg):
    global fails
    if cond:
        print("OK    " + ok_msg)
    else:
        print("FAIL  " + fail_msg)
        fails += 1

def run(inputs):
    """Spawn quanta on a pty, feed `inputs` (list of bytes) spaced out, and
    collect everything it writes. Returns (output_bytes, exit_code)."""
    pid, fd = pty.fork()
    if pid == 0:                                   # child: stdio is the pty (a tty)
        os.execv(QUANTA, [QUANTA, ELF])
        os._exit(127)
    # Silence the pty's own input echo so the only bytes we read back are what the
    # guest transmits — quanta will also drop echo when it goes raw; this just
    # closes the brief startup window before it does.
    try:
        a = termios.tcgetattr(fd)
        a[3] &= ~termios.ECHO
        termios.tcsetattr(fd, termios.TCSANOW, a)
    except Exception:
        pass
    out = b""
    it = iter(inputs)
    sent_all = False
    deadline = time.time() + 8.0
    while time.time() < deadline:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            try:
                data = os.read(fd, 4096)
            except OSError:                        # slave closed on exit
                break
            if not data:
                break
            out += data
        if not sent_all:
            try:
                os.write(fd, next(it))
                time.sleep(0.06)
            except StopIteration:
                sent_all = True
    try:
        wpid, status = os.waitpid(pid, os.WNOHANG)
        if wpid == 0:
            time.sleep(0.3)
            os.kill(pid, signal.SIGKILL)
            _, status = os.waitpid(pid, 0)
    except ChildProcessError:
        status = 0
    os.close(fd)
    return out, os.waitstatus_to_exitcode(status)

# 1. Echo + newline exit.
out, code = run([b"h", b"i", b"\n"])
check(b"Console: raw mode" in out, "raw mode engaged on a tty",
      "no raw-mode banner on a tty")
check(b"hi" in out and code == 0,
      "typed line echoed, newline exited clean",
      f"echo/exit wrong (code={code}, out={out!r})")

# 2. Ctrl-A x quits mid-run (no newline ever sent to the guest).
out, code = run([b"a", b"b", b"\x01", b"x"])
check(b"ab" in out and b"Console quit" in out and code == 0,
      "Ctrl-A x stopped the run cleanly (exit 0)",
      f"Ctrl-A x quit wrong (code={code}, out={out!r})")

# 3. Ctrl-A Ctrl-A -> one literal Ctrl-A, echoed by the guest, then newline exit.
out, code = run([b"\x01", b"\x01", b"\n"])
check(b"\x01" in out and code == 0,
      "Ctrl-A Ctrl-A delivered a literal Ctrl-A",
      f"literal Ctrl-A not echoed (code={code}, out={out!r})")

# 4. A terminating signal restores the saved terminal settings.
master, slave = pty.openpty()
cooked = termios.tcgetattr(slave)
cooked[3] |= (termios.ECHO | termios.ICANON)        # a normal shell's cooked tty
termios.tcsetattr(slave, termios.TCSANOW, cooked)
cooked_lflag = termios.tcgetattr(slave)[3]
pid = os.fork()
if pid == 0:
    os.setsid()
    for f in (0, 1, 2):
        os.dup2(slave, f)
    os.close(master); os.close(slave)
    os.execv(QUANTA, [QUANTA, ELF])
    os._exit(127)
os.close(slave)
time.sleep(0.5)                                     # let it reach the run loop (raw)
raw_lflag = termios.tcgetattr(master)[3]
os.kill(pid, signal.SIGTERM)
_, status = os.waitpid(pid, 0)
restored_lflag = termios.tcgetattr(master)[3]
os.close(master)
went_raw = not (raw_lflag & termios.ECHO) and not (raw_lflag & termios.ICANON)
restored = restored_lflag == cooked_lflag
by_sigterm = os.WIFSIGNALED(status) and os.WTERMSIG(status) == signal.SIGTERM
check(went_raw and restored and by_sigterm,
      "SIGTERM restored the terminal before dying",
      f"terminal not restored on signal (raw={went_raw}, "
      f"restored={restored}, by_sigterm={by_sigterm})")

if fails == 0:
    print("interactive console: raw mode, escapes, and signal restore OK")
sys.exit(1 if fails else 0)
