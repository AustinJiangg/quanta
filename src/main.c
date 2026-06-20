#include "cpu.h"
#include "memory.h"
#include "elf.h"
#include "decode.h"
#include "disasm.h"
#include "cache.h"
#include "pipeline.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

/*
 * Quanta driver.
 *
 * Usage:
 *   quanta [program.elf]
 *
 * With a path, Quanta loads that RV32I ELF executable and runs it from its
 * entry point. With no argument, it runs a tiny built-in demo program — a
 * toolchain-free smoke test — so the emulator stays runnable even without the
 * RISC-V cross-toolchain installed.
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
 * syscall on stdout. cpu_step() itself is untouched — "what changed" is
 * recovered by diffing a register snapshot taken around the step. */
static void trace_step(CPU *cpu) {
    uint32_t pc   = cpu->pc;
    uint32_t inst = mem_read32(cpu->mem, pc);
    uint32_t before[32];
    for (int i = 0; i < 32; i++) before[i] = cpu->regs[i];

    char text[80];
    disasm(pc, inst, text, sizeof text);

    cpu_step(cpu);

    fprintf(stderr, "%08x:  %08x  %-24s", pc, inst, text);
    for (int i = 1; i < 32; i++) /* x0 is hardwired; it can never change */
        if (cpu->regs[i] != before[i])
            fprintf(stderr, " %s=0x%08x", reg_abi_name((uint32_t)i), cpu->regs[i]);
    if (cpu->pc != pc + 4) /* taken branch, jump, or trap */
        fprintf(stderr, " ->0x%08x", cpu->pc);
    fprintf(stderr, "\n");
}

/* Step until the program halts, or a safety limit is hit so a buggy program
 * can never spin forever. With trace set, narrate each instruction to stderr.
 * Returns the number of instructions executed. */
static int run_until_halt(CPU *cpu, int trace, Pipeline *pipe) {
    int steps = 0;
    /* A generous runaway guard: high enough to let real workloads (loops over
     * arrays, deep call chains) run to completion, low enough that a program
     * that never halts still stops in about a second instead of hanging. */
    const int max_steps = 100 * 1000 * 1000;
    while (!cpu->halted && steps < max_steps) {
        uint32_t pc   = cpu->pc;
        uint32_t inst = pipe ? mem_read32(cpu->mem, pc) : 0;
        if (trace) trace_step(cpu);
        else       cpu_step(cpu);
        /* Feed the timing model the retired instruction and whether control
         * left the fall-through path (a taken branch or a jump). */
        if (pipe) pipeline_observe(pipe, inst, cpu->pc != pc + 4);
        steps++;
    }
    return steps;
}

int main(int argc, char **argv) {
    int trace = 0;
    int cache_on = 0;
    int pipe_on = 0;
    uint32_t csize = 1024, cways = 2, cblock = 32; /* default L1 geometry */
    const char *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) {
            trace = 1;
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
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "unknown option: %s\n", argv[i]);
            fprintf(stderr, "usage: %s [--trace] [--cache[=SIZE:WAYS:BLOCK]] "
                    "[--pipeline] [program.elf]\n", argv[0]);
            return 2;
        } else if (path == NULL) {
            path = argv[i];
        } else {
            fprintf(stderr, "usage: %s [--trace] [--cache[=SIZE:WAYS:BLOCK]] "
                    "[--pipeline] [program.elf]\n", argv[0]);
            return 2;
        }
    }

    Memory mem = {0}; /* zero-init so mem_free is safe even if loading fails */
    int demo = (path == NULL);
    uint32_t entry;

    /* Set up the memory image first, since loading an ELF can fail. Doing it
     * before any stdout output keeps loader diagnostics (stderr) from
     * interleaving with a half-printed banner. The demo uses a fixed region;
     * for an ELF, the loader sizes guest memory to the program's load image. */
    if (demo) {
        if (mem_init(&mem, MEM_BASE, MEM_SIZE) != 0) {
            fprintf(stderr, "failed to allocate guest memory\n");
            return 1;
        }
        if (mem_load(&mem, MEM_BASE, (const uint8_t *)demo_program,
                     sizeof demo_program) != 0) {
            fprintf(stderr, "failed to load demo program\n");
            mem_free(&mem);
            return 1;
        }
        entry = MEM_BASE;
    } else {
        if (elf_load(path, &mem, &entry) != 0) {
            return 1;
        }
    }

    /* A loader/kernel hands the program an initial stack pointer; the ISA reset
     * state leaves every register zero, which would fault the first push. Point
     * sp at the top of the guest region, 16-byte aligned per the RISC-V ABI.
     * The region carries stack headroom above the image (the demo's fixed area
     * doubles as stack; for an ELF the loader reserves it), and the stack grows
     * downward from here. */
    uint32_t sp = (mem.base + mem.size) & ~(uint32_t)0xf;

    printf("Quanta — RV32I emulator\n");
    if (demo) {
        printf("No ELF given; running the built-in demo program.\n\n");
    } else {
        printf("Loaded %s (entry = 0x%08x, sp = 0x%08x)\n\n", path, entry, sp);
    }

    CPU cpu;
    cpu_init(&cpu, &mem, entry);
    reg_write(&cpu, 2, sp); /* x2 = sp */

    /* Optional cache model: a pure observability layer in front of memory. It
     * watches data load/store addresses and tallies hits/misses without
     * touching the data, so it never changes what the program computes. */
    Cache cache;
    int cache_ready = 0;
    if (cache_on) {
        if (cache_init(&cache, csize, cways, cblock) != 0) {
            mem_free(&mem);
            return 2;
        }
        cpu.cache = &cache;
        cache_ready = 1;
    }

    /* Optional pipeline timing model: another overlay (it reads the retired
     * instruction stream, never the data), reported alongside the cache. */
    Pipeline pipe;
    if (pipe_on) pipeline_init(&pipe);

    /* Any output the program writes via syscalls appears here, mid-run. */
    int steps = run_until_halt(&cpu, trace, pipe_on ? &pipe : NULL);

    if (cpu.halt_reason == HALT_EXIT) {
        printf("Program exited with code %u after %d instruction(s).\n\n",
               cpu.exit_code, steps);
    } else if (cpu.halted) {
        printf("Halted: %s", halt_reason_str(cpu.halt_reason));
        if (cpu.halt_reason == HALT_MEM_FAULT)
            printf(" at 0x%08x", mem.fault_addr);
        printf(" after %d instruction(s).\n\n", steps);
    } else {
        printf("Stopped at the %d-instruction safety limit.\n\n", steps);
    }

    if (cache_ready) {
        cache_report(&cache, stdout);
        printf("\n");
    }

    if (pipe_on) {
        pipeline_report(&pipe, stdout);
        printf("\n");
    }

    cpu_dump(&cpu);

    /* The built-in demo has a known answer, so check it as a self-test. A
     * loaded ELF is arbitrary, so we just report its final state. */
    if (demo) {
        uint32_t a2 = reg_read(&cpu, 12); /* a2 */
        uint32_t a3 = reg_read(&cpu, 13); /* a3 */
        printf("\nExpected: a2 = 42, a3 = 32\n");
        printf("Got:      a2 = %u, a3 = %u  -> %s\n",
               a2, a3, (a2 == 42 && a3 == 32) ? "OK" : "MISMATCH");
    }

    /* Propagate the guest's exit status as our own process exit code (like
     * qemu-user or spike): a clean exit returns its code, an abnormal stop
     * (ebreak/trap/limit) returns 1. `make check` relies on this to tell a
     * passing conformance test (exit 0) from a failing one. */
    int status = (cpu.halt_reason == HALT_EXIT) ? (int)(cpu.exit_code & 0xffu) : 1;
    if (cache_ready) cache_free(&cache);
    mem_free(&mem);
    return status;
}
