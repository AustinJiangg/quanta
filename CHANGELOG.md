# Changelog

All notable changes to Quanta are documented here. The format is based on
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and the project follows
[Semantic Versioning](https://semver.org/spec/v2.0.0.html). The version is defined
once in `src/quanta.h` (`QUANTA_VERSION_*`) and surfaced by `quanta --version`.

## [Unreleased]

## [0.1.0] - 2026-06-28

First tagged release: a correct, observable RV32 emulator with a reusable engine
library and a production-grade test/CI harness. Roadmap milestones M0–M12 and
E1–E8.

### Added

- **Core ISA** — RV32I base integer set, RV32M multiply/divide, RV32A atomics
  (LR/SC and the AMOs), and Zicsr/Zifencei (CSR access, `fence.i`). (M0–M5, M8,
  M10)
- **Privileged architecture** — M/S/U privilege levels, the trap CSRs,
  exception entry with delegation, and `mret`/`sret`, with a built-in SEE
  fallback when the guest installs no trap handler. (M9)
- **Sv32 virtual memory** — a two-level page-table walker, a software TLB,
  A/D-bit handling, and precise page faults; the identity until a guest sets
  `satp`. (M12)
- **ELF loader and system calls** — loads static little-endian RV32 `ET_EXEC`
  images and services `write`/`exit` through the ECALL path. (M1–M2)
- **Disassembler and `--trace`** — objdump-style output that shares the
  executor's decode tables, plus per-instruction trace narration. (M4)
- **Performance overlays** — a set-associative LRU cache model (`--cache`) and a
  5-stage pipeline timing estimate (`--pipeline`), both pure observability layers
  that never change results. (M6–M7)
- **`libquanta`** — the engine as a static library behind an opaque `Quanta *`
  handle with no host-killing `exit()`; the CLI is a thin client. A `--version`
  flag and a `quanta_version()` accessor. (E1, E8)
- **Test and CI harness** — a hand-written conformance suite (`make check`), the
  official riscv-arch-test signatures (`make check-arch`), a disassembler/objdump
  cross-check, cache/pipeline checks, qemu differential testing, ASan/UBSan,
  libFuzzer harnesses, gcov/lcov coverage, and cppcheck/clang-tidy/scan-build
  static analysis — all run in GitHub Actions. (M3, E2–E7)
- **Packaging** — a `CHANGELOG`, a `quanta.1` man page, and a PREFIX-based
  `make install`. (E8)

[Unreleased]: https://github.com/AustinJiangg/quanta/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/AustinJiangg/quanta/releases/tag/v0.1.0
