/*
 * Snapshot / restore conformance (E10).
 *
 * The deterministic round-robin scheduler makes a run a pure function of its
 * state, so a snapshot taken partway through must let the tail replay
 * bit-for-bit. This harness proves that on real guest programs, driving the
 * engine only through the public libquanta API (like examples/embed.c):
 *
 *   A (control): load, run to halt, fingerprint the final machine.
 *   B: load, single-step to the midpoint, take a snapshot, then run to halt —
 *      its final fingerprint must equal A's (snapshotting is non-perturbing).
 *   C: restore the midpoint snapshot into B and run to halt again — its final
 *      fingerprint, tail step count, and exit must all match B's, which can only
 *      hold if the snapshot captured *every* piece of state the tail depends on
 *      (registers, CSRs, TLB, all of RAM, the device register files, the
 *      scheduler cursor). A missed byte anywhere diverges the fingerprint.
 *
 * Exit 0 if every guest passes, 1 otherwise. Each argv entry is a guest ELF; the
 * Makefile passes a few with varied state (a stack/array workload, muldiv, and
 * atomics — the last exercising the LR/SC reservation fields).
 */
#include "quanta.h"

#include <stdint.h>
#include <stdio.h>

/* FNV-1a over a byte range, chained so a whole-machine fingerprint is one call
 * sequence. */
static uint64_t fnv1a(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

/* A fingerprint of the architecturally-visible state: PC, the 32 registers, and
 * all of guest RAM. If a restore misplaces any of it, this changes. */
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

static Result run_to_halt(Quanta *q) {
    Result r;
    r.halt      = quanta_run(q, 0, &r.steps);
    r.exit_code = quanta_exit_code(q);
    r.fp        = fingerprint(q);
    return r;
}

/* Returns 0 if the guest's snapshot/restore behaviour is correct, 1 otherwise. */
static int check_guest(const char *path) {
    /* Run A: the control — a plain run to halt, no snapshot involved. */
    Quanta *a = quanta_create();
    if (!a || quanta_load_elf(a, path) != QUANTA_OK) {
        fprintf(stderr, "FAIL  %s: could not load\n", path);
        quanta_destroy(a);
        return 1;
    }
    Result ra = run_to_halt(a);
    quanta_destroy(a);

    if (ra.steps < 2) {
        fprintf(stderr, "FAIL  %s: too short (%llu steps) to snapshot midway\n",
                path, (unsigned long long)ra.steps);
        return 1;
    }

    /* Run B: single-step to the midpoint, snapshot, then run to halt. On a
     * uniprocessor each quanta_step retires exactly one instruction. */
    Quanta *b = quanta_create();
    if (!b || quanta_load_elf(b, path) != QUANTA_OK) {
        fprintf(stderr, "FAIL  %s: could not reload\n", path);
        quanta_destroy(b);
        return 1;
    }
    uint64_t mid = ra.steps / 2;
    for (uint64_t i = 0; i < mid; i++) quanta_step(b);

    QuantaSnapshot *snap = quanta_snapshot(b);
    if (!snap) {
        fprintf(stderr, "FAIL  %s: snapshot failed\n", path);
        quanta_destroy(b);
        return 1;
    }

    Result rb = run_to_halt(b);

    /* Run C: rewind to the snapshot and run the tail again. */
    if (quanta_restore(b, snap) != QUANTA_OK) {
        fprintf(stderr, "FAIL  %s: restore failed\n", path);
        quanta_snapshot_free(snap);
        quanta_destroy(b);
        return 1;
    }
    Result rc = run_to_halt(b);

    quanta_snapshot_free(snap);
    quanta_destroy(b);

    /* B's full run must match the control (snapshotting is read-only), and the
     * restored tail must match B's tail exactly. */
    int ok = 1;
    if (!(rb.halt == ra.halt && rb.exit_code == ra.exit_code && rb.fp == ra.fp)) {
        fprintf(stderr, "FAIL  %s: mid-run snapshot perturbed the run\n", path);
        ok = 0;
    }
    if (!(rc.halt == rb.halt && rc.exit_code == rb.exit_code &&
          rc.fp == rb.fp && rc.steps == rb.steps)) {
        fprintf(stderr, "FAIL  %s: restored tail diverged "
                "(halt %d/%d, exit %u/%u, steps %llu/%llu, fp %016llx/%016llx)\n",
                path, rb.halt, rc.halt, rb.exit_code, rc.exit_code,
                (unsigned long long)rb.steps, (unsigned long long)rc.steps,
                (unsigned long long)rb.fp, (unsigned long long)rc.fp);
        ok = 0;
    }
    if (ok)
        printf("PASS  %s (%llu steps, snapshot at %llu, tail replayed)\n",
               path, (unsigned long long)ra.steps, (unsigned long long)mid);
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s program.elf [program.elf ...]\n", argv[0]);
        return 2;
    }
    int fail = 0;
    for (int i = 1; i < argc; i++)
        fail |= check_guest(argv[i]);
    if (fail) {
        fprintf(stderr, "FAILED\n");
        return 1;
    }
    printf("all snapshot/restore checks passed\n");
    return 0;
}
