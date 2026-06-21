/*
 * Standalone driver for the fuzz harnesses, used when they are built without
 * libFuzzer (`make fuzz-replay`). It reads each file named on the command line
 * and feeds its contents to LLVMFuzzerTestOneInput once — so the harnesses can
 * be run over the seed corpus under gcc + ASan/UBSan with no clang, and a saved
 * crash input can be reproduced deterministically.
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); continue; }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); continue; }
        long n = ftell(f);
        if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); continue; }
        uint8_t *buf = malloc((size_t)n ? (size_t)n : 1);
        size_t rd = buf ? fread(buf, 1, (size_t)n, f) : 0;
        fclose(f);
        if (buf) {
            LLVMFuzzerTestOneInput(buf, rd);
            free(buf);
        }
    }
    return 0;
}
