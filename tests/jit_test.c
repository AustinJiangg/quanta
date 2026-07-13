/*
 * Basic-block JIT differential (M25b).
 *
 * The JIT translates guest basic blocks to host code, but its contract is the
 * same as every overlay before it: results bit-identical to the interpreter,
 * which remains the golden reference. This harness pins that the same way
 * dcache_test.c pins the decode cache — for each guest ELF the machine runs
 * twice through the public libquanta API, once with the JIT on and once off,
 * and the two runs must agree on every observable: halt reason, exit code,
 * retired-step count, and a fingerprint of PC + all registers + all of RAM.
 *
 * The step-count comparison is the sharp edge here: a translated block reports
 * how many scheduler steps it consumed, and any accounting slip (a trap exit
 * crediting the trapping instruction, an aborted device access consuming a
 * step) shifts the count even when the final state happens to match. The
 * guests span straight-line conformance code, tight loops, traps, paging
 * (Sv32 and Sv39), device interrupts and timers (where the interrupt-horizon
 * logic decides when blocks may run), and self-modifying code + FENCE.I
 * (smc.elf), which exercises the JIT flush.
 *
 * On a host without JIT support the whole check skips cleanly (exit 0), like
 * the qemu differential without qemu.
 */
#include "quanta.h"

#include <stdint.h>
#include <stdio.h>

/* FNV-1a over a byte range, chained. */
static uint64_t fnv1a(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

/* Fingerprint the architecturally-visible state: PC, the 32 registers, all RAM. */
static uint64_t fingerprint(const Quanta *q) {
    uint64_t h = 1469598103934665603ULL; /* FNV offset basis */
    uint64_t pc = quanta_pc(q);
    h = fnv1a(h, &pc, sizeof pc);
    for (int i = 0; i < 32; i++) {
        uint64_t r = quanta_reg(q, i);
        h = fnv1a(h, &r, sizeof r);
    }
    uint64_t base = quanta_mem_base(q), size = quanta_mem_size(q);
    uint8_t buf[4096];
    for (uint64_t off = 0; off < size; off += sizeof buf) {
        uint64_t left = size - off;
        size_t n = left < sizeof buf ? (size_t)left : sizeof buf;
        if (quanta_mem_read(q, base + off, buf, n) == QUANTA_OK)
            h = fnv1a(h, buf, n);
    }
    return h;
}

typedef struct {
    QuantaHalt halt;
    uint32_t   exit_code;
    uint64_t   steps;
    uint64_t   fp;
} Result;

/* Load and run a guest to halt with the JIT on or off. */
static int run_guest(const char *path, int jit_on, Result *out) {
    Quanta *q = quanta_create();
    if (!q) { fprintf(stderr, "FAIL  %s: alloc\n", path); return 1; }
    if (jit_on && quanta_set_jit(q, 1) != QUANTA_OK) {
        fprintf(stderr, "FAIL  %s: could not enable the JIT\n", path);
        quanta_destroy(q);
        return 1;
    }
    if (quanta_load_elf(q, path) != QUANTA_OK) {
        fprintf(stderr, "FAIL  %s: could not load\n", path);
        quanta_destroy(q);
        return 1;
    }
    out->halt      = quanta_run(q, 0, &out->steps);
    out->exit_code = quanta_exit_code(q);
    out->fp        = fingerprint(q);
    quanta_destroy(q);
    return 0;
}

static int check_guest(const char *path) {
    Result on, off;
    if (run_guest(path, 1, &on) || run_guest(path, 0, &off)) return 1;

    if (on.halt == off.halt && on.exit_code == off.exit_code &&
        on.steps == off.steps && on.fp == off.fp) {
        printf("PASS  %s (%llu steps, exit %u, jit == interpreter)\n",
               path, (unsigned long long)on.steps, on.exit_code);
        return 0;
    }
    fprintf(stderr, "FAIL  %s: the JIT diverged from the interpreter "
            "(halt %d/%d, exit %u/%u, steps %llu/%llu, fp %016llx/%016llx)\n",
            path, on.halt, off.halt, on.exit_code, off.exit_code,
            (unsigned long long)on.steps, (unsigned long long)off.steps,
            (unsigned long long)on.fp, (unsigned long long)off.fp);
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s program.elf [program.elf ...]\n", argv[0]);
        return 2;
    }
    if (!quanta_jit_available()) {
        printf("SKIP: no JIT on this host (needs x86-64); nothing to compare\n");
        return 0;
    }
    int fail = 0;
    for (int i = 1; i < argc; i++) fail |= check_guest(argv[i]);
    if (fail) { fprintf(stderr, "FAILED\n"); return 1; }
    printf("all jit differential checks passed\n");
    return 0;
}
