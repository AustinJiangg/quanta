/*
 * Decoded-instruction cache differential (M25a).
 *
 * The decode cache (decodecache.h) is a pure performance overlay: memoising the
 * fetch + length-decode + RVC expansion per physical PC must never change what a
 * program computes. This harness pins that invariant directly — the cache's own
 * golden reference is the plain switch-dispatched interpreter it accelerates.
 *
 * For each guest ELF it runs the machine twice through the public libquanta API:
 * once with the decode cache on (the default) and once off (quanta_set_dcache 0,
 * i.e. the plain interpreter). The two runs must agree on every observable: the
 * halt reason, the exit code, the retired-instruction count, and a fingerprint of
 * PC + all registers + all of RAM. Any divergence — a mis-memoised instruction, a
 * missed FENCE.I flush on self-modifying code — changes the fingerprint and fails.
 *
 * Exit 0 if every guest matches, 1 otherwise. The Makefile passes guests spanning
 * the state: a stack/array workload, muldiv, atomics, bit-manipulation, the MMIO
 * interrupt test, the Sv32 paging test, and the self-modifying-code + FENCE.I test
 * (smc.elf) that exercises the cache's invalidation path specifically.
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

/* Load and run a guest to halt with the decode cache on or off. */
static int run_guest(const char *path, int dcache_on, Result *out) {
    Quanta *q = quanta_create();
    if (!q) { fprintf(stderr, "FAIL  %s: alloc\n", path); return 1; }
    quanta_set_dcache(q, dcache_on);
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
        printf("PASS  %s (%llu steps, exit %u, on == off)\n",
               path, (unsigned long long)on.steps, on.exit_code);
        return 0;
    }
    fprintf(stderr, "FAIL  %s: decode cache diverged from the interpreter "
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
    int fail = 0;
    for (int i = 1; i < argc; i++) fail |= check_guest(argv[i]);
    if (fail) { fprintf(stderr, "FAILED\n"); return 1; }
    printf("all decode-cache differential checks passed\n");
    return 0;
}
