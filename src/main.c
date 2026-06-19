#include "cpu.h"
#include "memory.h"

#include <stdio.h>
#include <stdint.h>

/*
 * Quanta MVP driver.
 *
 * This is the smallest runnable loop: a tiny RV32I program is hardcoded as
 * machine-code words, loaded into memory, and executed until the CPU halts.
 * No ELF parsing yet — that is the next milestone. Hardcoding the program
 * keeps the dependency surface at zero and makes the fetch/decode/execute
 * core the only thing under test.
 *
 * The program below computes a few values into registers so you can see state
 * change after each step:
 *
 *   addi a0, zero, 5     # a0 = 5
 *   addi a1, zero, 37    # a1 = 37
 *   add  a2, a0, a1      # a2 = a0 + a1 = 42
 *   sub  a3, a1, a0      # a3 = a1 - a0 = 32
 *   ecall                # halt
 *
 * The 32-bit encodings were produced by hand from the RV32I spec; the same
 * bytes are what `riscv64-unknown-elf-gcc` would emit for these instructions.
 */

#define MEM_BASE 0x80000000u
#define MEM_SIZE (1u << 16)   /* 64 KiB is plenty for the demo */

static const uint32_t program[] = {
    0x00500513, /* addi a0, zero, 5   */
    0x02500593, /* addi a1, zero, 37  */
    0x00b50633, /* add  a2, a0, a1    */
    0x40a586b3, /* sub  a3, a1, a0    */
    0x00000073  /* ecall (halt)       */
};

int main(void) {
    Memory mem;
    if (mem_init(&mem, MEM_BASE, MEM_SIZE) != 0) {
        fprintf(stderr, "failed to allocate guest memory\n");
        return 1;
    }

    /* Load the program at the base of memory. */
    mem_load(&mem, MEM_BASE, (const uint8_t *)program, sizeof(program));

    CPU cpu;
    cpu_init(&cpu, &mem, MEM_BASE);

    printf("Quanta — RV32I emulator (MVP)\n");
    printf("Running %zu hardcoded instructions...\n\n",
           sizeof(program) / sizeof(program[0]));

    /* Step until the program halts (or a safety limit is hit, so a buggy
     * program can never spin forever). */
    int steps = 0;
    const int max_steps = 1000;
    while (!cpu.halted && steps < max_steps) {
        cpu_step(&cpu);
        steps++;
    }

    printf("Halted after %d instruction(s).\n\n", steps);
    cpu_dump(&cpu);

    /* Sanity check the expected results. */
    uint32_t a2 = reg_read(&cpu, 12); /* a2 */
    uint32_t a3 = reg_read(&cpu, 13); /* a3 */
    printf("\nExpected: a2 = 42, a3 = 32\n");
    printf("Got:      a2 = %u, a3 = %u  -> %s\n",
           a2, a3, (a2 == 42 && a3 == 32) ? "OK" : "MISMATCH");

    mem_free(&mem);
    return 0;
}
