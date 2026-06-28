#include "quanta.h"
#include "disasm.h"
#include "pipeline.h"
#include "gdbstub.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/*
 * Quanta driver — a thin client over libquanta.
 *
 * Usage:
 *   quanta [--version] [--trace] [--quiet] [--cache[=SIZE:WAYS:BLOCK]]
 *          [--pipeline] [--gdb[=PORT]] [--signature=FILE] [program.elf]
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

/* Execute one instruction and narrate it to stderr: the PC, the raw word, its
 * disassembly, and any register the step changed (with the new value), plus the
 * redirect target when control does not simply fall through. Trace goes to
 * stderr so it stays separate from whatever the guest prints via the write
 * syscall on stdout. "What changed" is recovered by diffing a register snapshot
 * taken around the step through the public accessors, so the engine core stays
 * untouched. */
static void trace_step(Quanta *q) {
    uint32_t pc   = quanta_pc(q);
    uint32_t inst = quanta_read_u32(q, pc, NULL);
    uint32_t before[32];
    for (int i = 0; i < 32; i++) before[i] = quanta_reg(q, i);

    char text[80];
    disasm(pc, inst, text, sizeof text);

    quanta_step(q);

    fprintf(stderr, "%08x:  %08x  %-24s", pc, inst, text);
    for (int i = 1; i < 32; i++) /* x0 is hardwired; it can never change */
        if (quanta_reg(q, i) != before[i])
            fprintf(stderr, " %s=0x%08x", quanta_reg_name(i), quanta_reg(q, i));
    if (quanta_pc(q) != pc + 4) /* taken branch, jump, or trap */
        fprintf(stderr, " ->0x%08x", quanta_pc(q));
    fprintf(stderr, "\n");
}

/* Step until the program halts, or a safety limit is hit so a buggy program can
 * never spin forever. With trace set, narrate each instruction to stderr; with a
 * pipeline, feed it each retired instruction. Returns the instruction count. */
static uint64_t run_until_halt(Quanta *q, int trace, Pipeline *pipe) {
    uint64_t steps = 0;
    /* A generous runaway guard: high enough to let real workloads (loops over
     * arrays, deep call chains) run to completion, low enough that a program
     * that never halts still stops in about a second instead of hanging. */
    const uint64_t max_steps = 100ull * 1000 * 1000;
    while (quanta_halt_reason(q) == QUANTA_RUN && steps < max_steps) {
        uint32_t pc   = quanta_pc(q);
        uint32_t inst = pipe ? quanta_read_u32(q, pc, NULL) : 0;
        if (trace) trace_step(q);
        else       quanta_step(q);
        /* Feed the timing model the retired instruction and whether control
         * left the fall-through path (a taken branch or a jump). */
        if (pipe) pipeline_observe(pipe, inst, quanta_pc(q) != pc + 4);
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

int main(int argc, char **argv) {
    int trace = 0;
    int quiet = 0;
    int cache_on = 0;
    int pipe_on = 0;
    int gdb_on = 0;
    int gdb_port = 1234;            /* the conventional gdbserver/qemu port */
    uint32_t csize = 1024, cways = 2, cblock = 32; /* default L1 geometry */
    const char *path = NULL;
    const char *sigfile = NULL; /* --signature=FILE: arch-test signature dump */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("quanta %s\n", quanta_version());
            return 0;
        }
        if (strcmp(argv[i], "--trace") == 0) {
            trace = 1;
        } else if (strcmp(argv[i], "--quiet") == 0) {
            quiet = 1;
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
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            fprintf(stderr, "usage: %s [--version] [--trace] [--quiet] "
                    "[--cache[=SIZE:WAYS:BLOCK]] [--pipeline] [--gdb[=PORT]] "
                    "[--signature=FILE] [program.elf]\n", argv[0]);
            return 2;
        } else if (path == NULL) {
            path = argv[i];
        } else {
            fprintf(stderr, "usage: %s [--version] [--trace] [--quiet] "
                    "[--cache[=SIZE:WAYS:BLOCK]] [--pipeline] [--gdb[=PORT]] "
                    "[--signature=FILE] [program.elf]\n", argv[0]);
            return 2;
        }
    }

    Quanta *q = quanta_create();
    if (!q) {
        fprintf(stderr, "failed to allocate emulator\n");
        return 1;
    }

    /* Load the program first, since it can fail. The demo maps a fixed region
     * and copies the hardcoded image; an ELF gets a region sized to its load
     * image, and the loader prints its own diagnostics on failure. */
    int demo = (path == NULL);
    QuantaStatus st = demo
        ? quanta_load_image(q, MEM_BASE, MEM_SIZE,
                            demo_program, sizeof demo_program, MEM_BASE)
        : quanta_load_elf(q, path);
    if (st != QUANTA_OK) {
        if (demo) fprintf(stderr, "failed to load demo program\n");
        quanta_destroy(q);
        return 1;
    }

    /* The loader set PC to the entry point and sp to the top of the region. */
    uint32_t entry = quanta_pc(q);
    uint32_t sp    = quanta_reg(q, 2);

    if (!quiet) {
        printf("Quanta — RV32I emulator\n");
        if (demo) {
            printf("No ELF given; running the built-in demo program.\n\n");
        } else {
            /* The loader hands the guest a device tree per the RISC-V boot
             * contract: a0 = hartid, a1 = dtb (reported here for visibility). */
            printf("Loaded %s (entry = 0x%08x, sp = 0x%08x, dtb = 0x%08x)\n\n",
                   path, entry, sp, quanta_dtb_addr(q));
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
        return gdb_status;
    }

    /* Optional pipeline timing model: another overlay (it reads the retired
     * instruction stream, never the data), reported alongside the cache. */
    Pipeline pipe;
    if (pipe_on) pipeline_init(&pipe);

    /* Any output the program writes via syscalls appears here, mid-run. */
    uint64_t steps = run_until_halt(q, trace, pipe_on ? &pipe : NULL);

    QuantaHalt halt = quanta_halt_reason(q);

    /* Optional architectural-test signature dump. It runs whatever the halt
     * reason — the test has already populated the region by the time it stops —
     * and is independent of the human-readable reporting below, so it composes
     * with --quiet (the harness wants only the file). The built-in demo has no
     * ELF, hence no signature symbols to locate. */
    int sig_failed = 0;
    if (sigfile) {
        if (demo) {
            fprintf(stderr, "--signature needs an ELF program, not the demo\n");
            sig_failed = 1;
        } else {
            sig_failed = dump_signature(path, q, sigfile) != 0;
        }
    }

    if (!quiet) {
        if (halt == QUANTA_HALT_EXIT) {
            printf("Program exited with code %u after %llu instruction(s).\n\n",
                   quanta_exit_code(q), (unsigned long long)steps);
        } else if (halt != QUANTA_RUN) {
            printf("Halted: %s", quanta_halt_str(halt));
            if (halt == QUANTA_HALT_MEM_FAULT)
                printf(" at 0x%08x", quanta_fault_addr(q));
            printf(" after %llu instruction(s).\n\n", (unsigned long long)steps);
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
    int status = (halt == QUANTA_HALT_EXIT)
        ? (int)(quanta_exit_code(q) & 0xffu) : 1;
    if (sig_failed && status == 0) status = 1; /* surface a dump failure */
    quanta_destroy(q);
    return status;
}
