/*
 * Minimal libquanta embedding example.
 *
 * Demonstrates the whole public surface needed to run a guest: create a handle,
 * load a raw code image, run it to completion, and read back architectural
 * state — without ever touching the engine internals. This is the program E1's
 * "Done when" calls for, and `make embed` runs it as a smoke test.
 *
 * The image is the built-in demo: a2 = 5 + 37 = 42, a3 = 37 - 5 = 32, exit(0).
 */
#include "quanta.h"

#include <stdint.h>
#include <stdio.h>

static const uint32_t demo[] = {
    0x00500513, /* addi a0, zero, 5   */
    0x02500593, /* addi a1, zero, 37  */
    0x00b50633, /* add  a2, a0, a1    */
    0x40a586b3, /* sub  a3, a1, a0    */
    0x05d00893, /* addi a7, zero, 93  (exit) */
    0x00000513, /* addi a0, zero, 0   (status) */
    0x00000073  /* ecall              */
};

int main(void) {
    Quanta *q = quanta_create();
    if (!q) {
        fprintf(stderr, "embed: out of memory\n");
        return 1;
    }

    if (quanta_load_image(q, 0x80000000u, 1u << 16,
                          demo, sizeof demo, 0x80000000u) != QUANTA_OK) {
        fprintf(stderr, "embed: failed to load image\n");
        quanta_destroy(q);
        return 1;
    }

    uint64_t steps = 0;
    QuantaHalt h = quanta_run(q, 0, &steps);
    uint32_t a2 = quanta_reg(q, 12), a3 = quanta_reg(q, 13);

    printf("halt=%s steps=%llu exit=%u  a2=%u a3=%u -> %s\n",
           quanta_halt_str(h), (unsigned long long)steps, quanta_exit_code(q),
           a2, a3, (a2 == 42 && a3 == 32) ? "OK" : "MISMATCH");

    int ok = (h == QUANTA_HALT_EXIT && a2 == 42 && a3 == 32);
    quanta_destroy(q);
    return ok ? 0 : 1;
}
