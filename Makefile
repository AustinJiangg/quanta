# Quanta build
#
# Two distinct toolchains are at play here:
#   CC       - your host compiler. Builds the emulator (a native x86 binary).
#   RVCC     - the RISC-V cross-compiler. Builds test programs that the
#              emulator will eventually load and run.
#
# Targets:
#   make            build the emulator -> ./quanta
#   make run        build and run the MVP (hardcoded program)
#   make tests      build the sample RISC-V program -> tests/hello.elf
#   make check      build and run the RV32I conformance suite
#   make check-disasm  cross-check the disassembler against objdump
#   make debug      build with -g -O0 for stepping under gdb
#   make clean      remove build artifacts

CC      ?= gcc
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -Isrc
LDFLAGS ?=

# RISC-V cross-toolchain (override if yours is named differently).
RVCC      ?= riscv64-unknown-elf-gcc
RVOBJDUMP ?= riscv64-unknown-elf-objdump
RVCFLAGS  ?= -march=rv32i -mabi=ilp32 -nostdlib -nostartfiles -Ttext=0x80000000

SRC := $(wildcard src/*.c)
BIN := quanta

.PHONY: all run tests check check-disasm debug clean

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

run: $(BIN)
	./$(BIN)

# Build the sample assembly programs with the cross-toolchain. Every tests/*.S
# becomes a tests/*.elf via the pattern rule below.
TEST_SRC := $(wildcard tests/*.S)
TEST_ELF := $(TEST_SRC:.S=.elf)

tests: $(TEST_ELF)

tests/%.elf: tests/%.S
	$(RVCC) $(RVCFLAGS) -o $@ $<
	@echo "Built $@ — disassemble with: $(RVOBJDUMP) -d $@"

# Run the RV32I conformance suite (tests/test_*.S) through the emulator. Each
# test exits 0 on success or the number of its first failed check, which quanta
# propagates as its own exit status; we use that to print PASS/FAIL per file.
CHECK_ELF := $(patsubst %.S,%.elf,$(wildcard tests/test_*.S))

check: $(BIN) $(CHECK_ELF)
	@fail=0; \
	for t in $(CHECK_ELF); do \
		./$(BIN) $$t >/dev/null 2>&1; rc=$$?; \
		if [ $$rc -eq 0 ]; then echo "PASS  $$t"; \
		else echo "FAIL  $$t (check $$rc)"; fail=1; fi; \
	done; \
	if [ $$fail -ne 0 ]; then echo "FAILED"; exit 1; else echo "all RV32I tests passed"; fi

# Cross-check the disassembler against the reference assembler: run each sample
# ELF under `quanta --trace` and diff its disassembly against objdump's. Needs
# the cross-toolchain for objdump; the script skips cleanly without it.
check-disasm: $(BIN) $(TEST_ELF)
	@sh tests/check_disasm.sh $(TEST_ELF)

debug: CFLAGS := -std=c11 -Wall -Wextra -g -O0 -Isrc
debug: clean $(BIN)

clean:
	rm -f $(BIN) tests/*.elf
