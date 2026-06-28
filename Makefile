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
#   make check-cache   check the cache model on a locality workload
#   make check-pipeline  check the pipeline model on a hazard workload
#   make check-gdb    drive the gdb remote stub with a self-contained client
#   make check-devices  check the MMIO devices and interrupt delivery (M13)
#   make check-diff   differential-test against a reference sim (qemu-riscv32)
#   make check-arch   run the official riscv-arch-test conformance suite
#   make sanitize     build with ASan+UBSan and run the suite through it
#   make fuzz         build the libFuzzer harnesses (needs clang)
#   make fuzz-replay  run the harnesses over the corpus under gcc+ASan/UBSan
#   make coverage     instrument with gcov, run the suite, report line coverage
#   make analyze      static analysis (cppcheck + clang-tidy, when installed)
#   make install      install the CLI, libquanta, headers, and the man page
#   make debug      build with -g -O0 for stepping under gdb
#   make clean      remove build artifacts

CC      ?= gcc
AR      ?= ar
CFLAGS  ?= -std=c11 -Wall -Wextra -O2 -Isrc
LDFLAGS ?=

# Install location (override: make install PREFIX=/usr DESTDIR=/tmp/stage).
PREFIX  ?= /usr/local
DESTDIR ?=

# AddressSanitizer + UBSan for the `sanitize` target (instruments the host
# emulator only; the guest ELFs are built by the cross-toolchain as usual).
SANFLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all

# libFuzzer harnesses (fuzz/) build the engine sources under clang.
FUZZ_CC      ?= clang
FUZZ_TARGETS := fuzz/fuzz_elf fuzz/fuzz_decode

# RISC-V cross-toolchain (override if yours is named differently).
RVCC      ?= riscv64-unknown-elf-gcc
RVOBJDUMP ?= riscv64-unknown-elf-objdump
RVCFLAGS  ?= -march=rv32i -mabi=ilp32 -nostdlib -nostartfiles -Ttext=0x80000000

SRC     := $(wildcard src/*.c)
LIB_SRC := $(filter-out src/main.c,$(SRC))
LIB_OBJ := $(LIB_SRC:.c=.o)
LIB     := libquanta.a
BIN     := quanta

.PHONY: all run tests check check-disasm check-cache check-pipeline check-gdb check-devices check-diff check-arch embed sanitize fuzz fuzz-replay coverage analyze install uninstall debug clean

all: $(BIN)

# The emulator engine as a static library; the CLI and any embedding program
# (see examples/) link against it. main.c is the CLI driver, kept out of the lib.
# 'D' makes the archive deterministic (zeroed mtime/uid/gid); together with
# objects that embed no __DATE__/__TIME__, a rebuild is byte-identical.
$(LIB): $(LIB_OBJ)
	$(AR) rcsD $@ $^

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Coarse but correct: any header change rebuilds every object.
$(LIB_OBJ) src/main.o: $(wildcard src/*.h)

$(BIN): src/main.o $(LIB)
	$(CC) $(CFLAGS) -o $@ src/main.o $(LIB) $(LDFLAGS)

run: $(BIN)
	./$(BIN)

# Build and run the minimal embedding example against the library.
embed: examples/embed
	./examples/embed

examples/embed: examples/embed.c $(LIB)
	$(CC) $(CFLAGS) -o $@ examples/embed.c $(LIB) $(LDFLAGS)

# Build the sample assembly programs with the cross-toolchain. Every tests/*.S
# becomes a tests/*.elf via the pattern rule below.
TEST_SRC := $(wildcard tests/*.S)
TEST_ELF := $(TEST_SRC:.S=.elf)

tests: $(TEST_ELF)

tests/%.elf: tests/%.S
	$(RVCC) $(RVCFLAGS) -o $@ $<
	@echo "Built $@ — disassemble with: $(RVOBJDUMP) -d $@"

# tests/test_muldiv.S exercises the RV32M extension, which the base -march=rv32i
# assembler rejects. Enable M for just this one ELF; the rest of the suite stays
# pure base integer. Everything else in RVCFLAGS is carried over unchanged.
tests/test_muldiv.elf: RVCFLAGS := $(subst rv32i,rv32im,$(RVCFLAGS))

# tests/test_csr.S uses the Zicsr CSR instructions and Zifencei's fence.i, which
# the base assembler rejects. Enable both extensions for just this one ELF.
tests/test_csr.elf: RVCFLAGS := $(subst rv32i,rv32i_zicsr_zifencei,$(RVCFLAGS))

# The M9/M12 privilege + paging tests use trap CSRs, mret/sret, satp and
# sfence.vma, which need Zicsr; the M13 interrupt test uses the trap CSRs too.
tests/test_trap.elf tests/test_priv.elf tests/test_vm.elf tests/test_irq.elf: RVCFLAGS := $(subst rv32i,rv32i_zicsr,$(RVCFLAGS))

# tests/test_atomic.S uses the RV32A atomics, which the base assembler rejects.
tests/test_atomic.elf: RVCFLAGS := $(subst rv32i,rv32ia,$(RVCFLAGS))

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

# Exercise the cache model: confirm it doesn't change program results and that
# a smaller cache misses more on the array-traversal workload (tests/test_stack).
check-cache: $(BIN) tests/test_stack.elf
	@sh tests/check_cache.sh

# Exercise the pipeline model: confirm that reordering to avoid a load-use
# hazard lowers the stall count and cycle estimate without changing the result.
check-pipeline: $(BIN) tests/hazard_slow.elf tests/hazard_fast.elf
	@sh tests/check_pipeline.sh

# Drive the GDB remote stub (--gdb) with a self-contained RSP client that
# attaches, reads/writes registers and memory, single-steps, sets a breakpoint,
# and continues — no riscv `gdb` needed. Skips cleanly without python3.
check-gdb: $(BIN) tests/hello.elf
	@sh tests/check_gdb.sh

# Exercise the M13 platform: the device/interrupt test must exit clean (CLINT
# timer, software IPI, and a PLIC-routed external interrupt all fire) and its
# UART console output must reach stdout.
check-devices: $(BIN) tests/test_irq.elf
	@sh tests/check_devices.sh

# Differential test: compare quanta against a reference simulator (qemu-riscv32
# by default; override with REF=...) on the sample programs. Skips cleanly if
# the reference simulator is not installed.
#
# The privileged tests are excluded: they touch machine-mode CSRs and trap
# state (mscratch, mtvec, mret, delegation) that user-mode qemu rejects with
# SIGILL because it provides its own supervisor. `make check` pins them instead.
DIFF_ELF := $(filter-out tests/test_csr.elf tests/test_trap.elf tests/test_priv.elf tests/test_vm.elf tests/test_irq.elf,$(TEST_ELF))

check-diff: $(BIN) $(TEST_ELF)
	@sh tests/check_diff.sh $(DIFF_ELF)

# Official RISC-V architectural conformance. Build each riscv-arch-test program
# with the Quanta target (tests/arch/), run it under --signature, and diff the
# result against the suite's committed reference signatures — the recognised bar
# for "this really is RV32I/M/...". The suite ships the golden signatures, so no
# reference model is needed; the harness fetches it into build/ on first run and
# skips cleanly without the cross-toolchain or network. See tests/arch/README.md.
check-arch: $(BIN)
	@sh tests/check_arch.sh

debug: CFLAGS := -std=c11 -Wall -Wextra -g -O0 -Isrc
debug: clean $(BIN)

# Build the emulator with AddressSanitizer + UBSan and run the whole suite
# through it. The sanitizers instrument the host emulator (not the guest), so a
# clean pass means no memory or undefined-behaviour bug fired while parsing the
# sample ELFs and interpreting every instruction group, overlay, and syscall.
# UB aborts the run (-fno-sanitize-recover), so a green result is meaningful.
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="-std=c11 -Wall -Wextra -g -O1 $(SANFLAGS) -Isrc" \
		embed check check-disasm check-cache check-pipeline check-gdb \
		check-devices check-diff

# Fuzzing. `make fuzz` builds the libFuzzer harnesses (clang only): each links
# the engine sources under -fsanitize=fuzzer,address,undefined. `make fuzz-replay`
# instead links a plain main (fuzz/standalone.c) with gcc + ASan/UBSan and runs
# each harness over the sample ELFs, so the harnesses stay exercised without
# clang; CI runs the real libFuzzer build.
fuzz: $(FUZZ_TARGETS)

fuzz/fuzz_%: fuzz/fuzz_%.c $(LIB_SRC) $(wildcard src/*.h)
	$(FUZZ_CC) -std=c11 -Wall -Wextra -g -O1 -fsanitize=fuzzer,address,undefined \
		-Isrc -o $@ $< $(LIB_SRC)

fuzz-replay: $(TEST_ELF)
	@for h in fuzz_elf fuzz_decode; do \
		$(CC) -std=c11 -Wall -Wextra -Werror -g -O1 $(SANFLAGS) -Isrc \
			-o fuzz/$$h.replay fuzz/$$h.c fuzz/standalone.c $(LIB_SRC) || exit 1; \
		echo "replay $$h over $(words $(TEST_ELF)) sample ELF(s):"; \
		./fuzz/$$h.replay $(TEST_ELF) && echo "  OK — $$h sanitizer-clean" || exit 1; \
	done

# Code coverage. Do a clean instrumented rebuild (gcov's --coverage, like the
# sanitize target's clean rebuild), run the functional suite plus the embedding
# example to exercise the engine, then summarise line coverage. The .gcda files
# accumulate across every quanta invocation, so coverage is the union of the
# whole suite. tests/coverage.sh prefers lcov (HTML under build/coverage) and
# falls back to gcov. Instruments the host emulator, never the guest ELFs.
COVFLAGS := -std=c11 -Wall -Wextra -g -O0 --coverage -Isrc

coverage:
	$(MAKE) clean
	$(MAKE) CFLAGS="$(COVFLAGS)" all embed check check-disasm check-cache check-pipeline check-gdb check-devices
	@sh tests/coverage.sh

# Static analysis: run whatever analyzers are installed (cppcheck, clang-tidy),
# each skipping cleanly when absent — like check-diff without qemu. CI installs
# them and additionally runs scan-build over a full build (see ci.yml). A clean
# pass means no analyzer flagged a defect outside the baselined suppressions
# (tests/cppcheck-suppress.txt, .clang-tidy).
analyze:
	@sh tests/analyze.sh

# Install the CLI, the engine library + public header, and the man page under
# $(DESTDIR)$(PREFIX). DESTDIR supports staged/packaging builds.
install: $(BIN) $(LIB)
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib \
		$(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 755 $(BIN) $(DESTDIR)$(PREFIX)/bin/
	install -m 644 $(LIB) $(DESTDIR)$(PREFIX)/lib/
	install -m 644 src/quanta.h src/gdbstub.h $(DESTDIR)$(PREFIX)/include/
	install -m 644 docs/quanta.1 $(DESTDIR)$(PREFIX)/share/man/man1/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN) $(DESTDIR)$(PREFIX)/lib/$(LIB) \
		$(DESTDIR)$(PREFIX)/include/quanta.h \
		$(DESTDIR)$(PREFIX)/include/gdbstub.h \
		$(DESTDIR)$(PREFIX)/share/man/man1/quanta.1

clean:
	rm -f $(BIN) $(LIB) src/*.o examples/embed tests/*.elf \
		$(FUZZ_TARGETS) fuzz/*.replay \
		src/*.gcno src/*.gcda examples/*.gcno examples/*.gcda
	rm -rf build/arch-work build/coverage
