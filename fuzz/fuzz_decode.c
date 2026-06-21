/*
 * libFuzzer harness for the decode/execute path. The input is loaded as a raw
 * code image and interpreted: the engine must survive any instruction stream
 * without crashing or hitting undefined behaviour. Out-of-range accesses become
 * HALT_MEM_FAULT, illegal encodings halt, and the run is bounded so a tight loop
 * cannot hang the fuzzer.
 *
 * See fuzz/fuzz_elf.c for the build modes (`make fuzz` / `make fuzz-replay`).
 */
#include "quanta.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size == 0) return 0;
    Quanta *q = quanta_create();
    if (!q) return 0;

    uint32_t base = 0x80000000u;
    uint32_t memsz = 1u << 16;                 /* 64 KiB code/data region */
    size_t len = size < memsz ? size : (size_t)memsz;
    if (quanta_load_image(q, base, memsz, data, len, base) == QUANTA_OK)
        quanta_run(q, 100000, NULL);           /* bounded run */

    quanta_destroy(q);
    return 0;
}
