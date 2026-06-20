#include "syscall.h"
#include "memory.h"

#include <stdio.h>

/* ABI argument/number registers (RISC-V calling convention). The syscall
 * number travels in a7; arguments in a0..a5; the return value goes back in
 * a0. We only need a0..a2 for the calls implemented so far. */
enum {
    REG_A0 = 10,
    REG_A1 = 11,
    REG_A2 = 12,
    REG_A7 = 17
};

/* RISC-V Linux/newlib generic syscall numbers (the subset we implement). The
 * same numbers are used by the RISC-V newlib port, so bare-metal programs and
 * Linux programs agree on them. */
enum {
    SYS_write      = 64,
    SYS_exit       = 93,
    SYS_exit_group = 94
};

/* write(fd, buf, count): copy `count` bytes from guest memory at `buf` out to
 * the host's stdout (fd 1) or stderr (fd 2). Returns the byte count in a0, or
 * -1 for an unsupported file descriptor. */
static void sys_write(CPU *cpu) {
    uint32_t fd    = reg_read(cpu, REG_A0);
    uint32_t buf   = reg_read(cpu, REG_A1);
    uint32_t count = reg_read(cpu, REG_A2);

    if (fd != 1 && fd != 2) {
        fprintf(stderr, "syscall write: unsupported fd %u\n", fd);
        reg_write(cpu, REG_A0, (uint32_t)-1);
        return;
    }

    FILE *out = (fd == 2) ? stderr : stdout;
    for (uint32_t i = 0; i < count; i++) {
        fputc(mem_read8(cpu->mem, buf + i), out);
    }
    reg_write(cpu, REG_A0, count);
}

/* exit(code) / exit_group(code): record the status and stop the machine. */
static void sys_exit(CPU *cpu) {
    cpu->exit_code   = reg_read(cpu, REG_A0);
    cpu->halt_reason = HALT_EXIT;
    cpu->halted      = 1;
}

void syscall_dispatch(CPU *cpu) {
    uint32_t num = reg_read(cpu, REG_A7);
    switch (num) {
        case SYS_write:
            sys_write(cpu);
            break;
        case SYS_exit:
        case SYS_exit_group:
            sys_exit(cpu);
            break;
        default:
            /* An unimplemented syscall is a bug in the program-vs-emulator
             * contract, not something to paper over: report it and stop. */
            fprintf(stderr, "unknown syscall %u (a7) at pc=0x%08x\n",
                    num, cpu->pc);
            cpu->halt_reason = HALT_UNKNOWN_SYSCALL;
            cpu->halted = 1;
            break;
    }
}
