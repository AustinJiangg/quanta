#ifndef QUANTA_CPU_H
#define QUANTA_CPU_H

#include <stdint.h>
#include "memory.h"

/*
 * RV32/RV64 CPU state.
 *
 * The architectural state of a hart is small: 32 general-purpose XLEN-wide
 * registers (32 or 64 bits, selected at load time by cpu->xlen) plus a program
 * counter. Everything is stored in 64-bit fields; in RV32 mode a register holds
 * the sign-extension of its 32-bit value (the Spike convention), so the upper
 * half is always a copy of bit 31. With the Zicsr extension a sparse
 * file of control/status registers joins them — configuration and counters
 * read and written only by the CSR instructions, never by load/store. The
 * privileged architecture (M9) adds a current privilege level: the hart runs
 * in Machine, Supervisor, or User mode, and a trap switches between them.
 * Everything the processor does is a function that maps one such state to the
 * next.
 *
 * Register x0 is hardwired to zero in RISC-V: reads always return 0 and
 * writes are discarded. We enforce that in reg_write() rather than trusting
 * every instruction to respect it.
 */

/* Privilege levels, encoded as the architecture numbers them (these values are
 * also what mstatus.MPP/SPP and the CSR-address privilege field hold). There is
 * no level 2. */
enum {
    PRIV_U = 0,  /* User       */
    PRIV_S = 1,  /* Supervisor */
    PRIV_M = 3   /* Machine    */
};

/* mstatus bit fields: the trap-handling state mret/sret and traps stack and
 * pop, plus the bits paging (M12) consults — MPRV redirects loads/stores to
 * MPP's translation, SUM lets S-mode touch U-pages, MXR makes X-only pages
 * readable. Shared with the MMU, which is why they live in the header. */
enum {
    MSTATUS_SIE  = 1u << 1,
    MSTATUS_MIE  = 1u << 3,
    MSTATUS_SPIE = 1u << 5,
    MSTATUS_MPIE = 1u << 7,
    MSTATUS_SPP  = 1u << 8,
    MSTATUS_MPP  = 3u << 11,
    MSTATUS_MPRV = 1u << 17,
    MSTATUS_SUM  = 1u << 18,
    MSTATUS_MXR  = 1u << 19,
    MSTATUS_MPP_SHIFT = 11,
    /* Floating-point unit state (M20). FS != Off marks the F/D registers live;
     * a float write sets it Dirty. Quanta does not trap when FS is Off (a
     * deliberate leniency — see cpu.c), but tracks the state for a full-system
     * guest's context switch. SD is the top XLEN bit summarising a dirty FS. */
    MSTATUS_FS       = 3u << 13,
    MSTATUS_FS_SHIFT = 13
};

/* Synchronous exception causes (mcause/scause with the interrupt bit clear).
 * The ecall causes are contiguous from U so ECALL_U + priv selects the right
 * one (priv is 0/1/3, and there is deliberately no cause 10); 12/13/15 are the
 * Sv32 page faults. Shared with the MMU. */
enum {
    CAUSE_INSN_MISALIGNED  = 0,
    CAUSE_INSN_ACCESS      = 1,
    CAUSE_ILLEGAL_INSN     = 2,
    CAUSE_BREAKPOINT       = 3,
    CAUSE_LOAD_MISALIGNED  = 4,
    CAUSE_LOAD_ACCESS      = 5,
    CAUSE_STORE_MISALIGNED = 6,
    CAUSE_STORE_ACCESS     = 7,
    CAUSE_ECALL_U          = 8,
    CAUSE_ECALL_S          = 9,
    CAUSE_ECALL_M          = 11,
    CAUSE_INSN_PAGE_FAULT  = 12,
    CAUSE_LOAD_PAGE_FAULT  = 13,
    CAUSE_STORE_PAGE_FAULT = 15
};

/* The kind of access being translated — selects the permission bit to check and
 * which page-fault cause to raise. */
typedef enum { ACC_FETCH, ACC_LOAD, ACC_STORE } AccessType;

/* A software TLB entry: one cached VA-page -> PA-page translation, tagged by
 * ASID and carrying the leaf PTE's permission bits so per-access permission
 * checks still run on a hit. Flushed by sfence.vma and any satp write. */
#define TLB_ENTRIES 16
typedef struct {
    int      valid;
    uint64_t vpn;       /* va >> 12 */
    uint32_t asid;
    uint64_t ppn;       /* pa >> 12 */
    uint32_t pte_flags; /* leaf PTE low byte: D A G U X W R V */
} TlbEntry;

/*
 * Why the machine stopped. cpu_step() and the syscall layer record one of
 * these in `halt_reason` whenever they set `halted`, so the embedder — the CLI
 * driver, or any program linking the core as a library — can tell a clean exit
 * from a fault. The core never calls exit() on its host; it stops and reports.
 */
typedef enum {
    HALT_NONE = 0,         /* still running */
    HALT_EXIT,             /* clean stop via the exit syscall */
    HALT_EBREAK,           /* ebreak with no debugger attached */
    HALT_ILLEGAL_INSN,     /* illegal or unimplemented instruction */
    HALT_UNIMP_SYSTEM,     /* unimplemented SYSTEM instruction (e.g. mret/wfi) */
    HALT_UNKNOWN_SYSCALL,  /* ecall with a syscall number we do not implement */
    HALT_MEM_FAULT         /* fetch/load/store outside mapped memory */
} HaltReason;

typedef struct CPU {
    uint64_t regs[32];      /* x0..x31; x0 is always zero (sext_xlen in RV32) */
    uint64_t fregs[32];     /* f0..f31 (M20): 64-bit, single-precision NaN-boxed */
    uint64_t pc;            /* program counter */
    int      xlen;          /* register width in bits: 32 (RV32) or 64 (RV64) */
    uint32_t hartid;        /* this hart's id (mhartid; a0 at boot); 0 on a uniprocessor */
    Memory  *mem;           /* attached memory (not owned by the CPU) */
    struct Cache *cache;    /* optional cache model; NULL if off (not owned) */
    int      halted;        /* set when execution should stop */
    HaltReason halt_reason; /* why it stopped (meaningful once halted) */
    uint32_t exit_code;     /* status the exit syscall passed in a0 */
    uint64_t instret;       /* retired-instruction count; backs the counter CSRs */
    uint32_t priv;          /* current privilege: PRIV_U / PRIV_S / PRIV_M */
    int      trapped;       /* set within a step when a trap redirected the PC */
    int      sbi_timer_armed; /* SBI set_timer set a deadline the firmware watches */
    int      reserve_valid; /* RV-A: an LR set a reservation still held */
    uint64_t reserve_addr;  /* RV-A: physical word the reservation covers */
    TlbEntry tlb[TLB_ENTRIES]; /* M12: cached Sv32 translations */
    uint32_t tlb_next;      /* round-robin TLB replacement index */
    uint64_t csr[4096];     /* CSR file: Zicsr counters + the M9 trap registers */
} CPU;

/* Initialise a CPU: zero all registers, set PC to the given entry point, select
 * RV32 (callers move to RV64 by setting cpu->xlen after loading an ELF64), and
 * attach memory. */
void cpu_init(CPU *cpu, Memory *mem, uint64_t entry_pc, int xlen);

/* Read register `i`. Returns 0 for x0. In RV32 the value is sign-extended to 64
 * bits; truncate to uint32_t for the architectural 32-bit value. */
uint64_t reg_read(const CPU *cpu, uint32_t i);

/* Write `value` to register `i`. Writes to x0 are ignored. The value is
 * sign-extended to XLEN (a no-op in RV64), keeping the sext invariant. */
void reg_write(CPU *cpu, uint32_t i, uint64_t value);

/* Fetch, decode, and execute a single instruction at PC. Advances PC (by 4,
 * or to a branch/jump target). On a memory fault it records HALT_MEM_FAULT and
 * stops instead of advancing. */
void cpu_step(CPU *cpu);

/* Arm the supervisor timer on behalf of the SBI (M15 follow-up): set the CLINT
 * comparator to `deadline`, clear any pending supervisor timer interrupt, and
 * mark the deadline for the firmware to watch. When it is reached, the firmware
 * raises the supervisor timer interrupt (STIP) so an S-mode OS can take a tick —
 * the role real firmware plays relaying the machine timer to the supervisor. */
void cpu_arm_supervisor_timer(CPU *cpu, uint64_t deadline);

/* A short human-readable name for a halt reason, for diagnostics. */
const char *halt_reason_str(HaltReason r);

#endif /* QUANTA_CPU_H */
