/*
 * libFuzzer harness for the ELF loader — the most security-sensitive path,
 * since it parses an untrusted file. The loader must reject malformed input with
 * an error and never crash, leak, or hit undefined behaviour. If an input does
 * load, we interpret it briefly too, exercising the engine on attacker-shaped
 * code.
 *
 * `make fuzz` builds this under clang's -fsanitize=fuzzer for real fuzzing;
 * `make fuzz-replay` links fuzz/standalone.c and runs it over the seed corpus
 * under gcc + ASan/UBSan (no clang needed).
 */
#define _DEFAULT_SOURCE   /* expose POSIX mkstemp/write/unlink under -std=c11 */

#include "quanta.h"

#include <stdlib.h>
#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    /* quanta_load_elf takes a path, so stage the input in a temp file. */
    char path[] = "/tmp/quanta_fuzz_elf_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 0;
    ssize_t w = write(fd, data, size);
    close(fd);
    if (w == (ssize_t)size) {
        Quanta *q = quanta_create();
        if (q) {
            if (quanta_load_elf(q, path) == QUANTA_OK)
                quanta_run(q, 10000, NULL); /* bounded: don't hang on a loop */
            quanta_destroy(q);
        }
    }
    unlink(path);
    return 0;
}
