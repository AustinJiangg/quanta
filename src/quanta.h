#ifndef QUANTA_H
#define QUANTA_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * libquanta — the embeddable RV32 emulator engine.
 *
 * An opaque handle owns the whole machine: CPU state, guest memory, and an
 * optional cache model. The core never calls exit() on its host — every stop is
 * reported as a QuantaHalt and every API failure as a QuantaStatus — so the
 * host program (the CLI, a test harness, a future GDB stub or language binding)
 * stays in control. Results are identical to driving the core directly; this
 * header just wraps it behind a stable interface.
 */

/* --- version (Semantic Versioning, https://semver.org) --- */

#define QUANTA_VERSION_MAJOR 0
#define QUANTA_VERSION_MINOR 1
#define QUANTA_VERSION_PATCH 0
#define QUANTA_VERSION_STRING "0.1.0"

/* The library version as "MAJOR.MINOR.PATCH" (== QUANTA_VERSION_STRING), for
 * when only the linked runtime is at hand rather than the headers — e.g. the
 * CLI's --version. */
const char *quanta_version(void);

/* Opaque emulator instance. Create with quanta_create(), free with
 * quanta_destroy(). */
typedef struct Quanta Quanta;

/* Return code for API calls that can fail. */
typedef enum {
    QUANTA_OK = 0,
    QUANTA_ERR_NOMEM,    /* out of memory */
    QUANTA_ERR_LOAD,     /* could not load the program (detail on stderr) */
    QUANTA_ERR_RANGE,    /* address or register index out of range */
    QUANTA_ERR_INVAL     /* invalid argument (e.g. a bad cache geometry) */
} QuantaStatus;

/* Why the machine is stopped (or QUANTA_RUN if it can still step). */
typedef enum {
    QUANTA_RUN = 0,               /* not halted */
    QUANTA_HALT_EXIT,             /* exit syscall; see quanta_exit_code() */
    QUANTA_HALT_EBREAK,           /* ebreak with no debugger attached */
    QUANTA_HALT_ILLEGAL_INSN,     /* illegal or unimplemented instruction */
    QUANTA_HALT_UNIMP_SYSTEM,     /* unimplemented SYSTEM instruction (a CSR) */
    QUANTA_HALT_UNKNOWN_SYSCALL,  /* ecall with an unimplemented number */
    QUANTA_HALT_MEM_FAULT,        /* access outside mapped memory */
    QUANTA_HALT_STEP_LIMIT        /* quanta_run() hit its instruction cap */
} QuantaHalt;

/* --- lifecycle --- */

/* Allocate an instance with no program loaded. Returns NULL on out-of-memory. */
Quanta *quanta_create(void);

/* Free an instance and its guest memory. A NULL handle is ignored. */
void quanta_destroy(Quanta *q);

/* --- loading --- */

/* Load a static little-endian RV32 ELF executable: size guest memory to its
 * load image plus a stack, copy the segments, set PC to the entry point, and
 * initialise sp to the top of the region. */
QuantaStatus quanta_load_elf(Quanta *q, const char *path);

/* Load a raw code/data image: map `size` bytes at `base`, copy `len` bytes of
 * `image` there, set PC to `entry`, and initialise sp to the top of the region.
 * Useful for embedding hand-assembled programs. */
QuantaStatus quanta_load_image(Quanta *q, uint32_t base, uint32_t size,
                               const void *image, size_t len, uint32_t entry);

/* Resolve a symbol's address by name from the ELF at `path`, writing it to
 * `*addr`. Returns QUANTA_OK if found, QUANTA_ERR_LOAD if the file has no such
 * symbol or no symbol table, QUANTA_ERR_INVAL on a NULL argument. Reads the
 * file directly and needs no loaded instance — a convenience for harnesses and
 * tools (e.g. the CLI's --signature dump locating begin_signature/end_signature
 * for the RISC-V architectural tests). */
QuantaStatus quanta_elf_symbol(const char *path, const char *name,
                               uint32_t *addr);

/* --- optional cache model --- */

/* Attach a set-associative LRU cache over data accesses (geometry as the
 * --cache flag: all powers of two, size = sets*ways*block). A pure
 * observability layer — it never changes results. */
QuantaStatus quanta_enable_cache(Quanta *q, uint32_t size_bytes,
                                 uint32_t assoc, uint32_t block_size);

/* Print the cache hit/miss summary to `out`. No-op if no cache is attached. */
void quanta_cache_report(const Quanta *q, FILE *out);

/* --- execution --- */

/* Execute one instruction. Returns the halt state afterwards (QUANTA_RUN while
 * the machine can still step). */
QuantaHalt quanta_step(Quanta *q);

/* Run until the machine halts or `max_steps` instructions retire (0 selects a
 * default runaway cap). The retired-instruction count is stored in *steps_out
 * when it is non-NULL. */
QuantaHalt quanta_run(Quanta *q, uint64_t max_steps, uint64_t *steps_out);

/* --- introspection --- */

uint32_t quanta_reg(const Quanta *q, int i);        /* x0..x31; 0 for x0/oob */
void     quanta_set_reg(Quanta *q, int i, uint32_t value);
uint32_t quanta_pc(const Quanta *q);
void     quanta_set_pc(Quanta *q, uint32_t pc);

/* Read/write guest memory without disturbing CPU fault state. Returns
 * QUANTA_ERR_RANGE if [addr, addr+len) leaves the mapped region. */
QuantaStatus quanta_mem_read(const Quanta *q, uint32_t addr,
                             void *dst, size_t len);
QuantaStatus quanta_mem_write(Quanta *q, uint32_t addr,
                              const void *src, size_t len);

/* Read a little-endian 32-bit word (e.g. an instruction). Returns 0 when `addr`
 * is out of range; pass `ok` (non-NULL) to tell a real 0 from out-of-range. */
uint32_t quanta_read_u32(const Quanta *q, uint32_t addr, int *ok);

/* Current halt reason, and when relevant the exit status / faulting address. */
QuantaHalt quanta_halt_reason(const Quanta *q);
uint32_t   quanta_exit_code(const Quanta *q);
uint32_t   quanta_fault_addr(const Quanta *q);

/* Bounds of the mapped guest region. */
uint32_t quanta_mem_base(const Quanta *q);
uint32_t quanta_mem_size(const Quanta *q);

/* Human-readable name for a halt state. */
const char *quanta_halt_str(QuantaHalt h);

/* ABI name for register x`i` ("zero","ra","sp",...); "?" if out of range. */
const char *quanta_reg_name(int i);

/* Print all registers and PC to `out` with ABI names, for diagnostics. */
void quanta_dump_regs(const Quanta *q, FILE *out);

#ifdef __cplusplus
}
#endif

#endif /* QUANTA_H */
