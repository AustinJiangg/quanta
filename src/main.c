#include "cpu.h"
#include "memory.h"
#include "elf.h"

#include <stdio.h>
#include <stdint.h>

/*
 * Quanta driver.
 *
 * Usage:
 *   quanta [program.elf]
 *
 * With a path, Quanta loads that RV32I ELF executable and runs it from its
 * entry point. With no argument, it runs a tiny built-in demo program — the
 * original MVP smoke test — so the emulator stays runnable even without the
 * RISC-V cross-toolchain installed.
 *
 * The demo computes a few values into registers so you can watch state change:
 *
 *   addi a0, zero, 5     # a0 = 5
 *   addi a1, zero, 37    # a1 = 37
 *   add  a2, a0, a1      # a2 = a0 + a1 = 42
 *   sub  a3, a1, a0      # a3 = a1 - a0 = 32
 *   ecall                # halt
 *
 * tests/hello.S is the same program in assembly; once built with the
 * cross-toolchain (`make tests`), `./quanta tests/hello.elf` reproduces the
 * register state below from a real ELF instead of these hardcoded words.
 */

#define MEM_BASE 0x80000000u
#define MEM_SIZE (1u << 16)   /* 64 KiB is plenty for the demo */

static const uint32_t demo_program[] = {
    0x00500513, /* addi a0, zero, 5   */
    0x02500593, /* addi a1, zero, 37  */
    0x00b50633, /* add  a2, a0, a1    */
    0x40a586b3, /* sub  a3, a1, a0    */
    0x00000073  /* ecall (halt)       */
};

/* Step until the program halts, or a safety limit is hit so a buggy program
 * can never spin forever. Returns the number of instructions executed. */
static int run_until_halt(CPU *cpu) {
    int steps = 0;
    const int max_steps = 1000;
    while (!cpu->halted && steps < max_steps) {
        cpu_step(cpu);
        steps++;
    }
    return steps;
}

int main(int argc, char **argv) {
    if (argc > 2) {
        fprintf(stderr, "usage: %s [program.elf]\n", argv[0]);
        return 2;
    }

    Memory mem;
    if (mem_init(&mem, MEM_BASE, MEM_SIZE) != 0) {
        fprintf(stderr, "failed to allocate guest memory\n");
        return 1;
    }

    int demo = (argc < 2);
    uint32_t entry;

    /* Set up the memory image first, since loading an ELF can fail. Doing it
     * before any stdout output keeps loader diagnostics (stderr) from
     * interleaving with a half-printed banner. */
    if (demo) {
        /* Load the built-in program at the base of memory. */
        mem_load(&mem, MEM_BASE, (const uint8_t *)demo_program,
                 sizeof(demo_program));
        entry = MEM_BASE;
    } else {
        if (elf_load(argv[1], &mem, &entry) != 0) {
            mem_free(&mem);
            return 1;
        }
    }

    printf("Quanta — RV32I emulator\n");
    if (demo) {
        printf("No ELF given; running the built-in demo program.\n\n");
    } else {
        printf("Loaded %s (entry = 0x%08x)\n\n", argv[1], entry);
    }

    CPU cpu;
    cpu_init(&cpu, &mem, entry);

    int steps = run_until_halt(&cpu);
    printf("Halted after %d instruction(s).\n\n", steps);
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

    mem_free(&mem);
    return 0;
}
