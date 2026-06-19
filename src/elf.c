#include "elf.h"

#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------------
 * ELF32 on-disk layout.
 *
 * An ELF file begins with a 52-byte header, followed (at e_phoff) by a table
 * of fixed-size program headers. Every multi-byte field is stored in the
 * file's own endianness, recorded in e_ident[EI_DATA]; for RV32I that is
 * little-endian. Rather than overlay C structs (whose padding and the host's
 * endianness we'd have to reason about), we read each field explicitly with
 * the little-endian helpers below — the same byte-at-a-time approach the
 * memory model uses, and host-endianness-independent by construction.
 *
 * Header field offsets (ELF32):
 *   e_ident   0   16 identification bytes (magic, class, data, ...)
 *   e_type    16  u16 object file type (ET_EXEC for a runnable image)
 *   e_machine 18  u16 target ISA (EM_RISCV)
 *   e_entry   24  u32 entry-point virtual address -> initial PC
 *   e_phoff   28  u32 file offset of the program-header table
 *   e_phentsize 42 u16 size of one program-header entry
 *   e_phnum   44  u16 number of program-header entries
 *
 * Program-header field offsets (ELF32, 32 bytes each):
 *   p_type    0  u32 segment type (PT_LOAD = load into memory)
 *   p_offset  4  u32 byte offset of the segment in the file
 *   p_vaddr   8  u32 virtual address to load the segment at
 *   p_filesz  16 u32 bytes present in the file
 *   p_memsz   20 u32 bytes occupied in memory (>= p_filesz; the tail is BSS)
 * ------------------------------------------------------------------------ */

enum {
    EI_NIDENT   = 16,
    ELF_EHSIZE  = 52,  /* size of the ELF32 header                 */
    ELF_PHSIZE  = 32,  /* size of one ELF32 program header         */

    EI_CLASS    = 4,   /* e_ident index: file class                */
    EI_DATA     = 5,   /* e_ident index: data encoding             */
    ELFCLASS32  = 1,   /* 32-bit objects                           */
    ELFDATA2LSB = 1,   /* little-endian                            */

    ET_EXEC     = 2,   /* executable file                          */
    EM_RISCV    = 243, /* RISC-V                                   */
    PT_LOAD     = 1    /* loadable segment                         */
};

/* Little-endian reads from a raw byte buffer; byte 0 is least significant. */
static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0]
         | (uint32_t)p[1] << 8
         | (uint32_t)p[2] << 16
         | (uint32_t)p[3] << 24;
}

static uint16_t rd_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

/* Slurp the whole file at `path` into a freshly malloc'd buffer. On success
 * returns the buffer and writes its length to `*len`; on failure returns NULL
 * (a diagnostic is printed). Caller frees the buffer. */
static uint8_t *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "elf: cannot open %s\n", path);
        return NULL;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "elf: cannot seek %s\n", path);
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if (size < 0) {
        fprintf(stderr, "elf: cannot size %s\n", path);
        fclose(f);
        return NULL;
    }
    rewind(f);

    uint8_t *buf = malloc((size_t)size);
    if (!buf) {
        fprintf(stderr, "elf: out of memory reading %s\n", path);
        fclose(f);
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        fprintf(stderr, "elf: short read on %s\n", path);
        free(buf);
        return NULL;
    }

    *len = (size_t)size;
    return buf;
}

/* Verify the ELF header describes the kind of image we can run: a 32-bit,
 * little-endian, RISC-V executable. Returns 0 if so, -1 otherwise. */
static int check_header(const uint8_t *buf, size_t len) {
    if (len < ELF_EHSIZE) {
        fprintf(stderr, "elf: file too small to be ELF32\n");
        return -1;
    }
    if (buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') {
        fprintf(stderr, "elf: bad magic (not an ELF file)\n");
        return -1;
    }
    if (buf[EI_CLASS] != ELFCLASS32) {
        fprintf(stderr, "elf: not a 32-bit object (need ELFCLASS32)\n");
        return -1;
    }
    if (buf[EI_DATA] != ELFDATA2LSB) {
        fprintf(stderr, "elf: not little-endian (need ELFDATA2LSB)\n");
        return -1;
    }
    if (rd_u16(buf + 16) != ET_EXEC) {
        fprintf(stderr, "elf: not an executable (need ET_EXEC; build static "
                        "with -nostdlib -nostartfiles -Ttext=...)\n");
        return -1;
    }
    if (rd_u16(buf + 18) != EM_RISCV) {
        fprintf(stderr, "elf: wrong machine (need EM_RISCV)\n");
        return -1;
    }
    return 0;
}

int elf_load(const char *path, Memory *mem, uint32_t *entry) {
    size_t len;
    uint8_t *buf = read_file(path, &len);
    if (!buf) {
        return -1;
    }

    int rc = -1;
    if (check_header(buf, len) != 0) {
        goto out;
    }

    uint32_t phoff     = rd_u32(buf + 28);
    uint16_t phentsize = rd_u16(buf + 42);
    uint16_t phnum     = rd_u16(buf + 44);

    if (phentsize < ELF_PHSIZE) {
        fprintf(stderr, "elf: unexpected program-header size %u\n", phentsize);
        goto out;
    }

    /* Walk the program-header table, loading each PT_LOAD segment. */
    int loaded = 0;
    for (uint16_t i = 0; i < phnum; i++) {
        size_t ph = (size_t)phoff + (size_t)i * phentsize;
        if (ph + ELF_PHSIZE > len) {
            fprintf(stderr, "elf: program-header table runs past end of file\n");
            goto out;
        }

        const uint8_t *p = buf + ph;
        if (rd_u32(p + 0) != PT_LOAD) {
            continue; /* not a loadable segment; skip it */
        }

        uint32_t p_offset = rd_u32(p + 4);
        uint32_t p_vaddr  = rd_u32(p + 8);
        uint32_t p_filesz = rd_u32(p + 16);
        uint32_t p_memsz  = rd_u32(p + 20);

        /* The file bytes we're about to copy must actually be in the file. */
        if ((size_t)p_offset + p_filesz > len) {
            fprintf(stderr, "elf: segment file range out of bounds\n");
            goto out;
        }
        /* The destination [p_vaddr, p_vaddr+p_memsz) must fit in guest memory.
         * 64-bit arithmetic keeps the bound check honest near 0xffffffff. */
        if ((uint64_t)p_vaddr < mem->base ||
            (uint64_t)p_vaddr + p_memsz > (uint64_t)mem->base + mem->size) {
            fprintf(stderr,
                    "elf: segment [0x%08x,+0x%x) does not fit in guest memory "
                    "[0x%08x,+0x%x)\n",
                    p_vaddr, p_memsz, mem->base, mem->size);
            goto out;
        }

        /* Copy the file-backed part. The remaining [p_filesz, p_memsz) bytes
         * are BSS; guest memory is zero-initialised at mem_init, so they are
         * already zero and need no explicit clearing. */
        if (p_filesz > 0) {
            mem_load(mem, p_vaddr, buf + p_offset, p_filesz);
        }
        loaded++;
    }

    if (loaded == 0) {
        fprintf(stderr, "elf: no PT_LOAD segments to run\n");
        goto out;
    }

    uint32_t e_entry = rd_u32(buf + 24);
    if (e_entry < mem->base || e_entry >= mem->base + mem->size) {
        fprintf(stderr, "elf: entry 0x%08x outside guest memory\n", e_entry);
        goto out;
    }

    *entry = e_entry;
    rc = 0;

out:
    free(buf);
    return rc;
}
