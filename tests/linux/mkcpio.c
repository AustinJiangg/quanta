/*
 * mkcpio.c — a minimal initramfs builder for Quanta's Linux boot (M18).
 *
 * A Linux initramfs is a cpio archive the kernel unpacks into a tmpfs and runs
 * `/init` from. The kernel wants the archive in the "new ASCII" (newc) format —
 * the same one the kernel's own `gen_init_cpio` and `cpio -H newc` emit — so this
 * tool writes exactly that, keeping the whole boot path buildable with just a C
 * compiler (no cpio, no root: a plain `cpio` cannot create the `/dev/console`
 * *device node* the kernel opens as PID 1's stdin/stdout without privileges, but
 * synthesising the header directly can).
 *
 * It packs a fixed, minimal tree — a `/dev` directory, a `/dev/console`
 * character device (major 5, minor 1), and `/init` (the given binary) — which is
 * all a single-program userspace needs. Output goes to stdout:
 *   mkcpio path/to/init > initramfs.cpio
 *
 * The newc entry is a 110-byte ASCII header (13 fixed 8-hex-digit fields after
 * the "070701" magic), then the NUL-terminated name padded to a 4-byte boundary,
 * then the file data likewise padded. The archive ends with a "TRAILER!!!" entry.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* File-type bits (octal), matching <sys/stat.h> S_IF* so we need no headers. */
#define TYPE_DIR  0040000
#define TYPE_CHR  0020000
#define TYPE_REG  0100000

static FILE *out;
static unsigned long ino = 721;   /* any distinct, nonzero starting inode */

static void put_pad(unsigned long written) {
    /* newc pads to a 4-byte boundary; the header is 110 bytes, so name/data
     * padding is computed from the running byte offset. */
    while (written & 3) { fputc(0, out); written++; }
}

/* Emit one newc entry: `name` with `mode` (type|perm), `data`/`len` bytes (NULL
 * for none), and device numbers for a node (0 otherwise). */
static void entry(const char *name, unsigned mode, unsigned nlink,
                  unsigned rmajor, unsigned rminor,
                  const void *data, unsigned long len) {
    unsigned long namesize = (unsigned long)strlen(name) + 1;
    /* 070701, then ino mode uid gid nlink mtime filesize
     * devmajor devminor rdevmajor rdevminor namesize check */
    fprintf(out,
            "070701%08lx%08x%08x%08x%08x%08x%08lx%08x%08x%08x%08x%08lx%08x",
            ino++, mode, 0u, 0u, nlink, 0u, len,
            0u, 0u, rmajor, rminor, namesize, 0u);
    fwrite(name, 1, namesize, out);        /* includes the trailing NUL */
    put_pad(110 + namesize);               /* pad the header+name region */
    if (data && len) {
        fwrite(data, 1, len, out);
        put_pad(len);
    }
}

static void *read_file(const char *path, unsigned long *len_out) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(1); }
    if (fseek(f, 0, SEEK_END) != 0) { perror(path); exit(1); }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { perror(path); exit(1); }
    void *buf = malloc((size_t)n ? (size_t)n : 1);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "mkcpio: cannot read %s\n", path);
        exit(1);
    }
    fclose(f);
    *len_out = (unsigned long)n;
    return buf;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <init-binary> > initramfs.cpio\n", argv[0]);
        return 2;
    }
    out = stdout;

    unsigned long init_len;
    void *init = read_file(argv[1], &init_len);

    /* Names are relative to the rootfs root (no leading slash), the convention a
     * real initramfs uses. /dev must precede /dev/console — the unpacker does not
     * create parent directories. */
    entry("dev", TYPE_DIR | 0755, 2, 0, 0, NULL, 0);
    entry("dev/console", TYPE_CHR | 0600, 1, 5, 1, NULL, 0);   /* the kernel console */
    entry("init", TYPE_REG | 0755, 1, 0, 0, init, init_len);
    entry("TRAILER!!!", 0, 1, 0, 0, NULL, 0);                  /* end of archive */

    free(init);
    if (fflush(out) != 0) { perror("mkcpio: write"); return 1; }
    return 0;
}
