/* Expose the POSIX sockets/select API under -std=c11 (which otherwise hides it).
 * This is the project's one piece of OS-specific code; everything else is ISO C
 * stdlib-only, so the feature-test macro lives here rather than in the build. */
#define _DEFAULT_SOURCE 1 /* NOLINT(bugprone-reserved-identifier): feature-test macro */

#include "gdbstub.h"
#include "quanta.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

/*
 * GDB remote serial protocol (RSP) server. See gdbstub.h for the embedding API.
 *
 * The wire format is simple: each packet is "$<payload>#<2-hex-checksum>", and
 * each side acks a good packet with '+' (a bad one with '-', asking for resend).
 * The stub answers the standard query/register/memory/step/continue packets and
 * serves a target description so gdb learns the RV32 register layout. Breakpoints
 * are tracked here (the Z0/z0 packets) and enforced by the continue loop, so the
 * guest's own memory is never patched.
 *
 * Everything below drives the machine only through the public quanta.h surface.
 */

#define GDB_BUFSZ        8192
#define GDB_MAX_PAYLOAD  ((GDB_BUFSZ - 16) / 2)  /* bytes per m/M packet */
#define GDB_MAX_BP       128
#define GDB_MAX_CP       33      /* reverse-exec checkpoints: step-0 + up to 32 recent */
#define GDB_CP_INTERVAL  4096    /* steps between checkpoints once reverse exec is used */

/* GDB signal numbers used in stop replies. */
#define SIG_INT   2   /* SIGINT  — Ctrl-C from the debugger          */
#define SIG_ILL   4   /* SIGILL  — illegal/unimplemented instruction */
#define SIG_TRAP  5   /* SIGTRAP — step done / breakpoint            */
#define SIG_SEGV 11   /* SIGSEGV — access outside mapped memory      */

/* The target description (x0..x31 then pc, regnums 0..32) is built per
 * connection by build_target_xml() below, so each register's bitsize tracks the
 * loaded program's width (RV32 vs RV64). The register order matches the g/G
 * packet layout. */

/* A reverse-execution checkpoint: a full machine snapshot tagged with the step it
 * was taken at (E10). */
typedef struct {
    uint64_t        step;
    QuantaSnapshot *snap;
} GdbCheckpoint;

typedef struct {
    int      fd;                  /* the connected debugger socket */
    Quanta  *q;                   /* the machine being debugged */
    int      feat_swbreak;        /* gdb negotiated the swbreak stop reason */
    int      dead;               /* connection lost — end the session */
    int      want_quit;          /* debugger detached/killed — end the session */
    uint64_t bp[GDB_MAX_BP];     /* software-breakpoint addresses */
    int      bp_used[GDB_MAX_BP];
    /* Reverse execution (E10): a running step count as a stable time coordinate,
     * plus a ring of machine snapshots so any earlier step can be reached by
     * restoring the nearest checkpoint and replaying forward. */
    uint64_t      nsteps;             /* instructions executed since attach */
    int           reverse_used;       /* a reverse op was issued (enables checkpointing) */
    uint64_t      last_cp_step;       /* step of the most recent checkpoint */
    GdbCheckpoint cp[GDB_MAX_CP];     /* cp[0] pinned to step 0; the rest roll */
    int           ncp;
} GdbStub;

/* do_continue() outcomes. */
enum { CONT_HALTED, CONT_BREAK, CONT_INTR, CONT_DEAD };

/* --- hex helpers --- */

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static char nibble_hex(unsigned v) {
    static const char digits[] = "0123456789abcdef";
    return digits[v & 0xfu];
}

/* Bytes per XLEN-wide register in the RSP packets: 4 for RV32, 8 for RV64. */
static int reg_bytes(const GdbStub *s) {
    return quanta_xlen(s->q) == 64 ? 8 : 4;
}

/* Encode the low `nbytes` of `v` as 2*nbytes hex chars, low byte first (the RSP
 * register/memory byte order). Writes exactly 2*nbytes chars to out. */
static void val_to_hex_le(uint64_t v, int nbytes, char *out) {
    for (size_t b = 0; b < (size_t)nbytes; b++) {
        unsigned byte = (v >> (8u * b)) & 0xffu;
        out[2 * b]     = nibble_hex(byte >> 4);
        out[2 * b + 1] = nibble_hex(byte);
    }
}

/* Decode 2*nbytes hex chars (low byte first) into *out. Returns -1 on a
 * non-hex digit. */
static int hex_to_val_le(const char *s, int nbytes, uint64_t *out) {
    uint64_t v = 0;
    for (size_t b = 0; b < (size_t)nbytes; b++) {
        int hi = hex_nibble((unsigned char)s[2 * b]);
        int lo = hex_nibble((unsigned char)s[2 * b + 1]);
        if (hi < 0 || lo < 0) return -1;
        v |= (uint64_t)(((unsigned)hi << 4) | (unsigned)lo) << (8u * b);
    }
    *out = v;
    return 0;
}

/* --- low-level socket I/O --- */

static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)n;
    }
    return 0;
}

/* Read one byte; returns 0..255, or -1 on EOF/error. */
static int read_byte(int fd) {
    for (;;) {
        unsigned char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n == 1) return (int)c;
        if (n == 0) return -1;
        if (errno == EINTR) continue;
        return -1;
    }
}

/* Frame `data` as a packet, send it, and wait for the '+' ack (resending on a
 * '-' up to a few times). Sets stub->dead and returns -1 if the link drops. */
static int send_packet(GdbStub *s, const char *data) {
    size_t len = strlen(data);
    unsigned sum = 0;
    for (size_t i = 0; i < len; i++) sum += (unsigned char)data[i];
    char tail[3];
    tail[0] = '#';
    tail[1] = nibble_hex(sum >> 4);
    tail[2] = nibble_hex(sum);
    char dollar = '$';

    for (int tries = 0; tries < 5; tries++) {
        if (write_all(s->fd, &dollar, 1) < 0 ||
            write_all(s->fd, data, len) < 0 ||
            write_all(s->fd, tail, 3) < 0) {
            s->dead = 1;
            return -1;
        }
        int ack = read_byte(s->fd);
        if (ack < 0) { s->dead = 1; return -1; }
        if (ack == '+') return 0;
        /* '-' or anything unexpected: resend */
    }
    s->dead = 1;
    return -1;
}

/* Read one packet's payload into buf (NUL-terminated), acking it. Returns the
 * payload length, 1 with buf[0]==0x03 for a bare Ctrl-C interrupt byte, or -1 on
 * EOF/error. A checksum mismatch is NAKed and the next (resent) packet read. */
static int read_packet(GdbStub *s, char *buf, size_t bufsz) {
    for (;;) {
        int c;
        do {
            c = read_byte(s->fd);
            if (c < 0) return -1;
            if (c == 0x03) { buf[0] = (char)0x03; return 1; }
        } while (c != '$');

        size_t len = 0;
        unsigned sum = 0;
        for (;;) {
            c = read_byte(s->fd);
            if (c < 0) return -1;
            if (c == '#') break;
            if (len + 1 >= bufsz) return -1;  /* overlong packet */
            buf[len++] = (char)c;
            sum += (unsigned)c;
        }
        int h1 = hex_nibble(read_byte(s->fd));
        int h2 = hex_nibble(read_byte(s->fd));
        if (h1 < 0 || h2 < 0) return -1;
        buf[len] = '\0';

        unsigned want = ((unsigned)h1 << 4) | (unsigned)h2;
        char ack = ((sum & 0xffu) == want) ? '+' : '-';
        if (write_all(s->fd, &ack, 1) < 0) return -1;
        if (ack == '+') return (int)len;
        /* mismatch: loop and read the client's resend */
    }
}

/* Non-blocking peek for a Ctrl-C (0x03) the debugger sends to halt a running
 * target. Returns 1 if seen, -1 on EOF, 0 otherwise. */
static int interrupt_pending(GdbStub *s) {
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(s->fd, &rfds);
    struct timeval tv = { 0, 0 };
    int r = select(s->fd + 1, &rfds, NULL, NULL, &tv);
    if (r <= 0) return 0;
    int c = read_byte(s->fd);
    if (c < 0) return -1;
    return (c == 0x03) ? 1 : 0;
}

/* --- breakpoints (stub-managed, address compared in the continue loop) --- */

static int bp_match(const GdbStub *s, uint64_t addr) {
    for (int i = 0; i < GDB_MAX_BP; i++)
        if (s->bp_used[i] && s->bp[i] == addr) return 1;
    return 0;
}

static int bp_add(GdbStub *s, uint64_t addr) {
    if (bp_match(s, addr)) return 1;  /* already set is a success */
    for (int i = 0; i < GDB_MAX_BP; i++) {
        if (!s->bp_used[i]) {
            s->bp_used[i] = 1;
            s->bp[i] = addr;
            return 1;
        }
    }
    return 0;  /* table full */
}

static void bp_remove(GdbStub *s, uint64_t addr) {
    for (int i = 0; i < GDB_MAX_BP; i++)
        if (s->bp_used[i] && s->bp[i] == addr) s->bp_used[i] = 0;
}

/* --- reverse execution: checkpoints + deterministic replay (E10) --- */

/* Step one instruction, tracking the running step count. Because the scheduler is
 * deterministic and a gdb session feeds no external input, this count is a stable
 * coordinate: any step can be revisited by restoring a checkpoint at or before it
 * and replaying forward. A no-op once the machine has halted. */
static QuantaHalt raw_step(GdbStub *s) {
    QuantaHalt h = quanta_halt_reason(s->q);
    if (h != QUANTA_RUN) return h;
    h = quanta_step(s->q);
    s->nsteps++;
    return h;
}

/* Checkpoint the current machine at step `step`. cp[0] is pinned to the attach
 * point (step 0); when the ring is full a new checkpoint evicts the oldest recent
 * one, so the retained set is step 0 plus the most recent GDB_MAX_CP-1
 * checkpoints — keeping replays short near the current position. Silently skips on
 * out-of-memory (fewer checkpoints only means longer replays, never wrong ones). */
static void cp_add(GdbStub *s, uint64_t step) {
    QuantaSnapshot *snap = quanta_snapshot(s->q);
    if (snap == NULL) return;
    int slot;
    if (s->ncp < GDB_MAX_CP) {
        slot = s->ncp++;
    } else {
        slot = 1;                        /* never evict cp[0] (step 0) */
        for (int i = 2; i < s->ncp; i++)
            if (s->cp[i].step < s->cp[slot].step) slot = i;
        quanta_snapshot_free(s->cp[slot].snap);
    }
    s->cp[slot].step = step;
    s->cp[slot].snap = snap;
    s->last_cp_step = step;
}

/* Step forward one instruction and, once reverse execution has been used at least
 * once, take a periodic checkpoint so later reverse operations replay from nearby.
 * Plain (forward-only) sessions never checkpoint, so they pay nothing. */
static QuantaHalt stub_step(GdbStub *s) {
    QuantaHalt h = raw_step(s);
    if (s->reverse_used && h == QUANTA_RUN &&
        s->nsteps - s->last_cp_step >= GDB_CP_INTERVAL)
        cp_add(s, s->nsteps);
    return h;
}

/* Move the machine to exactly step `target` (<= a previously reached step):
 * restore the nearest checkpoint at or before it, then replay forward. Exact,
 * since execution is deterministic. Does nothing if no checkpoint exists (the
 * attach snapshot failed), leaving the machine where it was. */
static void goto_step(GdbStub *s, uint64_t target) {
    int best = -1;
    for (int i = 0; i < s->ncp; i++)
        if (s->cp[i].step <= target &&
            (best < 0 || s->cp[i].step > s->cp[best].step))
            best = i;
    if (best < 0 || quanta_restore(s->q, s->cp[best].snap) != QUANTA_OK)
        return;
    s->nsteps = s->cp[best].step;
    while (s->nsteps < target)
        if (raw_step(s) != QUANTA_RUN) break; /* deterministic: won't halt early */
}

/* --- stop replies and execution --- */

/* Build the stop-reply for the current machine state. When the machine is still
 * runnable (a completed step, a breakpoint, or an interrupt) it reports `sig`;
 * once halted, the halt reason maps to W (clean exit) or the matching signal. */
static void stop_reply(GdbStub *s, char *out, size_t outsz, int sig,
                       int swbreak_hit) {
    QuantaHalt h = quanta_halt_reason(s->q);
    if (h == QUANTA_HALT_EXIT) {
        snprintf(out, outsz, "W%02x",
                 (unsigned)(quanta_exit_code(s->q) & 0xffu));
        return;
    }
    if (h != QUANTA_RUN) {
        unsigned hs;
        switch (h) {
            case QUANTA_HALT_MEM_FAULT:      hs = SIG_SEGV; break;
            case QUANTA_HALT_ILLEGAL_INSN:
            case QUANTA_HALT_UNIMP_SYSTEM:
            case QUANTA_HALT_UNKNOWN_SYSCALL: hs = SIG_ILL; break;
            default:                          hs = SIG_TRAP; break; /* incl. ebreak */
        }
        snprintf(out, outsz, "S%02x", hs);
        return;
    }
    if (swbreak_hit && s->feat_swbreak)
        snprintf(out, outsz, "T%02xswbreak:;", (unsigned)sig);
    else
        snprintf(out, outsz, "S%02x", (unsigned)sig);
}

static int send_stop(GdbStub *s, int sig, int swbreak_hit) {
    char rep[32];
    stop_reply(s, rep, sizeof rep, sig, swbreak_hit);
    return send_packet(s, rep);
}

/* Run until the guest halts, the PC reaches a breakpoint, or the debugger sends
 * a Ctrl-C. One instruction is always executed first, so "continue" off a
 * breakpoint makes progress instead of re-triggering it immediately. */
static int do_continue(GdbStub *s) {
    QuantaHalt h = stub_step(s);
    unsigned long guard = 0;
    while (h == QUANTA_RUN) {
        if (bp_match(s, quanta_pc(s->q))) return CONT_BREAK;
        if (((++guard) & 0xffffUL) == 0) {
            int ip = interrupt_pending(s);
            if (ip < 0) return CONT_DEAD;
            if (ip > 0) return CONT_INTR;
        }
        h = stub_step(s);
    }
    return CONT_HALTED;
}

static int continue_and_reply(GdbStub *s) {
    switch (do_continue(s)) {
        case CONT_BREAK: return send_stop(s, SIG_TRAP, 1);
        case CONT_INTR:  return send_stop(s, SIG_INT, 0);
        case CONT_DEAD:  s->dead = 1; return -1;
        default:         return send_stop(s, SIG_TRAP, 0); /* CONT_HALTED */
    }
}

/* bs — reverse single-step: go back one instruction by restoring the nearest
 * earlier checkpoint and replaying to just before the current step. */
static int handle_reverse_step(GdbStub *s) {
    s->reverse_used = 1;
    if (s->nsteps > 0) goto_step(s, s->nsteps - 1);
    return send_stop(s, SIG_TRAP, 0);
}

/* bc — reverse continue: stop at the most recent breakpoint strictly before the
 * current position, or rewind to the start of history if there is none. Replays
 * from the earliest checkpoint to find the last step whose PC is a breakpoint. */
static int handle_reverse_continue(GdbStub *s) {
    s->reverse_used = 1;
    uint64_t cur = s->nsteps;
    if (cur == 0) return send_stop(s, SIG_TRAP, 0);
    goto_step(s, 0);
    uint64_t hit = 0;
    int found = 0;
    while (s->nsteps < cur) {
        if (bp_match(s, quanta_pc(s->q))) { hit = s->nsteps; found = 1; }
        if (raw_step(s) != QUANTA_RUN) break;
    }
    goto_step(s, found ? hit : 0);
    return send_stop(s, SIG_TRAP, 0);
}

/* --- packet handlers --- */

static int handle_read_regs(GdbStub *s) {
    int nb = reg_bytes(s), nc = nb * 2;
    char out[33 * 16 + 1];
    for (int i = 0; i < 32; i++)
        val_to_hex_le(quanta_reg(s->q, i), nb, out + (size_t)i * nc);
    val_to_hex_le(quanta_pc(s->q), nb, out + (size_t)32 * nc);
    out[(size_t)33 * nc] = '\0';
    return send_packet(s, out);
}

static int handle_write_regs(GdbStub *s, const char *args) {
    int nb = reg_bytes(s), nc = nb * 2;
    if (strlen(args) < (size_t)33 * nc) return send_packet(s, "E22");
    for (int i = 0; i < 32; i++) {
        uint64_t v;
        if (hex_to_val_le(args + (size_t)i * nc, nb, &v) != 0)
            return send_packet(s, "E22");
        quanta_set_reg(s->q, i, v);
    }
    uint64_t pc;
    if (hex_to_val_le(args + (size_t)32 * nc, nb, &pc) != 0)
        return send_packet(s, "E22");
    quanta_set_pc(s->q, pc);
    return send_packet(s, "OK");
}

static int handle_read_reg(GdbStub *s, const char *args) {
    int nb = reg_bytes(s);
    unsigned long n = strtoul(args, NULL, 16);
    char out[17];
    if (n < 32)        val_to_hex_le(quanta_reg(s->q, (int)n), nb, out);
    else if (n == 32)  val_to_hex_le(quanta_pc(s->q), nb, out);
    else               return send_packet(s, "E01");
    out[(size_t)nb * 2] = '\0';
    return send_packet(s, out);
}

static int handle_write_reg(GdbStub *s, const char *args) {
    int nb = reg_bytes(s);
    char *p;
    unsigned long n = strtoul(args, &p, 16);
    if (*p != '=') return send_packet(s, "E22");
    uint64_t v;
    if (hex_to_val_le(p + 1, nb, &v) != 0) return send_packet(s, "E22");
    if (n < 32)        quanta_set_reg(s->q, (int)n, v);
    else if (n == 32)  quanta_set_pc(s->q, v);
    else               return send_packet(s, "E01");
    return send_packet(s, "OK");
}

static int handle_read_mem(GdbStub *s, const char *args) {
    char *p;
    unsigned long long addr = strtoull(args, &p, 16);
    if (*p != ',') return send_packet(s, "E22");
    unsigned long len = strtoul(p + 1, &p, 16);
    if (len == 0) return send_packet(s, "");
    if (len > GDB_MAX_PAYLOAD) len = GDB_MAX_PAYLOAD;

    unsigned char tmp[GDB_MAX_PAYLOAD];
    if (quanta_mem_read(s->q, addr, tmp, len) != QUANTA_OK)
        return send_packet(s, "E0e");

    char out[GDB_BUFSZ];
    for (unsigned long i = 0; i < len; i++) {
        out[2 * i]     = nibble_hex((unsigned)tmp[i] >> 4);
        out[2 * i + 1] = nibble_hex((unsigned)tmp[i]);
    }
    out[2 * len] = '\0';
    return send_packet(s, out);
}

static int handle_write_mem(GdbStub *s, const char *args) {
    char *p;
    unsigned long long addr = strtoull(args, &p, 16);
    if (*p != ',') return send_packet(s, "E22");
    unsigned long len = strtoul(p + 1, &p, 16);
    if (*p != ':') return send_packet(s, "E22");
    p++;
    if (len > GDB_MAX_PAYLOAD) return send_packet(s, "E22");

    unsigned char tmp[GDB_MAX_PAYLOAD];
    for (unsigned long i = 0; i < len; i++) {
        int hi = hex_nibble((unsigned char)p[2 * i]);
        int lo = hex_nibble((unsigned char)p[2 * i + 1]);
        if (hi < 0 || lo < 0) return send_packet(s, "E22");
        tmp[i] = (unsigned char)((hi << 4) | lo);
    }
    if (len && quanta_mem_write(s->q, addr, tmp, len) != QUANTA_OK)
        return send_packet(s, "E0e");
    return send_packet(s, "OK");
}

/* Z/z: set (set==1) or clear a breakpoint. type 0 (sw) and 1 (hw) are both
 * handled as address watchpoints in the continue loop; 2..4 (watchpoints) are
 * declined with an empty reply. */
static int handle_bp(GdbStub *s, const char *args, int set) {
    char *p;
    unsigned long type = strtoul(args, &p, 16);
    if (*p != ',') return send_packet(s, "E22");
    unsigned long long addr = strtoull(p + 1, &p, 16);
    if (type > 1) return send_packet(s, "");
    if (set)
        return send_packet(s, bp_add(s, addr) ? "OK" : "E22");
    bp_remove(s, addr);
    return send_packet(s, "OK");
}

/* Build the target description for the connected machine's width into `buf`,
 * returning its length. Each register is 32-bit on RV32, 64-bit on RV64; the
 * order (x0..x31 then pc) matches the g/G packets. */
static size_t build_target_xml(const GdbStub *s, char *buf, size_t n) {
    static const char *const names[33] = {
        "zero","ra","sp","gp","tp","t0","t1","t2","fp","s1",
        "a0","a1","a2","a3","a4","a5","a6","a7","s2","s3",
        "s4","s5","s6","s7","s8","s9","s10","s11","t3","t4","t5","t6","pc"
    };
    static const char *const types[33] = {
        "int","code_ptr","data_ptr","data_ptr","data_ptr","int","int","int","data_ptr","int",
        "int","int","int","int","int","int","int","int","int","int",
        "int","int","int","int","int","int","int","int","int","int","int","int","code_ptr"
    };
    int bits = (quanta_xlen(s->q) == 64) ? 64 : 32;
    size_t len = 0;
    len += (size_t)snprintf(buf + len, n - len,
        "<?xml version='1.0'?>\n"
        "<!DOCTYPE target SYSTEM 'gdb-target.dtd'>\n"
        "<target version='1.0'>\n"
        "  <architecture>riscv:rv%d</architecture>\n"
        "  <feature name='org.gnu.gdb.riscv.cpu'>\n", bits);
    for (int i = 0; i < 33 && len < n; i++)
        len += (size_t)snprintf(buf + len, n - len,
            "    <reg name='%s' bitsize='%d' type='%s'%s/>\n",
            names[i], bits, types[i], i == 0 ? " regnum='0'" : "");
    if (len < n)
        len += (size_t)snprintf(buf + len, n - len, "  </feature>\n</target>\n");
    return len < n ? len : n; /* clamp; the description fits well under n */
}

/* qXfer:features:read:<annex>:<off>,<len> — serve the target description. */
static int handle_qxfer(GdbStub *s, const char *args) {
    const char *colon = strchr(args, ':');
    if (colon == NULL) return send_packet(s, "E00");
    size_t annexlen = (size_t)(colon - args);
    if (annexlen != strlen("target.xml") ||
        strncmp(args, "target.xml", annexlen) != 0)
        return send_packet(s, "E00");

    char *p;
    unsigned long off = strtoul(colon + 1, &p, 16);
    if (*p != ',') return send_packet(s, "E00");
    unsigned long length = strtoul(p + 1, &p, 16);

    char xml[4096];
    size_t total = build_target_xml(s, xml, sizeof xml);
    if (off >= total) return send_packet(s, "l");
    size_t avail = total - (size_t)off;
    size_t chunk = ((size_t)length < avail) ? (size_t)length : avail;
    if (chunk > (size_t)(GDB_BUFSZ - 4)) chunk = (size_t)(GDB_BUFSZ - 4);

    char out[GDB_BUFSZ];
    out[0] = ((size_t)off + chunk >= total) ? 'l' : 'm';
    memcpy(out + 1, xml + off, chunk);
    out[1 + chunk] = '\0';
    return send_packet(s, out);
}

static int handle_query(GdbStub *s, const char *args) {
    if (strncmp(args, "Supported", 9) == 0) {
        char rep[160];
        snprintf(rep, sizeof rep,
                 "PacketSize=%lx;qXfer:features:read+;swbreak+;hwbreak+;"
                 "vContSupported+;ReverseStep+;ReverseContinue+",
                 (unsigned long)(GDB_BUFSZ - 16));
        return send_packet(s, rep);
    }
    if (strncmp(args, "Xfer:features:read:", 19) == 0)
        return handle_qxfer(s, args + 19);
    if (strcmp(args, "C") == 0)             return send_packet(s, "QC1");
    if (strcmp(args, "fThreadInfo") == 0)   return send_packet(s, "m1");
    if (strcmp(args, "sThreadInfo") == 0)   return send_packet(s, "l");
    if (strcmp(args, "Attached") == 0 ||
        strncmp(args, "Attached:", 9) == 0) return send_packet(s, "1");
    if (strncmp(args, "Symbol", 6) == 0)    return send_packet(s, "OK");
    return send_packet(s, "");  /* unknown query: empty (unsupported) */
}

static int handle_continue(GdbStub *s, const char *args, char cmd) {
    if (cmd == 'c' && args[0] != '\0') {
        char *p;
        unsigned long addr = strtoul(args, &p, 16);
        if (p != args) quanta_set_pc(s->q, (uint32_t)addr);
    }
    return continue_and_reply(s);
}

static int handle_step(GdbStub *s, const char *args, char cmd) {
    if (cmd == 's' && args[0] != '\0') {
        char *p;
        unsigned long addr = strtoul(args, &p, 16);
        if (p != args) quanta_set_pc(s->q, (uint32_t)addr);
    }
    stub_step(s);
    return send_stop(s, SIG_TRAP, 0);
}

static int handle_v(GdbStub *s, const char *args) {
    if (strncmp(args, "Cont?", 5) == 0)
        return send_packet(s, "vCont;c;C;s;S");
    if (strncmp(args, "Cont", 4) == 0) {
        /* A step action (s/S) anywhere wins on our single-thread machine;
         * otherwise continue (c/C). */
        if (strstr(args, ";s") != NULL || strstr(args, ";S") != NULL) {
            stub_step(s);
            return send_stop(s, SIG_TRAP, 0);
        }
        return continue_and_reply(s);
    }
    if (strncmp(args, "Kill", 4) == 0) {
        s->want_quit = 1;
        return send_packet(s, "OK");
    }
    return send_packet(s, "");  /* incl. vMustReplyEmpty and unknown v packets */
}

/* --- server loop --- */

int quanta_gdb_serve(Quanta *q, int port) {
    if (q == NULL || port <= 0 || port > 65535) return -1;

    int lsock = socket(AF_INET, SOCK_STREAM, 0);
    if (lsock < 0) { perror("quanta gdb: socket"); return -1; }

    int one = 1;
    (void)setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &one,
                     (socklen_t)sizeof one);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (bind(lsock, (struct sockaddr *)&addr, (socklen_t)sizeof addr) < 0) {
        perror("quanta gdb: bind");
        close(lsock);
        return -1;
    }
    if (listen(lsock, 1) < 0) {
        perror("quanta gdb: listen");
        close(lsock);
        return -1;
    }

    fprintf(stderr,
            "quanta: GDB stub listening on 127.0.0.1:%d — connect with:\n"
            "  (gdb) target remote :%d\n", port, port);

    int cfd = accept(lsock, NULL, NULL);
    close(lsock);  /* a single debugger connection */
    if (cfd < 0) { perror("quanta gdb: accept"); return -1; }
    int nodelay = 1;
    (void)setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
                     (socklen_t)sizeof nodelay);

    GdbStub stub;
    memset(&stub, 0, sizeof stub);
    stub.fd = cfd;
    stub.q = q;
    stub.feat_swbreak = 1;
    cp_add(&stub, 0);  /* step-0 checkpoint, so reverse execution can rewind to the start */

    /* Zero-filled so any read past a matched packet prefix is a defined 0, not
     * uninitialised memory (also keeps the static analyzer happy). */
    char *buf = calloc(1, GDB_BUFSZ);
    if (buf == NULL) { close(cfd); return -1; }

    while (!stub.dead && !stub.want_quit) {
        int n = read_packet(&stub, buf, GDB_BUFSZ);
        if (n < 0) break;
        if (n >= 1 && buf[0] == (char)0x03) {  /* Ctrl-C while already stopped */
            (void)send_stop(&stub, SIG_INT, 0);
            continue;
        }
        if (n == 0) { (void)send_packet(&stub, ""); continue; }

        char cmd = buf[0];
        const char *args = buf + 1;
        switch (cmd) {
            case '?': (void)send_stop(&stub, SIG_TRAP, 0); break;
            case 'g': (void)handle_read_regs(&stub); break;
            case 'G': (void)handle_write_regs(&stub, args); break;
            case 'p': (void)handle_read_reg(&stub, args); break;
            case 'P': (void)handle_write_reg(&stub, args); break;
            case 'm': (void)handle_read_mem(&stub, args); break;
            case 'M': (void)handle_write_mem(&stub, args); break;
            case 'X': (void)send_packet(&stub, ""); break;  /* force M fallback */
            case 'c':
            case 'C': (void)handle_continue(&stub, args, cmd); break;
            case 's':
            case 'S': (void)handle_step(&stub, args, cmd); break;
            case 'Z': (void)handle_bp(&stub, args, 1); break;
            case 'z': (void)handle_bp(&stub, args, 0); break;
            case 'b':  /* reverse execution: bs (reverse-step), bc (reverse-continue) */
                if (args[0] == 's')      (void)handle_reverse_step(&stub);
                else if (args[0] == 'c') (void)handle_reverse_continue(&stub);
                else                     (void)send_packet(&stub, "");
                break;
            case 'H': (void)send_packet(&stub, "OK"); break;  /* set thread: ok */
            case 'q': (void)handle_query(&stub, args); break;
            case 'Q': (void)send_packet(&stub, ""); break;    /* no Q* settings */
            case 'v': (void)handle_v(&stub, args); break;
            case 'D': (void)send_packet(&stub, "OK"); stub.want_quit = 1; break;
            case 'k': stub.want_quit = 1; break;              /* kill: no reply */
            case 'T': (void)send_packet(&stub, "OK"); break;  /* thread alive */
            default:  (void)send_packet(&stub, ""); break;    /* unsupported */
        }
    }

    for (int i = 0; i < stub.ncp; i++)
        quanta_snapshot_free(stub.cp[i].snap);
    free(buf);
    close(cfd);
    return 0;
}
