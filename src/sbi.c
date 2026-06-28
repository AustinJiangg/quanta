#include "sbi.h"

#include "quanta.h"   /* QUANTA_VERSION_* for the implementation version */
#include "device.h"   /* the CLINT, programmed by set_timer */

#include <stdio.h>

/*
 * SBI implementation — see sbi.h for the why. This is the firmware half of the
 * S-mode `ecall` path: a register-based RPC into Quanta. Each extension is a
 * small handler that reads a0-a5 and writes (a0, a1).
 */

/* ABI register indices the SBI convention uses. */
enum { REG_A0 = 10, REG_A1 = 11, REG_A6 = 16, REG_A7 = 17 };

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

/* Write the (error, value) return pair into a0/a1. */
static void sbi_return(CPU *cpu, int32_t error, uint32_t value) {
    reg_write(cpu, REG_A0, (uint32_t)error);
    reg_write(cpu, REG_A1, value);
}

/* Stop the machine cleanly with exit status 0 — the SBI shutdown/reset effect. */
static void sbi_halt(CPU *cpu) {
    cpu->exit_code   = 0;
    cpu->halt_reason = HALT_EXIT;
    cpu->halted      = 1;
}

/* Program the CLINT timer comparator (low/high words of a 64-bit value). Arming
 * it far ahead leaves the machine timer quiet; full supervisor-timer *delivery*
 * (the firmware relaying MTIP to the OS as STIP) is left for a later milestone. */
static void sbi_set_timer(CPU *cpu, uint32_t lo, uint32_t hi) {
    if (cpu->mem->plat)
        cpu->mem->plat->clint.mtimecmp = ((uint64_t)hi << 32) | lo;
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
            if (fid == 2) {               /* hart_get_status(hartid = a0) */
                if (a0 == 0) sbi_return(cpu, SBI_SUCCESS, 0); /* 0 = STARTED */
                else         sbi_return(cpu, SBI_ERR_INVALID_PARAM, 0);
            } else {
                /* start/stop/suspend: a single hart is always running */
                sbi_return(cpu, SBI_ERR_NOT_SUPPORTED, 0);
            }
            return;

        default:
            sbi_return(cpu, SBI_ERR_NOT_SUPPORTED, 0);
            return;
    }
}
