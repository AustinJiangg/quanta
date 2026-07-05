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
#define QUANTA_VERSION_MINOR 2
#define QUANTA_VERSION_PATCH 0
#define QUANTA_VERSION_STRING "0.2.0"

/* The library version as "MAJOR.MINOR.PATCH" (== QUANTA_VERSION_STRING), for
 * when only the linked runtime is at hand rather than the headers — e.g. the
 * CLI's --version. */
const char *quanta_version(void);

/* The most harts the machine can model (SMP, M19). A run uses 1..this many,
 * chosen with quanta_set_harts / the CLI's --harts. */
#ifndef QUANTA_MAX_HARTS
#define QUANTA_MAX_HARTS 8u
#endif

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

/* Configure the machine with `nharts` harts (1..QUANTA_MAX_HARTS) for SMP (M19).
 * Must be called before loading a program. All harts share one memory and one
 * platform; a deterministic round-robin scheduler interleaves them one
 * instruction at a time, and each gets its id in mhartid (and a0 at boot). Returns
 * QUANTA_ERR_INVAL if a program is already loaded or the count is out of range.
 * The default (no call) is 1 hart. Only the direct ELF/image boot brings up all
 * harts; the firmware (--bios) path parks the secondaries. */
QuantaStatus quanta_set_harts(Quanta *q, int nharts);

/* --- loading --- */

/* Load a static little-endian RV32 ELF executable: size guest memory to its
 * load image plus a stack, copy the segments, set PC to the entry point, and
 * initialise sp to the top of the region. */
QuantaStatus quanta_load_elf(Quanta *q, const char *path);

/* As quanta_load_elf, but grow the guest region to at least `min_mem` bytes when
 * the image plus its stack is smaller. The spare RAM lands above the image for
 * the guest to manage (an OS sizing page tables and user pages), and the boot
 * device tree's /memory node reports the true size. `min_mem` == 0 is exactly
 * quanta_load_elf. */
QuantaStatus quanta_load_elf_ex(Quanta *q, const char *path, uint32_t min_mem);

/* Load a raw code/data image: map `size` bytes at `base`, copy `len` bytes of
 * `image` there, set PC to `entry`, and initialise sp to the top of the region.
 * Useful for embedding hand-assembled programs. */
QuantaStatus quanta_load_image(Quanta *q, uint32_t base, uint32_t size,
                               const void *image, size_t len, uint32_t entry);

/* Boot an M-mode firmware that hands off to an S-mode OS — the way a real RISC-V
 * machine boots (OpenSBI, then Linux). Loads the firmware ELF (`bios_path`, e.g.
 * OpenSBI's fw_dynamic build) at its link address and the raw OS image
 * (`kernel_path`, e.g. a Linux `Image`) 2 MiB above RAM base (0x80200000, the
 * qemu 'virt' convention), sizes RAM to at least `min_mem` bytes, builds the boot
 * device tree (placed low with headroom so the firmware can expand it in place),
 * and enters the firmware with a0 = hartid, a1 = DTB, and a2 = a `fw_dynamic_info`
 * descriptor directing it into the OS in S-mode. `bootargs` is the kernel command
 * line placed in the device tree's /chosen node (NULL or "" for none) — e.g.
 * "earlycon=sbi console=ttyS0" for a Linux guest. `initrd_path`, if non-NULL, is
 * a cpio initramfs staged in RAM below the DTB and advertised to the kernel via
 * /chosen linux,initrd-start/-end (the rootfs from which Linux runs its /init).
 * Returns QUANTA_ERR_LOAD if a file cannot be loaded or the RAM is too small for
 * the layout. */
QuantaStatus quanta_load_firmware(Quanta *q, const char *bios_path,
                                  const char *kernel_path, const char *bootargs,
                                  const char *initrd_path, uint32_t min_mem);

/* Resolve a symbol's address by name from the ELF at `path`, writing it to
 * `*addr`. Returns QUANTA_OK if found, QUANTA_ERR_LOAD if the file has no such
 * symbol or no symbol table, QUANTA_ERR_INVAL on a NULL argument. Reads the
 * file directly and needs no loaded instance — a convenience for harnesses and
 * tools (e.g. the CLI's --signature dump locating begin_signature/end_signature
 * for the RISC-V architectural tests). */
QuantaStatus quanta_elf_symbol(const char *path, const char *name,
                               uint32_t *addr);

/* --- optional block device --- */

/* Attach a raw disk image from `path` as the machine's block device, read wholly
 * into memory (so reads and writes hit the buffer; writes do not persist back to
 * the file). A future virtio-mmio block device DMAs against it — e.g. an OS
 * mounting its root filesystem. Returns QUANTA_ERR_LOAD if the file cannot be
 * read, QUANTA_ERR_NOMEM on allocation failure, QUANTA_ERR_INVAL on a NULL
 * argument. Replaces any previously attached disk. */
QuantaStatus quanta_attach_disk(Quanta *q, const char *path);

/* --- console input --- */

/* Deliver a byte to the UART receive path, as if typed on the serial console:
 * the guest reads it from the UART and, with RX interrupts enabled, takes an
 * external interrupt. Returns 1 if accepted, 0 if the receive buffer is still
 * full (hold the byte and retry). The CLI pumps host stdin through here. */
int quanta_uart_input(Quanta *q, uint8_t byte);

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

/* --- snapshot / restore (E10) --- */

/* An opaque, self-contained copy of the whole machine state at a point in time:
 * every hart's registers, CSRs, TLB, and PC; all of guest RAM; the device
 * register files (CLINT/PLIC/UART/virtio) and power-off state; the in-memory
 * disk; and the scheduler cursor and machine-halt state. The observability-only
 * cache is deliberately not captured — it never affects results. Because the
 * round-robin scheduler is fully deterministic, a run is a pure function of a
 * snapshot plus its later console input, so this is the primitive under
 * record/replay and reverse debugging. */
typedef struct QuantaSnapshot QuantaSnapshot;

/* Capture the current machine state as a deep, independent copy. Returns NULL if
 * `q` has no program loaded or on out-of-memory. `q` may run on afterwards and
 * still be restored to this point later. Free with quanta_snapshot_free. */
QuantaSnapshot *quanta_snapshot(const Quanta *q);

/* Restore a snapshot into `q`, replacing its entire state and re-wiring the
 * borrowed pointers to the live objects. `q` must be the same machine the
 * snapshot came from — same guest-RAM and disk sizes — or QUANTA_ERR_INVAL is
 * returned and `q` is left untouched. The cache's hit/miss counters are left as
 * they are (results are unaffected). Returns QUANTA_OK on success. */
QuantaStatus quanta_restore(Quanta *q, const QuantaSnapshot *s);

/* Free a snapshot. A NULL handle is ignored. */
void quanta_snapshot_free(QuantaSnapshot *s);

/* --- introspection --- */

uint64_t quanta_reg(const Quanta *q, int i);        /* x0..x31; 0 for x0/oob */
void     quanta_set_reg(Quanta *q, int i, uint64_t value);
uint64_t quanta_pc(const Quanta *q);
void     quanta_set_pc(Quanta *q, uint64_t pc);

/* The register width of the loaded program: 32 (RV32) or 64 (RV64). 0 if none
 * is loaded. Lets a caller format addresses/registers to the right width. */
int      quanta_xlen(const Quanta *q);

/* Read/write guest memory without disturbing CPU fault state. Returns
 * QUANTA_ERR_RANGE if [addr, addr+len) leaves the mapped region. */
QuantaStatus quanta_mem_read(const Quanta *q, uint64_t addr,
                             void *dst, size_t len);
QuantaStatus quanta_mem_write(Quanta *q, uint64_t addr,
                              const void *src, size_t len);

/* Read a little-endian 32-bit word (e.g. an instruction). Returns 0 when `addr`
 * is out of range; pass `ok` (non-NULL) to tell a real 0 from out-of-range. */
uint32_t quanta_read_u32(const Quanta *q, uint64_t addr, int *ok);

/* Current halt reason, and when relevant the exit status / faulting address. */
QuantaHalt quanta_halt_reason(const Quanta *q);
uint32_t   quanta_exit_code(const Quanta *q);
uint64_t   quanta_fault_addr(const Quanta *q);

/* Bounds of the mapped guest region. */
uint64_t quanta_mem_base(const Quanta *q);
uint64_t quanta_mem_size(const Quanta *q);

/* Physical address of the device tree handed to the guest at boot (in a1), or 0
 * if none was placed (e.g. a raw image load). See dtb.h. */
uint64_t quanta_dtb_addr(const Quanta *q);

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
