/* Expose the POSIX select()/read() API under -std=c11 for the console (pumping
 * host stdin into the guest UART). Isolated here as gdbstub.c does for its
 * sockets — the two are the driver's only OS-specific corners. */
#define _DEFAULT_SOURCE 1 /* NOLINT(bugprone-reserved-identifier): feature-test macro */

#include "quanta.h"
#include "disasm.h"
#include "pipeline.h"
#include "gdbstub.h"
#include "netstack.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>
#if defined(__linux__)
#include <net/if.h>
#include <linux/if_tun.h>   /* the TAP backend (--netdev=tap); Linux-only */
#endif

/*
 * Quanta driver — a thin client over libquanta.
 *
 * Usage:
 *   quanta [--version] [--trace] [--quiet] [--cache[=SIZE:WAYS:BLOCK]]
 *          [--pipeline] [--memory=SIZE] [--harts=N] [--max-steps=N] [--gdb[=PORT]] [--signature=FILE]
 *          [--disk=FILE | --disk-ro=FILE] [--netdev=user|tap] [--snapshot=FILE] [--restore=FILE]
 *          [--bios=FILE --kernel=FILE [--append=STRING] [--initrd=FILE]]
 *          [program.elf]
 *
 * --snapshot=FILE writes the whole machine state to FILE when the run ends;
 * --restore=FILE rebuilds a machine from such a file and resumes it (no program
 * needed). Together they checkpoint and resume a long run, e.g.
 * `quanta --max-steps=N --snapshot=cp ... ` then `quanta --restore=cp`.
 *
 * --bios (an M-mode firmware ELF, e.g. OpenSBI) with --kernel (a raw S-mode OS
 * image, e.g. a Linux Image) boots the way a real machine does: the firmware runs
 * first and hands off to the OS in S-mode. --append sets the kernel command line
 * (the device tree's /chosen bootargs), e.g. "earlycon=sbi console=ttyS0", and
 * --initrd stages a cpio initramfs as the kernel's root filesystem. Without
 * --bios, a program.elf is run directly in M-mode (Quanta acting as the SEE/SBI).
 *
 * A running guest can take console input: host stdin is pumped into the UART's
 * receive path during the run, so a full-system guest has a keyboard. When stdin
 * is a terminal the run puts it in raw mode (character-at-a-time, no host echo,
 * Ctrl-A x to quit), mirroring qemu's -nographic console; a pipe or file is read
 * verbatim. --disk attaches a raw block-device image the virtio-mmio device serves,
 * persisting guest writes back to the file; --disk-ro attaches it read-only (guest
 * writes are buffered in RAM and discarded at exit, leaving the file untouched).
 *
 * With a path, Quanta loads that RV32I ELF executable and runs it from its
 * entry point. With no argument, it runs a tiny built-in demo program — a
 * toolchain-free smoke test — so the emulator stays runnable even without the
 * RISC-V cross-toolchain installed.
 *
 * Everything that touches the machine goes through the public engine API
 * (quanta.h); the driver only owns argument parsing, the trace narration, and
 * the pipeline timing overlay. The disassembler and pipeline model are library
 * utilities the driver still calls directly.
 *
 * --quiet suppresses all driver chatter (banner, summary, reports, register
 * dump), leaving only what the guest itself writes — so quanta's output lines
 * up byte-for-byte with a reference simulator for differential testing.
 *
 * The demo computes a couple of values, then asks the "kernel" to terminate it
 * with the exit syscall (a7 = 93). a0 carries the status, so it doubles as the
 * exit code:
 *
 *   addi a0, zero, 5     # a0 = 5
 *   addi a1, zero, 37    # a1 = 37
 *   add  a2, a0, a1      # a2 = a0 + a1 = 42
 *   sub  a3, a1, a0      # a3 = a1 - a0 = 32
 *   addi a7, zero, 93    # a7 = exit
 *   addi a0, zero, 0     # status = 0   (reuses a0)
 *   ecall                # exit(0)
 *
 * tests/hello.S is the same program in assembly; tests/hello_world.S goes one
 * step further and prints a string with the write syscall before exiting.
 */

#define MEM_BASE 0x80000000u
#define MEM_SIZE (1u << 16)   /* 64 KiB is plenty for the demo */

static const uint32_t demo_program[] = {
    0x00500513, /* addi a0, zero, 5   -> a0 = 5            */
    0x02500593, /* addi a1, zero, 37  -> a1 = 37           */
    0x00b50633, /* add  a2, a0, a1    -> a2 = 42           */
    0x40a586b3, /* sub  a3, a1, a0    -> a3 = 32           */
    0x05d00893, /* addi a7, zero, 93  -> a7 = exit syscall */
    0x00000513, /* addi a0, zero, 0   -> exit status 0     */
    0x00000073  /* ecall              -> exit(0)           */
};

/* Parse a byte count with an optional K/M/G (1024-based) suffix, e.g. "8M" or
 * "0x10000". Writes the result to *out; returns 0 on success, -1 on a malformed
 * value or a count that overflows the 32-bit address space. */
static int parse_size(const char *s, uint32_t *out) {
    if (!s || !*s) return -1;
    char *end;
    unsigned long long v = strtoull(s, &end, 0);
    unsigned long long mul = 1;
    switch (*end) {
        case 'K': case 'k': mul = 1024ULL;               end++; break;
        case 'M': case 'm': mul = 1024ULL * 1024;        end++; break;
        case 'G': case 'g': mul = 1024ULL * 1024 * 1024; end++; break;
        default: break;
    }
    if (*end != '\0') return -1;
    v *= mul;
    if (v == 0 || v > 0xffffffffULL) return -1;
    *out = (uint32_t)v;
    return 0;
}

/* Parse an instruction-count cap with an optional K/M/G/T (1000-based) suffix,
 * e.g. "500M" or "2G". 0 means "no cap" — for an interactive full-system guest
 * (a booting OS) that legitimately runs billions of instructions. Writes the
 * result to *out; returns 0 on success, -1 on a malformed value. Uses decimal
 * multipliers, unlike parse_size's 1024-based memory sizes. */
static int parse_steps(const char *s, uint64_t *out) {
    if (!s || !*s) return -1;
    char *end;
    unsigned long long v = strtoull(s, &end, 0);
    unsigned long long mul = 1;
    switch (*end) {
        case 'K': case 'k': mul = 1000ULL;                      end++; break;
        case 'M': case 'm': mul = 1000ULL * 1000;               end++; break;
        case 'G': case 'g': mul = 1000ULL * 1000 * 1000;        end++; break;
        case 'T': case 't': mul = 1000ULL * 1000 * 1000 * 1000; end++; break;
        default: break;
    }
    if (*end != '\0') return -1;
    *out = (uint64_t)v * mul;
    return 0;
}

/* Execute one instruction and narrate it to stderr: the PC, the raw word, its
 * disassembly, and any register the step changed (with the new value), plus the
 * redirect target when control does not simply fall through. Trace goes to
 * stderr so it stays separate from whatever the guest prints via the write
 * syscall on stdout. "What changed" is recovered by diffing a register snapshot
 * taken around the step through the public accessors, so the engine core stays
 * untouched. */
static void trace_step(Quanta *q) {
    int      w    = (quanta_xlen(q) == 64) ? 16 : 8; /* hex digits per XLEN value */
    uint64_t pc   = quanta_pc(q);
    uint32_t raw  = quanta_read_u32(q, pc, NULL);
    /* The low two bits give the length: a 16-bit compressed instruction (RVC)
     * unless they are 0b11. Show only the bytes that belong to it. */
    uint32_t ilen = ((raw & 0x3u) == 0x3u) ? 4u : 2u;
    uint32_t inst = (ilen == 2) ? (raw & 0xffffu) : raw;
    uint64_t before[32];
    for (int i = 0; i < 32; i++) before[i] = quanta_reg(q, i);

    char text[80];
    disasm(pc, inst, quanta_xlen(q), text, sizeof text);

    quanta_step(q);

    if (ilen == 2) fprintf(stderr, "%0*llx:  %04x      %-24s", w, (unsigned long long)pc, inst, text);
    else           fprintf(stderr, "%0*llx:  %08x  %-24s", w, (unsigned long long)pc, inst, text);
    for (int i = 1; i < 32; i++) /* x0 is hardwired; it can never change */
        if (quanta_reg(q, i) != before[i])
            fprintf(stderr, " %s=0x%0*llx", quanta_reg_name(i), w, (unsigned long long)quanta_reg(q, i));
    if (quanta_pc(q) != pc + ilen) /* taken branch, jump, or trap */
        fprintf(stderr, " ->0x%0*llx", w, (unsigned long long)quanta_pc(q));
    fprintf(stderr, "\n");
}

/* --- console input ---------------------------------------------------------
 * Pump host stdin into the guest UART's receive path so a full-system guest (a
 * booting kernel) has a keyboard. Readiness is checked with a zero-timeout
 * select() rather than by making stdin non-blocking: stdin's flags are shared
 * with the parent shell, so mutating them is a footgun a crash would leave
 * behind. When stdin is a tty the terminal is put in raw mode for the run (see
 * console_raw_enable) so keystrokes reach the guest one at a time, unechoed, and
 * signal/flow-control keys are delivered as bytes; a pipe or file is read as-is,
 * which is what the piped-input test relies on. */
static int stdin_has_byte(void) {
    fd_set r;
    FD_ZERO(&r);
    FD_SET(STDIN_FILENO, &r);
    struct timeval tv = { 0, 0 };
    return select(STDIN_FILENO + 1, &r, NULL, NULL, &tv) > 0;
}

/* Raw-mode terminal handling. The saved settings and the "raw is active" flag
 * are file-scope so the atexit/signal restore path reaches them without an
 * argument: once raw mode is on, a signal (or normal exit) can fire at any point
 * and MUST put the terminal back, or it would leave the user's shell unusable
 * (no echo, no line editing). tcsetattr/signal/raise are all async-signal-safe,
 * so calling them from the handler is defined. */
static struct termios g_console_saved;
static volatile sig_atomic_t g_console_raw  = 0; /* terminal currently in raw mode */
static volatile sig_atomic_t g_console_quit = 0; /* Ctrl-A x: stop the run */

/* Restore the terminal to how we found it. Idempotent, so atexit and a signal
 * handler can both call it. */
static void console_restore(void) {
    if (g_console_raw) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_console_saved);
        g_console_raw = 0;
    }
}

/* On a terminating signal, put the terminal back and re-raise with the default
 * disposition so the process dies as it normally would (right exit status to the
 * parent). Reached only for signals that can arrive from outside while raw mode
 * is on — with ISIG cleared, Ctrl-C/\ do not generate SIGINT/SIGQUIT here. */
static void console_on_signal(int sig) {
    console_restore();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Put a tty into raw mode for the duration of the run, mirroring qemu's
 * -nographic terminal setup: character-at-a-time input, no host echo (the guest
 * echoes), and signal/flow-control keys delivered to the guest as bytes rather
 * than acted on by the host — but OUTPUT processing (c_oflag) is left alone so
 * the guest's bare '\n' still displays as a proper new line (ONLCR maps it to
 * CR-LF). A pipe or file is not a tty, so this is a no-op there and input flows
 * through verbatim. On success, arms an atexit and signal handlers to guarantee
 * the terminal is restored however the process ends. */
static void console_raw_enable(void) {
    if (!isatty(STDIN_FILENO)) return;               /* a pipe/file: leave verbatim */
    if (tcgetattr(STDIN_FILENO, &g_console_saved) != 0) return;

    struct termios raw = g_console_saved;
    raw.c_iflag &= ~(tcflag_t)(IGNBRK | BRKINT | PARMRK | ISTRIP |
                               INLCR | IGNCR | ICRNL | IXON);
    raw.c_lflag &= ~(tcflag_t)(ECHO | ECHONL | ICANON | IEXTEN | ISIG);
    raw.c_cflag &= ~(tcflag_t)(CSIZE | PARENB);
    raw.c_cflag |=  (tcflag_t)CS8;
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
    g_console_raw = 1;

    atexit(console_restore);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = console_on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
}

/* Move at most one byte from host stdin into the guest UART. A one-byte holding
 * slot keeps a byte read from stdin from being dropped when the UART's receive
 * buffer is momentarily full: read a fresh byte only when nothing is pending, and
 * clear the pending byte once the UART accepts it. In raw (tty) mode a Ctrl-A
 * prefix is the escape (qemu convention): Ctrl-A x quits the emulator, Ctrl-A
 * Ctrl-A sends one literal Ctrl-A, so the guest can still receive every other
 * key — including a bare Ctrl-C, which ISIG-off delivers as a byte. The escape
 * is skipped for a pipe/file so byte streams pass through unaltered. */
static void console_pump(Quanta *q) {
    static int pending = -1;
    static int escape = 0;             /* saw the Ctrl-A prefix, awaiting its command */
    if (pending < 0 && stdin_has_byte()) {
        unsigned char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            /* The Ctrl-A escape applies only in raw (tty) mode; a pipe/file
             * (g_console_raw == 0) always falls to the verbatim branch below. */
            if (g_console_raw && escape) {
                escape = 0;
                if (c == 'x' || c == 'X') g_console_quit = 1; /* Ctrl-A x: quit */
                else pending = (int)c;               /* Ctrl-A Ctrl-A -> one Ctrl-A */
            } else if (g_console_raw && c == 0x01) {
                escape = 1;                           /* Ctrl-A: await command byte */
            } else {
                pending = (int)c;                    /* ordinary byte, or piped verbatim */
            }
        }
    }
    if (pending >= 0 && quanta_uart_input(q, (uint8_t)pending)) pending = -1;
}

/* ---------------------------------------------------------------------------
 * POSIX socket backend for the usermode NAT stack (--netdev=user).
 *
 * The network stack (netstack.c) is host-independent; it reaches the real world
 * through this NetIo backend, the driver's third OS-specific corner (after the
 * console input above and gdbstub.c's sockets). A NAT flow is named by the small
 * integer id the stack allocates; we key our socket table by that same id. All
 * sockets are non-blocking, polled once per run-loop tick (net_pump) with a
 * zero-timeout select, and host-side events are handed back to the stack through
 * its netstack_host_* entry points.
 * ------------------------------------------------------------------------- */
typedef struct { int fd; int proto; int connecting; } NetSock;
typedef struct { NetStack *ns; NetSock socks[NS_NAT_MAX]; } NetPosix;

static int net_io_open(void *ctx, int id, int proto, const uint8_t ip[4],
                       uint16_t port) {
    NetPosix *np = ctx;
    if (id < 0 || id >= NS_NAT_MAX || np->socks[id].fd >= 0) return -1;
    int fd = socket(AF_INET, proto == NS_PROTO_TCP ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    memcpy(&sa.sin_addr, ip, 4);
    int r = connect(fd, (struct sockaddr *)&sa, sizeof sa);
    np->socks[id].proto = proto;
    np->socks[id].connecting = 0;
    if (proto == NS_PROTO_TCP) {
        if (r < 0 && errno != EINPROGRESS) { close(fd); return -1; }
        np->socks[id].connecting = 1;             /* completion reported via poll */
    } else if (r < 0) {
        close(fd);
        return -1;
    }
    np->socks[id].fd = fd;
    return 0;
}

static int net_io_send(void *ctx, int id, const uint8_t *data, uint32_t len) {
    NetPosix *np = ctx;
    if (id < 0 || id >= NS_NAT_MAX || np->socks[id].fd < 0) return -1;
    ssize_t n = send(np->socks[id].fd, data, len, 0);
    if (n >= 0) return (int)n;
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return 0;
    return -1;
}

static void net_io_shutdown(void *ctx, int id) {
    NetPosix *np = ctx;
    if (id >= 0 && id < NS_NAT_MAX && np->socks[id].fd >= 0)
        shutdown(np->socks[id].fd, SHUT_WR);
}

static void net_io_close(void *ctx, int id) {
    NetPosix *np = ctx;
    if (id >= 0 && id < NS_NAT_MAX && np->socks[id].fd >= 0) {
        close(np->socks[id].fd);
        np->socks[id].fd = -1;
        np->socks[id].connecting = 0;
    }
}

/* The first `nameserver` in /etc/resolv.conf is the resolver DNS packets to the
 * virtual DNS server (10.0.2.3) are relayed to; fall back to a public one. */
static void resolve_upstream_dns(uint8_t out[4]) {
    out[0] = 8; out[1] = 8; out[2] = 8; out[3] = 8;
    FILE *f = fopen("/etc/resolv.conf", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned a, b, c, d;
        if (sscanf(line, " nameserver %u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
            a < 256 && b < 256 && c < 256 && d < 256) {
            out[0] = (uint8_t)a; out[1] = (uint8_t)b;
            out[2] = (uint8_t)c; out[3] = (uint8_t)d;
            break;
        }
    }
    fclose(f);
}

/* ---------------------------------------------------------------------------
 * TAP backend for --netdev=tap: a raw layer-2 bridge to a host TAP device.
 *
 * Unlike the usermode stack, this does no protocol work — it shuttles whole
 * ethernet frames between the guest's virtio-net device and /dev/net/tun, and
 * the host owns the other end (its own addressing, routing, and NAT). Creating a
 * fresh TAP needs CAP_NET_ADMIN, but attaching to a persistent one pre-created
 * for this user (`ip tuntap add tapN mode tap user $USER`) works unprivileged,
 * which is the intended use. Linux-only; a manual milestone (no host TAP to test
 * against here). Returns the fd, or -1 (with errno set) if it cannot attach.
 * ------------------------------------------------------------------------- */
#if defined(__linux__)
static int tap_open(const char *name) {
    int fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) return -1;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;          /* raw ethernet frames, no header */
    if (name && *name) {
        strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
        ifr.ifr_name[IFNAMSIZ - 1] = '\0';
    }
    if (ioctl(fd, TUNSETIFF, &ifr) < 0) { close(fd); return -1; }
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    return fd;
}
#else
static int tap_open(const char *name) { (void)name; errno = ENOSYS; return -1; }
#endif

/* Device transmit callback for the TAP backend: write the frame to the TAP fd
 * (ctx is &tap_fd). A short/failed write drops the frame, as a real NIC would. */
static void tap_backend_tx(void *ctx, const uint8_t *frame, uint32_t len) {
    int fd = *(int *)ctx;
    if (fd >= 0) { ssize_t w = write(fd, frame, len); (void)w; }
}

/* Poll all open NAT sockets once (zero timeout) and drive the stack: complete
 * TCP connects, deliver received bytes, and retry buffered writes. A callback
 * may free a flow (closing its fd), so the fd is re-checked before each use. */
static void net_pump_user(NetPosix *np) {
    if (!np) return;
    fd_set rfds, wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    int maxfd = -1;
    for (int id = 0; id < NS_NAT_MAX; id++) {
        int fd = np->socks[id].fd;
        if (fd < 0) continue;
        int want_r = 0, want_w = 0;
        if (np->socks[id].connecting) {
            want_w = 1;                            /* watch for connect completion */
        } else {
            if (netstack_rx_room(np->ns, id) > 0) want_r = 1;
            if (netstack_wants_write(np->ns, id)) want_w = 1;
        }
        if (want_r) { FD_SET(fd, &rfds); if (fd > maxfd) maxfd = fd; }
        if (want_w) { FD_SET(fd, &wfds); if (fd > maxfd) maxfd = fd; }
    }
    if (maxfd < 0) return;
    struct timeval tv = { 0, 0 };
    if (select(maxfd + 1, &rfds, &wfds, NULL, &tv) <= 0) return;

    for (int id = 0; id < NS_NAT_MAX; id++) {
        int fd = np->socks[id].fd;
        if (fd < 0) continue;
        if (np->socks[id].connecting && FD_ISSET(fd, &wfds)) {
            int err = 0;
            socklen_t el = sizeof err;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
            np->socks[id].connecting = 0;
            if (err) { netstack_host_reset(np->ns, id); continue; }
            netstack_host_connected(np->ns, id);
            if (np->socks[id].fd < 0) continue;    /* freed by the callback */
        }
        if (np->socks[id].fd >= 0 && FD_ISSET(fd, &rfds)) {
            uint32_t room = netstack_rx_room(np->ns, id);
            if (room > 0) {
                static uint8_t buf[16384];
                uint32_t cap = room < sizeof buf ? room : (uint32_t)sizeof buf;
                ssize_t n = recv(fd, buf, cap, 0);
                if (n > 0)       netstack_host_recv(np->ns, id, buf, (uint32_t)n);
                else if (n == 0) netstack_host_closed(np->ns, id);
                else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                    netstack_host_reset(np->ns, id);
            }
        }
        if (np->socks[id].fd >= 0 && !np->socks[id].connecting &&
            FD_ISSET(fd, &wfds) && netstack_wants_write(np->ns, id))
            netstack_host_writable(np->ns, id);
    }
}

/* The active network backend the run loop pumps: exactly one of the usermode NAT
 * sockets (--netdev=user) or a TAP fd (--netdev=tap). */
typedef struct {
    NetPosix *user;    /* --netdev=user: NAT sockets to poll (NULL for tap)    */
    Quanta   *q;       /* engine, for delivering received TAP frames to the guest */
    int       tap_fd;  /* --netdev=tap: the TAP device fd (-1 for user)         */
} NetPump;

/* Service the active backend once. For TAP, drain a bounded burst of frames from
 * the host into the guest's receive path (a frame the FIFO cannot hold is dropped,
 * like a real NIC under overrun). Guest transmit goes the other way through the
 * device backend callback (tap_backend_tx), so it needs no polling. */
static void net_pump(NetPump *np) {
    if (!np) return;
    if (np->user) { net_pump_user(np->user); return; }
    if (np->tap_fd < 0) return;
    for (int i = 0; i < 64; i++) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(np->tap_fd, &rf);
        struct timeval tv = { 0, 0 };
        if (select(np->tap_fd + 1, &rf, NULL, NULL, &tv) <= 0) break;
        uint8_t buf[2048];
        ssize_t n = read(np->tap_fd, buf, sizeof buf);
        if (n <= 0) break;
        quanta_net_rx(np->q, buf, (uint32_t)n);
    }
}

/* Step until the program halts, or a safety limit is hit so a buggy program can
 * never spin forever. With trace set, narrate each instruction to stderr; with a
 * pipeline, feed it each retired instruction. `max_steps` caps the run as a
 * runaway guard (0 = no cap, for an interactive full-system guest that
 * legitimately runs billions of instructions). Returns the instruction count. */
static uint64_t run_until_halt(Quanta *q, int trace, Pipeline *pipe,
                               uint64_t max_steps, NetPump *net) {
    uint64_t steps = 0;

    /* Fast path: with no per-instruction observer attached, drive the engine in
     * console-pump-sized chunks through quanta_run — the per-step quanta_step /
     * quanta_pc / quanta_halt_reason API calls were a measurable tax on a
     * full-system run (M25 perf). The pump cadence is unchanged: one
     * console/net poll per 1024 instructions. */
    if (!trace && !pipe) {
        while (quanta_halt_reason(q) == QUANTA_RUN && !g_console_quit &&
               (max_steps == 0 || steps < max_steps)) {
            console_pump(q); net_pump(net);
            uint64_t chunk = 0x400;
            if (max_steps && max_steps - steps < chunk) chunk = max_steps - steps;
            uint64_t ran = 0;
            quanta_run(q, chunk, &ran); /* stops early on any halt */
            steps += ran;
        }
        return steps;
    }

    while (quanta_halt_reason(q) == QUANTA_RUN && !g_console_quit &&
           (max_steps == 0 || steps < max_steps)) {
        /* Feed any waiting console input to the UART, and poll the NAT sockets,
         * occasionally — often enough to feel responsive, rarely enough that the
         * select()s are not a per-instruction tax. Harmless when nothing waits. */
        if ((steps & 0x3ff) == 0) { console_pump(q); net_pump(net); }
        uint32_t pc   = quanta_pc(q);
        uint32_t inst = pipe ? quanta_read_u32(q, pc, NULL) : 0;
        if (trace) trace_step(q);
        else       quanta_step(q);
        /* Feed the timing model the retired instruction and whether control left
         * the fall-through path (a taken branch or a jump). The fall-through is
         * pc + the instruction's length (2 for a compressed instruction). */
        if (pipe) {
            uint32_t ilen = ((inst & 0x3u) == 0x3u) ? 4u : 2u;
            pipeline_observe(pipe, inst, quanta_pc(q) != pc + ilen);
        }
        steps++;
    }
    return steps;
}

/* Dump the RISC-V architectural-test signature region to `sigfile` ("-" for
 * stdout): the words in [begin_signature, end_signature), one per line as eight
 * lowercase hex digits, lowest address first — the format the official suite's
 * reference signatures use. A test fills that region as it runs and brackets it
 * with the two global symbols, which we resolve from the ELF; by the time the
 * machine halts the region holds the result to compare. This makes Quanta a
 * drop-in target for the suite (cf. spike's --test-signature). Returns 0 on
 * success, -1 on error (no such symbols, a malformed region, or a bad write). */
static int dump_signature(const char *path, Quanta *q, const char *sigfile) {
    uint32_t begin, end;
    if (quanta_elf_symbol(path, "begin_signature", &begin) != QUANTA_OK ||
        quanta_elf_symbol(path, "end_signature", &end) != QUANTA_OK) {
        fprintf(stderr, "signature: %s defines no begin_signature/"
                        "end_signature region\n", path);
        return -1;
    }
    if (end < begin || ((end - begin) & 3u) != 0) {
        fprintf(stderr, "signature: malformed region [0x%08x, 0x%08x)\n",
                begin, end);
        return -1;
    }

    FILE *f = (strcmp(sigfile, "-") == 0) ? stdout : fopen(sigfile, "w");
    if (!f) {
        fprintf(stderr, "signature: cannot open %s\n", sigfile);
        return -1;
    }
    int rc = 0;
    for (uint32_t a = begin; a < end; a += 4) {
        int ok = 0;
        uint32_t w = quanta_read_u32(q, a, &ok);
        if (!ok) {
            fprintf(stderr, "signature: 0x%08x is outside guest memory\n", a);
            rc = -1;
            break;
        }
        fprintf(f, "%08x\n", w);
    }
    if (f == stdout) fflush(stdout); else fclose(f);
    return rc;
}

/* Bridge the virtio-net device to the usermode network stack (--netdev=user).
 * A frame the guest transmits goes device -> net_backend_tx -> the stack; a reply
 * the stack produces goes stack -> net_deliver_to_guest -> the device's receive
 * path. The two directions are synchronous: the stack answers during the guest's
 * transmit, so no background polling is needed for its virtual-gateway services. */
static void net_backend_tx(void *ctx, const uint8_t *frame, uint32_t len) {
    netstack_input((NetStack *)ctx, frame, len);
}
static void net_deliver_to_guest(void *ctx, const uint8_t *frame, uint32_t len) {
    quanta_net_rx((Quanta *)ctx, frame, len);
}

int main(int argc, char **argv) {
    int trace = 0;
    int quiet = 0;
    int cache_on = 0;
    int no_dcache = 0;  /* --no-dcache: run the plain interpreter (M25a reference) */
    int jit_on = 0;     /* --jit: enable the basic-block JIT (M25b; opt-in) */
    int pipe_on = 0;
    int gdb_on = 0;
    int gdb_port = 1234;            /* the conventional gdbserver/qemu port */
    uint32_t csize = 1024, cways = 2, cblock = 32; /* default L1 geometry */
    uint32_t mem_req = 0;          /* --memory: minimum guest RAM (0 = image-sized) */
    uint64_t max_steps = 100ull * 1000 * 1000; /* --max-steps runaway cap (0 = none) */
    const char *path = NULL;
    const char *sigfile = NULL; /* --signature=FILE: arch-test signature dump */
    const char *diskpath = NULL; /* --disk=FILE / --disk-ro=FILE: block-device image */
    int disk_writable = 1;       /* --disk persists writes (M24); --disk-ro is an overlay */
    const char *bios = NULL;    /* --bios=FILE: M-mode firmware ELF (OpenSBI) */
    const char *kernel = NULL;  /* --kernel=FILE: raw S-mode OS image (Linux Image) */
    const char *append = NULL;  /* --append=STRING: kernel command line (DTB bootargs) */
    const char *initrd = NULL;  /* --initrd=FILE: cpio initramfs (the kernel's rootfs) */
    const char *snappath = NULL;   /* --snapshot=FILE: save machine state on exit (E10) */
    const char *restorepath = NULL;/* --restore=FILE: resume from a saved snapshot (E10) */
    int nharts = 1;             /* --harts=N: number of harts for SMP (M19) */
    const char *netdev = NULL;  /* --netdev=user|tap: attach a network backend (M23) */
    int         tap_mode = 0;   /* --netdev=tap selected (else the usermode stack)  */
    const char *tap_ifname = NULL; /* --netdev=tap=IFNAME: attach to this TAP device */
    int         tap_fd = -1;    /* the open TAP fd, when --netdev=tap               */
    NetStack *ns = NULL;        /* the usermode network stack, when --netdev=user   */
    NetPosix  netbk;            /* its POSIX socket backend (for outbound NAT)      */
    NetIo     netio;            /* the NetIo vtable pointing at netbk               */
    NetPump   pump;             /* the active backend the run loop services         */
    NetPump  *netbp = NULL;     /* passed to the run loop only when a backend is on */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("quanta %s\n", quanta_version());
            return 0;
        }
        if (strcmp(argv[i], "--trace") == 0) {
            trace = 1;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
        } else if (strcmp(argv[i], "--no-dcache") == 0) {
            no_dcache = 1;  /* disable the decoded-instruction cache (M25a) */
        } else if (strcmp(argv[i], "--jit") == 0) {
            jit_on = 1;     /* enable the basic-block JIT (M25b) */
        } else if (strcmp(argv[i], "--cache") == 0) {
            cache_on = 1;
        } else if (strncmp(argv[i], "--cache=", 8) == 0) {
            cache_on = 1;
            if (sscanf(argv[i] + 8, "%u:%u:%u", &csize, &cways, &cblock) != 3) {
                fprintf(stderr, "bad --cache spec '%s' "
                        "(want SIZE:WAYS:BLOCK, e.g. 1024:2:32)\n", argv[i] + 8);
                return 2;
            }
        } else if (strcmp(argv[i], "--pipeline") == 0) {
            pipe_on = 1;
        } else if (strncmp(argv[i], "--memory=", 9) == 0) {
            if (parse_size(argv[i] + 9, &mem_req) != 0) {
                fprintf(stderr, "bad --memory size '%s' "
                        "(want bytes with an optional K/M/G suffix, e.g. 8M)\n",
                        argv[i] + 9);
                return 2;
            }
        } else if (strncmp(argv[i], "--max-steps=", 12) == 0) {
            if (parse_steps(argv[i] + 12, &max_steps) != 0) {
                fprintf(stderr, "bad --max-steps '%s' "
                        "(want a count with an optional K/M/G/T suffix, or 0 for "
                        "no cap)\n", argv[i] + 12);
                return 2;
            }
        } else if (strcmp(argv[i], "--gdb") == 0) {
            gdb_on = 1;
        } else if (strncmp(argv[i], "--gdb=", 6) == 0) {
            gdb_on = 1;
            if (sscanf(argv[i] + 6, "%d", &gdb_port) != 1 ||
                gdb_port <= 0 || gdb_port > 65535) {
                fprintf(stderr, "bad --gdb port '%s' (want 1..65535)\n",
                        argv[i] + 6);
                return 2;
            }
        } else if (strncmp(argv[i], "--signature=", 12) == 0) {
            sigfile = argv[i] + 12;
            if (sigfile[0] == '\0') {
                fprintf(stderr, "--signature needs a file (or '-' for stdout)\n");
                return 2;
            }
        } else if (strncmp(argv[i], "--disk=", 7) == 0) {
            diskpath = argv[i] + 7;
            disk_writable = 1;
            if (diskpath[0] == '\0') {
                fprintf(stderr, "--disk needs a file path\n");
                return 2;
            }
        } else if (strncmp(argv[i], "--disk-ro=", 10) == 0) {
            diskpath = argv[i] + 10;
            disk_writable = 0;
            if (diskpath[0] == '\0') {
                fprintf(stderr, "--disk-ro needs a file path\n");
                return 2;
            }
        } else if (strncmp(argv[i], "--bios=", 7) == 0) {
            bios = argv[i] + 7;
            if (bios[0] == '\0') {
                fprintf(stderr, "--bios needs a firmware ELF path\n");
                return 2;
            }
        } else if (strncmp(argv[i], "--kernel=", 9) == 0) {
            kernel = argv[i] + 9;
            if (kernel[0] == '\0') {
                fprintf(stderr, "--kernel needs an OS image path\n");
                return 2;
            }
        } else if (strncmp(argv[i], "--append=", 9) == 0) {
            append = argv[i] + 9;   /* kernel command line (may be empty) */
        } else if (strncmp(argv[i], "--initrd=", 9) == 0) {
            initrd = argv[i] + 9;
            if (initrd[0] == '\0') {
                fprintf(stderr, "--initrd needs a file path\n");
                return 2;
            }
        } else if (strncmp(argv[i], "--snapshot=", 11) == 0) {
            snappath = argv[i] + 11;
            if (snappath[0] == '\0') {
                fprintf(stderr, "--snapshot needs a file path\n");
                return 2;
            }
        } else if (strncmp(argv[i], "--restore=", 10) == 0) {
            restorepath = argv[i] + 10;
            if (restorepath[0] == '\0') {
                fprintf(stderr, "--restore needs a file path\n");
                return 2;
            }
        } else if (strncmp(argv[i], "--harts=", 8) == 0) {
            if (sscanf(argv[i] + 8, "%d", &nharts) != 1 ||
                nharts < 1 || nharts > (int)QUANTA_MAX_HARTS) {
                fprintf(stderr, "bad --harts '%s' (want 1..%u)\n",
                        argv[i] + 8, QUANTA_MAX_HARTS);
                return 2;
            }
        } else if (strncmp(argv[i], "--netdev=", 9) == 0) {
            netdev = argv[i] + 9;
            if (strcmp(netdev, "user") == 0) {
                tap_mode = 0;                     /* usermode NAT stack (default) */
            } else if (strcmp(netdev, "tap") == 0 || strncmp(netdev, "tap=", 4) == 0) {
                tap_mode = 1;                     /* raw layer-2 bridge to a host TAP */
                const char *eq = strchr(netdev, '=');
                tap_ifname = eq ? eq + 1 : NULL;
            } else {
                fprintf(stderr, "unknown --netdev backend '%s' (want 'user' or "
                        "'tap[=IFNAME]')\n", netdev);
                return 2;
            }
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            fprintf(stderr, "usage: %s [--version] [--trace] [--quiet] "
                    "[--cache[=SIZE:WAYS:BLOCK]] [--pipeline] [--jit] [--no-dcache] [--memory=SIZE] [--harts=N] [--max-steps=N] "
                    "[--gdb[=PORT]] [--signature=FILE] [--disk=FILE|--disk-ro=FILE] [--netdev=user|tap] "
                    "[--snapshot=FILE] [--restore=FILE] "
                    "[--bios=FILE --kernel=FILE [--append=STRING] [--initrd=FILE]] "
                    "[program.elf]\n",
                    argv[0]);
            return 2;
        } else if (path == NULL) {
            path = argv[i];
        } else {
            fprintf(stderr, "usage: %s [--version] [--trace] [--quiet] "
                    "[--cache[=SIZE:WAYS:BLOCK]] [--pipeline] [--jit] [--no-dcache] [--memory=SIZE] [--harts=N] [--max-steps=N] "
                    "[--gdb[=PORT]] [--signature=FILE] [--disk=FILE|--disk-ro=FILE] [--netdev=user|tap] "
                    "[--snapshot=FILE] [--restore=FILE] "
                    "[--bios=FILE --kernel=FILE [--append=STRING] [--initrd=FILE]] "
                    "[program.elf]\n",
                    argv[0]);
            return 2;
        }
    }

    Quanta *q = quanta_create();
    if (!q) {
        fprintf(stderr, "failed to allocate emulator\n");
        return 1;
    }
    if (no_dcache) quanta_set_dcache(q, 0); /* opt out of the decode cache (M25a) */
    if (jit_on && quanta_set_jit(q, 1) != QUANTA_OK) {
        /* Refuse rather than silently interpret: the user asked for speed. */
        fprintf(stderr, "--jit is not supported on this host (needs x86-64)\n");
        quanta_destroy(q);
        return 2;
    }

    /* Configure SMP before loading, so the boot handoff brings up every hart. */
    if (quanta_set_harts(q, nharts) != QUANTA_OK) {
        fprintf(stderr, "failed to configure %d harts\n", nharts);
        quanta_destroy(q);
        return 2;
    }

    /* Advertise the virtio-net device in the boot device tree only when a network
     * backend is attached, so a guest OS discovers it exactly when it is usable;
     * without --netdev the node is omitted and existing boots are unperturbed.
     * Set before loading, since the DTB is built during the boot handoff. */
    quanta_set_netdev_advertised(q, netdev != NULL);
    /* Likewise advertise the virtio-blk device only when a --disk image is given,
     * so a distribution kernel finds its root disk (M24); set before loading. */
    quanta_set_disk_advertised(q, diskpath != NULL);

    /* --bios selects the firmware boot path: an M-mode firmware (OpenSBI) that
     * hands off to an S-mode OS image (--kernel), the way a real machine boots. */
    if (bios && !kernel) {
        fprintf(stderr, "--bios needs --kernel (the S-mode OS image to boot)\n");
        quanta_destroy(q);
        return 2;
    }
    if (kernel && !bios) {
        fprintf(stderr, "--kernel needs --bios (the M-mode firmware to boot it)\n");
        quanta_destroy(q);
        return 2;
    }
    if (initrd && !bios) {
        fprintf(stderr, "--initrd needs --bios/--kernel (it is the booted kernel's "
                "rootfs)\n");
        quanta_destroy(q);
        return 2;
    }
    /* --restore reconstructs the whole machine from a snapshot file, so it is
     * standalone — no program, firmware, or kernel is loaded alongside it. */
    if (restorepath && (path || bios || kernel)) {
        fprintf(stderr, "--restore is standalone: it cannot be combined with a "
                "program, --bios, or --kernel\n");
        quanta_destroy(q);
        return 2;
    }

    /* Load the program first, since it can fail. --restore rebuilds the machine
     * from a snapshot; otherwise the demo maps a fixed region and copies the
     * hardcoded image, an ELF gets a region sized to its load image, and the
     * loader prints its own diagnostics on failure. */
    int demo = (path == NULL && bios == NULL && restorepath == NULL);
    QuantaStatus st = restorepath
        ? quanta_load_snapshot(q, restorepath)
        : bios
        ? quanta_load_firmware(q, bios, kernel, append, initrd, mem_req)
        : demo
        ? quanta_load_image(q, MEM_BASE, MEM_SIZE,
                            demo_program, sizeof demo_program, MEM_BASE)
        : quanta_load_elf_ex(q, path, mem_req);
    if (st != QUANTA_OK) {
        if (demo)              fprintf(stderr, "failed to load demo program\n");
        else if (restorepath)  fprintf(stderr, "failed to restore snapshot %s\n",
                                       restorepath);
        else if (bios)         fprintf(stderr, "failed to load firmware/kernel\n");
        quanta_destroy(q);
        return 1;
    }

    /* Optionally attach a raw disk image, loaded after the program so the guest
     * region already exists. The virtio-mmio block device DMAs against it. --disk
     * persists guest writes back to the file (M24); --disk-ro is a discard overlay. */
    if (diskpath && quanta_attach_disk_ex(q, diskpath, disk_writable) != QUANTA_OK) {
        fprintf(stderr, "failed to load disk image %s%s\n", diskpath,
                disk_writable ? " (needs read-write permission)" : "");
        quanta_destroy(q);
        return 1;
    }

    /* Optionally attach a network backend to the virtio-net device. --netdev=tap
     * is a raw layer-2 bridge to a host TAP device (the host owns addressing and
     * routing); --netdev=user is the built-in usermode stack — a virtual gateway
     * (10.0.2.0/24) the guest can DHCP against and ping, with outbound UDP/TCP NAT
     * and a DNS relay to the real network, needing no host privileges. Either way
     * the device's transmit path feeds the backend and its receives come back
     * through quanta_net_rx; the run loop services the backend via net_pump. */
    if (netdev && tap_mode) {
        tap_fd = tap_open(tap_ifname);
        if (tap_fd < 0) {
            fprintf(stderr, "failed to open TAP device%s%s: %s\n",
                    tap_ifname ? " " : "", tap_ifname ? tap_ifname : "",
                    strerror(errno));
            fprintf(stderr, "(pre-create a TAP owned by you: "
                    "`sudo ip tuntap add %s mode tap user $USER`, or run with "
                    "CAP_NET_ADMIN)\n", tap_ifname ? tap_ifname : "tap0");
            quanta_destroy(q);
            return 1;
        }
        quanta_net_set_backend(q, tap_backend_tx, &tap_fd);
        pump.user = NULL; pump.q = q; pump.tap_fd = tap_fd;
        netbp = &pump;
    } else if (netdev) {
        ns = netstack_new(net_deliver_to_guest, q);
        if (!ns) {
            fprintf(stderr, "failed to create the network stack\n");
            quanta_destroy(q);
            return 1;
        }
        quanta_net_set_backend(q, net_backend_tx, ns);

        for (int s = 0; s < NS_NAT_MAX; s++) netbk.socks[s].fd = -1;
        netbk.ns       = ns;
        netio.ctx      = &netbk;
        netio.open     = net_io_open;
        netio.send     = net_io_send;
        netio.shutdown = net_io_shutdown;
        netio.close    = net_io_close;
        uint8_t upstream_dns[4];
        resolve_upstream_dns(upstream_dns);
        netstack_set_io(ns, &netio, upstream_dns);
        pump.user = &netbk; pump.q = q; pump.tap_fd = -1;
        netbp = &pump;
    }

    /* The loader set PC to the entry point and sp to the top of the region. */
    uint64_t entry = quanta_pc(q);
    uint64_t sp    = quanta_reg(q, 2);
    int      w     = (quanta_xlen(q) == 64) ? 16 : 8; /* hex digits per XLEN value */

    if (!quiet) {
        printf("Quanta — RV%d emulator\n", quanta_xlen(q) ? quanta_xlen(q) : 32);
        if (demo) {
            printf("No ELF given; running the built-in demo program.\n\n");
        } else if (bios) {
            /* Firmware boot: the M-mode firmware (entry) will hand off to the
             * S-mode OS at 0x80200000; a2 points at the fw_dynamic descriptor. */
            printf("Firmware %s (entry = 0x%0*llx, dtb = 0x%0*llx)\n",
                   bios, w, (unsigned long long)entry,
                   w, (unsigned long long)quanta_dtb_addr(q));
            printf("Kernel:  %s\n", kernel);
            if (diskpath) printf("Disk:    %s%s\n", diskpath,
                                 disk_writable ? "" : " (read-only)");
            printf("\n");
        } else if (restorepath) {
            printf("Restored from snapshot %s (pc = 0x%0*llx, dtb = 0x%0*llx)\n\n",
                   restorepath, w, (unsigned long long)entry,
                   w, (unsigned long long)quanta_dtb_addr(q));
        } else {
            /* The loader hands the guest a device tree per the RISC-V boot
             * contract: a0 = hartid, a1 = dtb (reported here for visibility). */
            printf("Loaded %s (entry = 0x%0*llx, sp = 0x%0*llx, dtb = 0x%0*llx)\n",
                   path, w, (unsigned long long)entry, w, (unsigned long long)sp,
                   w, (unsigned long long)quanta_dtb_addr(q));
            if (diskpath) printf("Disk:  %s%s\n", diskpath,
                                 disk_writable ? "" : " (read-only)");
            printf("\n");
        }
    }

    /* Optional cache model: a pure observability layer in front of memory. It
     * watches data load/store addresses and tallies hits/misses without
     * touching the data, so it never changes what the program computes. */
    if (cache_on && quanta_enable_cache(q, csize, cways, cblock) != QUANTA_OK) {
        quanta_destroy(q);
        return 2;
    }

    /* GDB remote debugging takes over execution: rather than running the guest
     * to completion, hand control to a debugger over TCP, which drives the
     * machine (read/write state, breakpoints, step, continue) through libquanta.
     * This is a self-contained mode, so the trace and pipeline overlays — both
     * tied to the run-to-completion loop — do not apply. */
    if (gdb_on) {
        if (!quiet)
            printf("Starting GDB stub on port %d; waiting for a connection.\n",
                   gdb_port);
        int rc = quanta_gdb_serve(q, gdb_port);
        QuantaHalt gdb_halt = quanta_halt_reason(q);
        int gdb_status = 0;
        if (rc != 0) {
            gdb_status = 1;                       /* could not start the stub */
        } else if (gdb_halt == QUANTA_HALT_EXIT) {
            if (!quiet)
                printf("Program exited with code %u.\n", quanta_exit_code(q));
            gdb_status = (int)(quanta_exit_code(q) & 0xffu);
        } else if (!quiet) {
            printf("GDB session ended.\n");
        }
        quanta_destroy(q);
        netstack_free(ns);
        if (tap_fd >= 0) close(tap_fd);
        return gdb_status;
    }

    /* Optional pipeline timing model: another overlay (it reads the retired
     * instruction stream, never the data), reported alongside the cache. */
    Pipeline pipe;
    if (pipe_on) pipeline_init(&pipe);

    /* If stdin is a terminal, run with it in raw mode so an interactive guest
     * (a booting OS at its shell) gets clean character-at-a-time input; a pipe or
     * file is untouched. Restored right after the run (and, as a backstop, via
     * atexit/signal handlers) so the user's shell is always handed back intact. */
    console_raw_enable();
    if (g_console_raw && !quiet)
        printf("Console: raw mode — Ctrl-A x to quit, Ctrl-A Ctrl-A for a literal Ctrl-A.\n\n");

    /* Any output the program writes via syscalls appears here, mid-run. */
    uint64_t steps = run_until_halt(q, trace, pipe_on ? &pipe : NULL, max_steps, netbp);

    console_restore();
    QuantaHalt halt = quanta_halt_reason(q);

    /* Optional architectural-test signature dump. It runs whatever the halt
     * reason — the test has already populated the region by the time it stops —
     * and is independent of the human-readable reporting below, so it composes
     * with --quiet (the harness wants only the file). The built-in demo has no
     * ELF, hence no signature symbols to locate. */
    int sig_failed = 0;
    if (sigfile) {
        if (demo || restorepath) {
            fprintf(stderr, "--signature needs an ELF program\n");
            sig_failed = 1;
        } else {
            sig_failed = dump_signature(path, q, sigfile) != 0;
        }
    }

    /* Optionally persist the final machine state so a later --restore resumes it
     * (E10). It runs whatever the halt reason — checkpointing a capped run is the
     * point: `quanta --max-steps=N --snapshot=FILE ...` then `quanta --restore=FILE`
     * continues from exactly there. */
    int snap_failed = 0;
    if (snappath) {
        if (quanta_save_snapshot(q, snappath) != QUANTA_OK) {
            fprintf(stderr, "failed to write snapshot to %s\n", snappath);
            snap_failed = 1;
        } else if (!quiet) {
            printf("Snapshot written to %s after %llu instruction(s).\n\n",
                   snappath, (unsigned long long)steps);
        }
    }

    if (!quiet) {
        if (halt == QUANTA_HALT_EXIT) {
            printf("Program exited with code %u after %llu instruction(s).\n\n",
                   quanta_exit_code(q), (unsigned long long)steps);
        } else if (halt != QUANTA_RUN) {
            printf("Halted: %s", quanta_halt_str(halt));
            if (halt == QUANTA_HALT_MEM_FAULT)
                printf(" at 0x%0*llx", w, (unsigned long long)quanta_fault_addr(q));
            printf(" after %llu instruction(s).\n\n", (unsigned long long)steps);
        } else if (g_console_quit) {
            printf("Console quit after %llu instruction(s).\n\n",
                   (unsigned long long)steps);
        } else {
            printf("Stopped at the %llu-instruction safety limit.\n\n",
                   (unsigned long long)steps);
        }
    }

    if (cache_on && !quiet) {
        quanta_cache_report(q, stdout);
        printf("\n");
    }

    if (pipe_on && !quiet) {
        pipeline_report(&pipe, stdout);
        printf("\n");
    }

    if (!quiet) quanta_dump_regs(q, stdout);

    /* The built-in demo has a known answer, so check it as a self-test. A
     * loaded ELF is arbitrary, so we just report its final state. */
    if (demo && !quiet) {
        uint32_t a2 = quanta_reg(q, 12); /* a2 */
        uint32_t a3 = quanta_reg(q, 13); /* a3 */
        printf("\nExpected: a2 = 42, a3 = 32\n");
        printf("Got:      a2 = %u, a3 = %u  -> %s\n",
               a2, a3, (a2 == 42 && a3 == 32) ? "OK" : "MISMATCH");
    }

    /* Propagate the guest's exit status as our own process exit code (like
     * qemu-user or spike): a clean exit returns its code, an abnormal stop
     * (ebreak/trap/limit) returns 1. `make check` relies on this to tell a
     * passing conformance test (exit 0) from a failing one. */
    int status = (halt == QUANTA_HALT_EXIT) ? (int)(quanta_exit_code(q) & 0xffu)
               : g_console_quit ? 0    /* the user asked to quit — a clean stop */
               : 1;
    if (sig_failed && status == 0) status = 1;  /* surface a dump failure */
    if (snap_failed && status == 0) status = 1; /* surface a snapshot-write failure */
    quanta_destroy(q);
    netstack_free(ns);
    if (tap_fd >= 0) close(tap_fd);
    return status;
}
