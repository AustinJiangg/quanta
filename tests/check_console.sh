#!/bin/sh
# Exercise the raw-mode interactive console (main.c's console_raw_enable) over a
# real pseudo-terminal, which the piped check_uart_rx.sh cannot reach: a pty makes
# isatty() true, so raw mode actually engages. tests/console_pty.py drives
# tests/uart_echo.elf and asserts the banner, character echo, the Ctrl-A escapes,
# and — the safety property — that a terminating signal restores the terminal.
#
# Skips cleanly when python3 is absent, the same "no tool, no problem" stance as
# check_gdb.sh and check_diff.sh.
set -u

if ! command -v python3 >/dev/null 2>&1; then
    echo "check-console: python3 not found — skipping"
    echo "  (install python3 to exercise the interactive console)"
    exit 0
fi

QUANTA=./quanta
ELF=tests/uart_echo.elf

if [ ! -x "$QUANTA" ]; then
    echo "check-console: build quanta first (run 'make')"
    exit 1
fi
if [ ! -f "$ELF" ]; then
    echo "check-console: $ELF missing (run 'make tests')"
    exit 1
fi

exec python3 tests/console_pty.py "$QUANTA" "$ELF"
