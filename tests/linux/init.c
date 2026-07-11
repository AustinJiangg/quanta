/*
 * init.c — a freestanding userspace `/init` for Quanta's Linux boot (M18).
 *
 * This is the program a mainline Linux kernel runs as PID 1 once it has mounted
 * the initramfs Quanta hands it (`--initrd`). It uses no C library: every action
 * is a raw RISC-V Linux system call (`ecall` with the number in a7, args in
 * a0..a5, result in a0 — the asm-generic ABI riscv uses). It is the last link in
 * the boot chain — OpenSBI -> Linux -> here — proving the whole stack reaches a
 * real userspace process on Quanta's emulated hardware.
 *
 * It runs a tiny line-oriented shell over the serial console: the kernel's tty
 * line discipline echoes and buffers each line (canonical mode), init reads it,
 * responds, and powers the machine off on `poweroff` (or on console EOF) via the
 * reboot syscall — which Linux turns into an SBI SRST call, halting Quanta.
 *
 * Built static, non-PIE, rv64imac (Quanta has no RV64F/D), no startfiles:
 *   riscv64-linux-gnu-gcc -march=rv64imac -mabi=lp64 -static -no-pie \
 *       -nostdlib -ffreestanding -fno-stack-protector -Os -o init init.c
 * The kernel gives PID 1 a valid stack (with argc/argv/envp/auxv), so _start can
 * be a plain C function; it never returns.
 */

/* asm-generic syscall numbers (arch/riscv shares include/uapi/asm-generic). */
#define SYS_openat  56
#define SYS_close   57
#define SYS_read    63
#define SYS_write   64
#define SYS_mount   40
#define SYS_reboot 142

#define AT_FDCWD   (-100)   /* openat: resolve the path relative to cwd */

/* reboot(2) magics and the power-off command (include/uapi/linux/reboot.h). */
#define REBOOT_MAGIC1    0xfee1deadL
#define REBOOT_MAGIC2    0x28121969L
#define REBOOT_POWEROFF  0x4321fedcL

static long sys3(long n, long a0, long a1, long a2) {
    register long x0 asm("a0") = a0;
    register long x1 asm("a1") = a1;
    register long x2 asm("a2") = a2;
    register long x7 asm("a7") = n;
    asm volatile("ecall" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x7) : "memory");
    return x0;
}

static long sys4(long n, long a0, long a1, long a2, long a3) {
    register long x0 asm("a0") = a0;
    register long x1 asm("a1") = a1;
    register long x2 asm("a2") = a2;
    register long x3 asm("a3") = a3;
    register long x7 asm("a7") = n;
    asm volatile("ecall" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x7) : "memory");
    return x0;
}

static long sys5(long n, long a0, long a1, long a2, long a3, long a4) {
    register long x0 asm("a0") = a0;
    register long x1 asm("a1") = a1;
    register long x2 asm("a2") = a2;
    register long x3 asm("a3") = a3;
    register long x4 asm("a4") = a4;
    register long x7 asm("a7") = n;
    asm volatile("ecall" : "+r"(x0)
                 : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x7) : "memory");
    return x0;
}

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts_(const char *s) { sys3(SYS_write, 1, (long)s, slen(s)); }
static void putn_(const char *s, long n) { sys3(SYS_write, 1, (long)s, n); }

/* Copy /proc/cpuinfo to the console — proof, from userspace, of every hart the
 * kernel brought online (the SMP boot's "done when"). procfs is mounted at start. */
static void cat_cpuinfo(void) {
    long fd = sys4(SYS_openat, AT_FDCWD, (long)"/proc/cpuinfo", 0 /*O_RDONLY*/, 0);
    if (fd < 0) { puts_("cpuinfo: /proc not mounted\n"); return; }
    char b[512];
    long r;
    while ((r = sys3(SYS_read, fd, (long)b, sizeof b)) > 0) putn_(b, r);
    sys3(SYS_close, fd, 0, 0);
}

/* Does the console line `buf[0..n)` (trailing CR/LF stripped) equal `lit`? */
static int line_is(const char *buf, long n, const char *lit) {
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    long i = 0;
    for (; i < n; i++)
        if (lit[i] == '\0' || buf[i] != lit[i]) return 0;
    return lit[i] == '\0';
}

void __attribute__((noreturn)) _start(void) {
    sys5(SYS_mount, (long)"proc", (long)"/proc", (long)"proc", 0, 0); /* for cpuinfo */

    puts_("\n[ quanta ] userspace reached: PID 1 running, no libc, raw syscalls.\n");
    puts_("commands: help, echo <text>, cpuinfo, poweroff (or Ctrl-A x to quit)\n");

    char buf[256];
    for (;;) {
        puts_("quanta$ ");
        long n = sys3(SYS_read, 0, (long)buf, sizeof buf);
        if (n <= 0) break;                    /* console EOF/error -> power off */
        if (line_is(buf, n, "poweroff") || line_is(buf, n, "exit") ||
            line_is(buf, n, "halt"))
            break;
        if (line_is(buf, n, "help")) {
            puts_("builtins: help, echo <text>, cpuinfo, poweroff\n");
            continue;
        }
        if (line_is(buf, n, "cpuinfo")) {     /* list the harts the kernel onlined */
            cat_cpuinfo();
            continue;
        }
        if (n > 5 && buf[0] == 'e' && buf[1] == 'c' && buf[2] == 'h' &&
            buf[3] == 'o' && buf[4] == ' ') {
            putn_(buf + 5, n - 5);            /* echo the rest of the line back */
            continue;
        }
        puts_("you said: ");
        putn_(buf, n);
    }

    puts_("[ quanta ] powering off.\n");
    sys4(SYS_reboot, REBOOT_MAGIC1, REBOOT_MAGIC2, REBOOT_POWEROFF, 0);
    for (;;) { }                              /* unreachable: SRST halts the hart */
}
