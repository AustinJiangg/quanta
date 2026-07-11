#include "sbi.h"

#include "quanta.h"   /* QUANTA_VERSION_* for the implementation version */
#include "device.h"   /* Platform: the sibling-hart array HSM reaches */
#include "decode.h"   /* CSR_SATP / CSR_MSTATUS the HSM start state resets */
#include "mmu.h"      /* mmu_flush when a started hart's satp is reset */

#include <stdio.h>

/*
 * SBI implementation — see sbi.h for the why. This is the firmware half of the
 * S-mode `ecall` path: a register-based RPC into Quanta. Each extension is a
 * small handler that reads a0-a5 and writes (a0, a1).
 */

/* ABI register indices the SBI convention uses. */
enum { REG_A0 = 10, REG_A1 = 11, REG_A2 = 12, REG_A6 = 16, REG_A7 = 17 };

/* Standard SBI return error codes (negative; a0 carries them). */
enum {
    SBI_SUCCESS               =  0,
    SBI_ERR_FAILED            = -1,
    SBI_ERR_NOT_SUPPORTED     = -2,
    SBI_ERR_INVALID_PARAM     = -3,
    SBI_ERR_DENIED            = -4,
    SBI_ERR_INVALID_ADDRESS   = -5,
    SBI_ERR_ALREADY_AVAILABLE = -6
};

/* Extension IDs (EIDs). The legacy console/shutdown extensions predate the
 * function-id convention and ignore a6; the rest are the modern multi-function
 * extensions whose names spell ASCII ("TIME", "SRST", "HSM"). */
enum {
    EXT_SET_TIMER = 0x00,        /* legacy: set the timer (a0:a1 = 64-bit) */
    EXT_PUTCHAR   = 0x01,        /* legacy: console putchar (a0 = byte)    */
    EXT_GETCHAR   = 0x02,        /* legacy: console getchar                */
    EXT_SHUTDOWN  = 0x08,        /* legacy: shut the system down           */
    EXT_BASE      = 0x10,        /* base: version/probe                    */
    EXT_TIME      = 0x54494D45,  /* "TIME": timer                          */
    EXT_SRST      = 0x53525354,  /* "SRST": system reset                   */
    EXT_HSM       = 0x48534D     /* "HSM":  hart state management          */
};

/* What sbi_get_spec_version reports: major in bits [30:24], minor in [23:0].
 * We implement a v1.0-shaped subset. */
#define SBI_SPEC_VERSION (1u << 24)

/* Implementation id we report. The SBI registry assigns small ids to known
 * firmwares (OpenSBI, RustSBI, ...); Quanta is not registered, so this is purely
 * informational — a kernel that does not recognise it just skips its quirks. */
#define SBI_IMPL_ID 0x9u

/* Write the (error, value) return pair into a0/a1. SBI returns are XLEN-wide
 * longs, so the (negative) error is sign-extended: an RV64 caller reading a0 as a
 * long must see e.g. -3, not 0x00000000fffffffd. The value fields we return are
 * small non-negatives, so zero-extending value is equivalent. */
static void sbi_return(CPU *cpu, int32_t error, uint32_t value) {
    reg_write(cpu, REG_A0, (uint64_t)(int64_t)error);
    reg_write(cpu, REG_A1, value);
}

/* Stop the machine cleanly with exit status 0 — the SBI shutdown/reset effect. */
static void sbi_halt(CPU *cpu) {
    cpu->exit_code   = 0;
    cpu->halt_reason = HALT_EXIT;
    cpu->halted      = 1;
}

/* Set the next timer deadline (low/high words of a 64-bit value). The firmware
 * (cpu.c) watches it and, when mtime reaches it, delivers a supervisor timer
 * interrupt (STIP) to the OS — and clears any previous one here. */
static void sbi_set_timer(CPU *cpu, uint32_t lo, uint32_t hi) {
    cpu_arm_supervisor_timer(cpu, ((uint64_t)hi << 32) | lo);
}

/* Is `eid` an extension this firmware implements? Backs base probe_extension. */
static int sbi_probe(uint32_t eid) {
    switch (eid) {
        case EXT_SET_TIMER: case EXT_PUTCHAR: case EXT_GETCHAR:
        case EXT_SHUTDOWN:  case EXT_BASE:    case EXT_TIME:
        case EXT_SRST:      case EXT_HSM:
            return 1;
        default:
            return 0;
    }
}

/* The Base extension: discovery the OS uses to learn what the firmware offers. */
static void sbi_base(CPU *cpu, uint32_t fid, uint32_t arg0) {
    switch (fid) {
        case 0: sbi_return(cpu, SBI_SUCCESS, SBI_SPEC_VERSION); return; /* spec version */
        case 1: sbi_return(cpu, SBI_SUCCESS, SBI_IMPL_ID);      return; /* impl id      */
        case 2: sbi_return(cpu, SBI_SUCCESS,                            /* impl version */
                    (QUANTA_VERSION_MAJOR << 16) |
                    (QUANTA_VERSION_MINOR << 8)  | QUANTA_VERSION_PATCH); return;
        case 3: sbi_return(cpu, SBI_SUCCESS, (uint32_t)sbi_probe(arg0)); return; /* probe */
        case 4: case 5: case 6: /* mvendorid / marchid / mimpid: none modelled */
            sbi_return(cpu, SBI_SUCCESS, 0); return;
        default:
            sbi_return(cpu, SBI_ERR_NOT_SUPPORTED, 0); return;
    }
}

/* Resolve a hartid to its CPU, or NULL if out of range. On a uniprocessor with
 * no platform hart array, only the calling hart's own id is valid. */
static CPU *hsm_hart(CPU *cpu, uint32_t hartid) {
    Platform *plat = cpu->mem->plat;
    if (!plat || !plat->harts) return (hartid == cpu->hartid) ? cpu : NULL;
    if (hartid >= (uint32_t)plat->nharts) return NULL;
    return &plat->harts[hartid];
}

/* Bring a hart up in S-mode at `pc` with a0 = hartid, a1 = opaque, satp = 0
 * (Bare) and supervisor interrupts off — the HSM spec's start/resume state.
 * Used by both hart_start (a sibling) and non-retentive hart_suspend (self). */
static void hsm_enter(CPU *t, uint64_t pc, uint64_t opaque) {
    t->pc   = pc;
    t->priv = PRIV_S;
    reg_write(t, REG_A0, t->hartid);
    reg_write(t, REG_A1, opaque);
    t->csr[CSR_SATP]     = 0;
    t->csr[CSR_MSTATUS] &= ~(uint64_t)0x2; /* clear sstatus.SIE */
    t->reserve_valid = 0;
    mmu_flush(t);
    t->hsm_state   = HSM_STARTED;
    t->halt_reason = HALT_NONE;
    t->halted      = 0;
}

/* HSM — hart state management (M22): the firmware side of SMP hart bring-up. A
 * stopped hart's round-robin slot is a no-op until hart_start wakes it; hart_stop
 * parks the calling hart; hart_get_status reports the state; hart_suspend idles
 * (WFI is a nop here, so a retentive suspend resumes immediately). Reached only
 * when Quanta is the firmware — a from-scratch SMP kernel on the direct boot;
 * under OpenSBI this path is bypassed and OpenSBI provides HSM itself. */
static void sbi_hsm(CPU *cpu, uint32_t fid) {
    switch (fid) {
    case 0: { /* hart_start(hartid, start_addr, opaque) */
        uint32_t hartid = (uint32_t)reg_read(cpu, REG_A0);
        uint64_t start  = reg_read(cpu, REG_A1);
        uint64_t opaque = reg_read(cpu, REG_A2);
        CPU *t = hsm_hart(cpu, hartid);
        if (!t)                          { sbi_return(cpu, SBI_ERR_INVALID_PARAM, 0);     return; }
        if (t->hsm_state == HSM_STARTED) { sbi_return(cpu, SBI_ERR_ALREADY_AVAILABLE, 0); return; }
        if (t->hsm_state != HSM_STOPPED) { sbi_return(cpu, SBI_ERR_FAILED, 0);            return; }
        hsm_enter(t, start, opaque);
        sbi_return(cpu, SBI_SUCCESS, 0);
        return;
    }
    case 1: /* hart_stop(): the calling hart parks; no return on success */
        cpu->hsm_state   = HSM_STOPPED;
        cpu->halt_reason = HALT_EXIT;   /* parked, not a fault */
        cpu->halted      = 1;
        return;
    case 2: { /* hart_get_status(hartid) */
        CPU *t = hsm_hart(cpu, (uint32_t)reg_read(cpu, REG_A0));
        if (!t) sbi_return(cpu, SBI_ERR_INVALID_PARAM, 0);
        else    sbi_return(cpu, SBI_SUCCESS, (uint32_t)t->hsm_state);
        return;
    }
    case 3: { /* hart_suspend(suspend_type, resume_addr, opaque) */
        uint32_t type = (uint32_t)reg_read(cpu, REG_A0);
        if (type == 0x00000000u)        /* retentive: resume here, state retained */
            sbi_return(cpu, SBI_SUCCESS, 0);
        else if (type == 0x80000000u)   /* non-retentive: resume at resume_addr */
            hsm_enter(cpu, reg_read(cpu, REG_A1), reg_read(cpu, REG_A2));
        else
            sbi_return(cpu, SBI_ERR_INVALID_PARAM, 0);
        return;
    }
    default:
        sbi_return(cpu, SBI_ERR_NOT_SUPPORTED, 0);
        return;
    }
}

void sbi_call(CPU *cpu) {
    uint32_t eid = reg_read(cpu, REG_A7);
    uint32_t fid = reg_read(cpu, REG_A6);
    uint32_t a0  = reg_read(cpu, REG_A0);
    uint32_t a1  = reg_read(cpu, REG_A1);

    switch (eid) {
        case EXT_PUTCHAR:                 /* legacy console putchar */
            putchar((int)(a0 & 0xff));
            fflush(stdout);
            reg_write(cpu, REG_A0, 0);    /* legacy: only a0, 0 on success */
            return;

        case EXT_GETCHAR:                 /* legacy console getchar: no input here */
            reg_write(cpu, REG_A0, (uint32_t)-1);
            return;

        case EXT_SET_TIMER:               /* legacy set_timer (a0=lo, a1=hi) */
            sbi_set_timer(cpu, a0, a1);
            reg_write(cpu, REG_A0, 0);
            return;

        case EXT_SHUTDOWN:                /* legacy shutdown */
            sbi_halt(cpu);
            return;

        case EXT_BASE:
            sbi_base(cpu, fid, a0);
            return;

        case EXT_TIME:                    /* TIME: set_timer (fid 0) */
            if (fid == 0) { sbi_set_timer(cpu, a0, a1); sbi_return(cpu, SBI_SUCCESS, 0); }
            else          sbi_return(cpu, SBI_ERR_NOT_SUPPORTED, 0);
            return;

        case EXT_SRST:                    /* SRST: system_reset (fid 0) */
            if (fid == 0) {
                /* reset_type in a0: 0 shutdown, 1 cold reboot, 2 warm reboot.
                 * We cannot reboot, so every valid type stops the machine. */
                if (a0 <= 2) sbi_halt(cpu);
                else         sbi_return(cpu, SBI_ERR_INVALID_PARAM, 0);
            } else {
                sbi_return(cpu, SBI_ERR_NOT_SUPPORTED, 0);
            }
            return;

        case EXT_HSM:                     /* HSM: hart state management */
            sbi_hsm(cpu, fid);
            return;

        default:
            sbi_return(cpu, SBI_ERR_NOT_SUPPORTED, 0);
            return;
    }
}
