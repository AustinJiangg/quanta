/*
 * M16 — a small RV32 teaching kernel that boots on Quanta and runs a userspace
 * process. It is the integration milestone for everything M8-M15 built: the
 * privilege model, Sv32 paging, the trap path, the platform devices, and the
 * SBI firmware interface all have to work together for a single program to reach
 * its shell — which is exactly where bugs the per-feature tests miss show up.
 *
 * The boot story, end to end:
 *
 *   1. Quanta's loader enters the image in Machine mode (a0 = hart id, a1 = a
 *      flattened device tree) and boot.S mret's into kmain() in Supervisor mode,
 *      having delegated user traps to S-mode and left mtvec 0 so the kernel's own
 *      SBI ecalls reach Quanta's firmware (M9, M14, M15).
 *   2. kmain() reads the RAM range from the device tree (M14), then hands out the
 *      spare RAM above its image as physical pages (the --memory knob makes that
 *      space exist).
 *   3. It builds an Sv32 address space (M12): the kernel and the MMIO devices are
 *      identity-mapped with megapages; a user code page and stack page are mapped
 *      at low virtual addresses with the U bit. Turning on satp makes paging live.
 *   4. It installs an stvec trap handler, enables SUM so it can touch user memory,
 *      arms a periodic deadline through SBI set_timer (M15), and sret's into the
 *      user program (M9).
 *   5. The user prints via the write syscall and spins; supervisor timer
 *      interrupts preempt it (M13/M15 timer relay); when it calls exit the kernel
 *      shuts the machine down through SBI system_reset (M15).
 *
 * Console output is written straight to the mapped 16550 UART (M13), proving MMIO
 * through Sv32 translation. Built freestanding (-nostdlib) for rv32imac.
 */

#include <stdint.h>
#include <stddef.h>

/* ---- the qemu 'virt' platform map Quanta implements (M13) ---- */
#define UART_THR     0x10000000u   /* 16550 transmit-holding register */
#define UART_BASE    0x10000000u
#define CLINT_BASE   0x02000000u
#define CLINT_MTIME  0x0200bff8u   /* CLINT mtime, low 32 bits */

/* ---- SBI extension ids (M15) ---- */
#define SBI_EXT_TIME 0x54494D45u   /* "TIME": set_timer (fid 0)        */
#define SBI_EXT_SRST 0x53525354u   /* "SRST": system_reset (fid 0)     */

/* ---- Sv32 page-table entry bits (M12) ---- */
#define PTE_V 0x001u
#define PTE_R 0x002u
#define PTE_W 0x004u
#define PTE_X 0x008u
#define PTE_U 0x010u
#define PTE_A 0x040u
#define PTE_D 0x080u
#define PAGE       4096u
#define MEGAPAGE   (4u * 1024 * 1024)
#define SATP_SV32  0x80000000u

/* ---- syscall numbers (RISC-V Linux/newlib, as Quanta's SEE uses) ---- */
#define SYS_write 64
#define SYS_exit  93

/* ---- user address space ---- */
#define USER_CODE_VA   0x00010000u
#define USER_STACK_VA  0x00011000u
#define USER_STACK_TOP (USER_STACK_VA + PAGE)

/* ---- preemption demo: how often, and how many times, to fire the timer ---- */
#define TIMER_DELTA  3000u         /* mtime ticks between deadlines */
#define TICKS_WANTED 3             /* preempt the user this many times */

/* Symbols from the linker script and boot.S. */
extern char __user_start[], __user_end[];
extern char trap_stack_top[];
extern void trap_entry(void);
extern void enter_user(uint32_t entry, uint32_t user_sp) __attribute__((noreturn));

/* ------------------------------------------------------------- CSR access */
static inline uint32_t r_scause(void) {
    uint32_t v; __asm__ volatile ("csrr %0, scause" : "=r"(v)); return v;
}
static inline uint32_t r_sepc(void) {
    uint32_t v; __asm__ volatile ("csrr %0, sepc" : "=r"(v)); return v;
}
static inline uint32_t r_stval(void) {
    uint32_t v; __asm__ volatile ("csrr %0, stval" : "=r"(v)); return v;
}
static inline void w_sepc(uint32_t v)    { __asm__ volatile ("csrw sepc, %0"    :: "r"(v)); }
static inline void w_stvec(uint32_t v)   { __asm__ volatile ("csrw stvec, %0"   :: "r"(v)); }
static inline void w_sscratch(uint32_t v){ __asm__ volatile ("csrw sscratch, %0":: "r"(v)); }
static inline void set_sstatus(uint32_t b){ __asm__ volatile ("csrs sstatus, %0" :: "r"(b)); }
static inline void set_sie(uint32_t b)   { __asm__ volatile ("csrs sie, %0"     :: "r"(b)); }
static inline void w_satp(uint32_t v) {
    __asm__ volatile ("sfence.vma");
    __asm__ volatile ("csrw satp, %0" :: "r"(v));
    __asm__ volatile ("sfence.vma");
}

/* --------------------------------------------------------- tiny freestanding libc */
/* Standard names so any compiler-emitted memcpy/memset libcall resolves here. */
void *memset(void *dst, int c, size_t n) {
    uint8_t *p = dst;
    while (n--) *p++ = (uint8_t)c;
    return dst;
}
void *memcpy(void *dst, const void *src, size_t n) {
    uint8_t *d = dst; const uint8_t *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

/* ----------------------------------------------------------------- console */
static void kputc(char c)        { *(volatile uint8_t *)UART_THR = (uint8_t)c; }
static void kputs(const char *s) { while (*s) kputc(*s++); }
static void kputhex(uint32_t v) {
    kputs("0x");
    for (int i = 28; i >= 0; i -= 4) kputc("0123456789abcdef"[(v >> i) & 0xf]);
}
static void kputdec(uint32_t v) {
    char buf[10];
    int n = 0;
    if (v == 0) { kputc('0'); return; }
    while (v) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) kputc(buf[--n]);
}

/* --------------------------------------------------------------------- SBI */
static uint32_t sbi_ecall(uint32_t eid, uint32_t fid, uint32_t a0, uint32_t a1) {
    register uint32_t r0 __asm__("a0") = a0;
    register uint32_t r1 __asm__("a1") = a1;
    register uint32_t r6 __asm__("a6") = fid;
    register uint32_t r7 __asm__("a7") = eid;
    __asm__ volatile ("ecall" : "+r"(r0), "+r"(r1) : "r"(r6), "r"(r7) : "memory");
    return r0;
}
static void sbi_set_timer(uint64_t deadline) {
    sbi_ecall(SBI_EXT_TIME, 0, (uint32_t)deadline, (uint32_t)(deadline >> 32));
}
static void sbi_shutdown(void) {
    sbi_ecall(SBI_EXT_SRST, 0, 0 /*shutdown*/, 0 /*no reason*/);
}

/* Arm the next preemption: a deadline TIMER_DELTA mtime ticks from now. The
 * firmware (Quanta) relays the machine timer to us as a supervisor timer
 * interrupt once mtime passes it (M15). */
static void arm_timer(void) {
    uint32_t now = *(volatile uint32_t *)CLINT_MTIME;
    sbi_set_timer((uint64_t)now + TIMER_DELTA);
}

/* -------------------------------------------------- device tree (FDT) reader */
static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
           (uint32_t)p[2] <<  8 | (uint32_t)p[3];
}
static int name_is(const char *s, const char *prefix) {
    while (*prefix) { if (*s++ != *prefix++) return 0; }
    return 1;
}

/* Walk the flattened device tree at `dtb` and recover the /memory node's reg =
 * <addr_hi addr_lo size_hi size_lo>, reporting the low cells. Returns 1 on
 * success. A from-scratch reader, the C cousin of tests/test_dtb.S, proving the
 * kernel discovers its RAM from the tree rather than assuming a base. */
static int dtb_memory(uint32_t dtb, uint32_t *base, uint32_t *size) {
    const uint8_t *b = (const uint8_t *)dtb;
    if (be32(b) != 0xd00dfeedu) return 0;                /* FDT magic */
    const uint8_t *st  = b + be32(b + 8);                /* structure block */
    const uint8_t *str = b + be32(b + 12);               /* strings block   */
    int in_memory = 0;
    for (;;) {
        uint32_t tok = be32(st);
        st += 4;
        if (tok == 9) break;                             /* FDT_END */
        if (tok == 1) {                                  /* FDT_BEGIN_NODE */
            const char *name = (const char *)st;
            in_memory = name_is(name, "memory@");
            uint32_t len = 0;
            while (name[len]) len++;
            st += (len + 1 + 3) & ~3u;                   /* name + NUL, padded */
        } else if (tok == 3) {                           /* FDT_PROP */
            uint32_t plen = be32(st);
            uint32_t noff = be32(st + 4);
            const char *pname = (const char *)(str + noff);
            if (in_memory && name_is(pname, "reg") && pname[3] == '\0' &&
                plen >= 16) {
                *base = be32(st + 8 + 4);                /* reg addr, low cell */
                *size = be32(st + 8 + 12);               /* reg size, low cell */
                return 1;
            }
            st += 8 + ((plen + 3) & ~3u);                /* len + nameoff + val */
        } else if (tok != 2 && tok != 4) {               /* not END_NODE / NOP */
            return 0;                                    /* malformed */
        }
    }
    return 0;
}

/* ------------------------------------------------ physical page allocator */
static uint32_t pool, pool_end;

static uint32_t alloc_page(void) {
    if (pool + PAGE > pool_end) {
        kputs("quanta-os: out of physical memory\n");
        sbi_shutdown();
    }
    uint32_t p = pool;
    pool += PAGE;
    memset((void *)p, 0, PAGE);
    return p;
}

/* ----------------------------------------------------- Sv32 page-table build */
static uint32_t *root;     /* the active root page table (identity-mapped) */

static void map_mega(uint32_t va, uint32_t pa, uint32_t flags) {
    root[va >> 22] = ((pa >> 12) << 10) | flags;         /* 4 MiB superpage */
}
static void map_page(uint32_t *leaf, uint32_t va, uint32_t pa, uint32_t flags) {
    leaf[(va >> 12) & 0x3ffu] = ((pa >> 12) << 10) | flags;
}

/* --------------------------------------------------------------- trap handler */
static int ticks;

/* Called from trap_entry with a pointer to the saved register frame (x0..x31,
 * x0 slot unused). scause/sepc/stval are read from CSRs. */
void trap_handler(uint32_t *regs) {
    uint32_t cause = r_scause();

    if (cause & 0x80000000u) {                           /* an interrupt */
        if ((cause & 0xffu) == 5u) {                     /* supervisor timer */
            ticks++;
            kputs("quanta-os: timer tick\n");
            if (ticks < TICKS_WANTED) arm_timer();       /* re-arm for the next */
            else sbi_set_timer(~(uint64_t)0);            /* disarm: never again */
        }
        return;                                          /* resume; do not bump sepc */
    }

    if (cause == 8u) {                                   /* ecall from U-mode */
        uint32_t num = regs[17];                         /* a7 */
        if (num == SYS_write) {
            uint32_t buf = regs[11], len = regs[12];     /* a1, a2 (a0 = fd) */
            /* SUM lets the kernel read the user's buffer through its own VAs. */
            for (uint32_t i = 0; i < len; i++)
                kputc(((volatile char *)buf)[i]);
            regs[10] = len;                              /* bytes written -> a0 */
        } else if (num == SYS_exit) {
            kputs("quanta-os: user exited (code ");
            kputdec(regs[10]);
            kputs("), shutting down\n");
            sbi_shutdown();                              /* halts the machine */
        } else {
            kputs("quanta-os: unknown syscall ");
            kputdec(num);
            kputc('\n');
            regs[10] = (uint32_t)-1;
        }
        w_sepc(r_sepc() + 4);                            /* step past the ecall */
        return;
    }

    /* Anything else is a fault we do not handle: report it and trap to the SEE
     * (an S-mode ebreak is not delegated, so Quanta stops with a non-zero exit,
     * which make check reads as failure). */
    kputs("quanta-os: fatal trap scause=");
    kputhex(cause);
    kputs(" sepc=");
    kputhex(r_sepc());
    kputs(" stval=");
    kputhex(r_stval());
    kputc('\n');
    __asm__ volatile ("ebreak");
}

/* --------------------------------------------------------------------- kmain */
void kmain(uint32_t hartid, uint32_t dtb) {
    extern char _end[];

    /* 1. Discover RAM from the device tree (fall back to Quanta's load base). */
    uint32_t mem_base = 0x7ffff000u, mem_size = 0x10000u;
    dtb_memory(dtb, &mem_base, &mem_size);
    uint32_t mem_end = mem_base + mem_size;

    /* 2. The free pool is everything between the kernel image and the device
     *    tree (which sits at the top of RAM); page-align both ends. */
    pool     = ((uint32_t)_end + PAGE - 1) & ~(PAGE - 1);
    pool_end = (dtb ? dtb : mem_end) & ~(PAGE - 1);
    if (pool_end > mem_end) pool_end = mem_end & ~(PAGE - 1);

    /* 3. Build the Sv32 address space. */
    root = (uint32_t *)alloc_page();
    /* identity-map all of RAM as supervisor RWX, one megapage at a time */
    for (uint32_t pa = mem_base & ~(MEGAPAGE - 1); pa < mem_end; pa += MEGAPAGE)
        map_mega(pa, pa, PTE_V | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);
    /* identity-map the CLINT and UART MMIO windows, supervisor RW (no execute) */
    map_mega(CLINT_BASE, CLINT_BASE, PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);
    map_mega(UART_BASE,  UART_BASE,  PTE_V | PTE_R | PTE_W | PTE_A | PTE_D);

    /* the user gets its own low-VA mappings: a leaf table under root[0], a code
     * page (copied from the embedded blob) and a stack page, both U-accessible */
    uint32_t *uleaf = (uint32_t *)alloc_page();
    root[0] = (((uint32_t)uleaf >> 12) << 10) | PTE_V;   /* non-leaf -> uleaf */
    uint32_t ucode  = alloc_page();
    uint32_t ustack = alloc_page();
    memcpy((void *)ucode, __user_start, (size_t)(__user_end - __user_start));
    map_page(uleaf, USER_CODE_VA,  ucode,
             PTE_V | PTE_R | PTE_W | PTE_X | PTE_U | PTE_A | PTE_D);
    map_page(uleaf, USER_STACK_VA, ustack,
             PTE_V | PTE_R | PTE_W | PTE_U | PTE_A | PTE_D);

    /* 4. Install the trap vector and its stack, allow S-mode to read user pages,
     *    enable the supervisor timer source, then turn paging on. */
    w_stvec((uint32_t)trap_entry);
    w_sscratch((uint32_t)trap_stack_top);
    set_sstatus(1u << 18);          /* sstatus.SUM */
    set_sie(1u << 5);               /* sie.STIE: arm the supervisor timer source */
    w_satp(SATP_SV32 | ((uint32_t)root >> 12));

    /* 5. Boot banner — now through the mapped UART, i.e. MMIO via Sv32. */
    kputs("quanta-os: booting on hart ");
    kputdec(hartid);
    kputc('\n');
    kputs("quanta-os: RAM ");
    kputdec(mem_size >> 10);
    kputs(" KiB at ");
    kputhex(mem_base);
    kputc('\n');
    kputs("quanta-os: sv32 paging enabled\n");
    kputs("quanta-os: launching user process at va ");
    kputhex(USER_CODE_VA);
    kputc('\n');

    /* 6. Arm the first preemption and drop into user mode. The supervisor timer
     *    fires in U-mode regardless of sstatus.SIE; control returns only via a
     *    trap, so enter_user does not return. */
    arm_timer();
    enter_user(USER_CODE_VA, USER_STACK_TOP);
}
