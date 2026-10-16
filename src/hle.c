#include "mips.h"
#include <stdio.h>
#include <string.h>
#include <kos.h>

/*
 * Minimal HLE BIOS, used only when BOOT_AUTOBOOT=1 (no BIOS image).
 * The real BIOS path (OpenBIOS) is preferred; this is a fallback.
 */

#define MAX_FDS 16
static file_t open_files[MAX_FDS];

static void translate_path(const char *ps1_path, char *dc_path) {
    if (strncmp(ps1_path, "cdrom:", 6) == 0) snprintf(dc_path, 128, "/cd/%s", ps1_path + 6);
    else if (strncmp(ps1_path, "bu", 2) == 0) snprintf(dc_path, 128, "/vmu/a1/%s", ps1_path + 3);
    else snprintf(dc_path, 128, "/vmu/a1/%s", ps1_path);
    char *semicolon = strchr(dc_path, ';');
    if (semicolon) *semicolon = '\0';
}

static void get_string_from_mem(mips_t *cpu, mem_t *mem, u32 addr, char *buf, int max_len) {
    for (int i = 0; i < max_len - 1; i++) {
        buf[i] = mem_read8(cpu, mem, addr + i);
        if (buf[i] == '\0') return;
    }
    buf[max_len - 1] = '\0';
}

void hle_syscall(mips_t *cpu) {
    switch (cpu->r[4]) {
        case 1: cpu->cop0[12] &= ~0x404u; break;   /* EnterCriticalSection */
        case 2: cpu->cop0[12] |= 0x404u; break;    /* ExitCriticalSection  */
        default: break;
    }
}

/* Called once per vblank: fill the pad buffers registered via InitPad */
void hle_vblank(mips_t *cpu, mem_t *mem) {
    if (cpu->hle_pad_buf1) {
        u32 a = cpu->hle_pad_buf1;
        mem_write8(cpu, mem, a + 0, 0x00);
        mem_write8(cpu, mem, a + 1, 0x41);
        mem_write8(cpu, mem, a + 2, cpu->pad_state & 0xFF);
        mem_write8(cpu, mem, a + 3, (cpu->pad_state >> 8) & 0xFF);
    }
    if (cpu->hle_pad_buf2) {
        u32 a = cpu->hle_pad_buf2;
        mem_write8(cpu, mem, a + 0, 0xFF);
        mem_write8(cpu, mem, a + 1, 0xFF);
    }
}

void hle_bios_call(mips_t *cpu, mem_t *mem, gpu_t *gpu, u32 pc) {
    u32 func = cpu->r[9];   /* BIOS calls: function number in t1 */
    (void)gpu;
    cpu->hle_calls++;
    cpu->last_bios_call = pc;
    cpu->last_bios_fn = func;
    if (cpu->hle_calls <= 64 || cpu->hle_calls % 1000 == 0)
        dlog("HLE %02lX:%02lX a0=%08lX ra=%08lX", (unsigned long)pc, (unsigned long)func,
             (unsigned long)cpu->r[4], (unsigned long)cpu->r[31]);

    switch (pc) {
    case 0xA0:
        switch (func) {
        case 0x00: { /* open */
            char ps1_path[128], dc_path[128];
            get_string_from_mem(cpu, mem, cpu->r[4], ps1_path, 128);
            translate_path(ps1_path, dc_path);
            file_t fd = fs_open(dc_path, O_RDONLY);
            int handle = -1;
            if (fd != FILEHND_INVALID)
                for (int i = 0; i < MAX_FDS; i++) if (open_files[i] == 0) { open_files[i] = fd; handle = i; break; }
            cpu->r[2] = handle;
            break;
        }
        case 0x02: { /* read */
            int handle = cpu->r[4];
            u32 buf_addr = cpu->r[5] & 0x1FFFFF;
            u32 size = cpu->r[6];
            if (handle >= 0 && handle < MAX_FDS && open_files[handle] && buf_addr + size <= RAM_SIZE)
                cpu->r[2] = fs_read(open_files[handle], &mem->ram[buf_addr], size);
            else cpu->r[2] = -1;
            break;
        }
        case 0x04: { /* close */
            int handle = cpu->r[4];
            if (handle >= 0 && handle < MAX_FDS && open_files[handle]) { fs_close(open_files[handle]); open_files[handle] = 0; }
            cpu->r[2] = handle;
            break;
        }
        case 0x10: cpu->r[2] = cpu->vblank_count; break;          /* GetVblankCount? (unofficial) */
        case 0x13: cpu->r[2] = 0; break;                          /* setjmp */
        case 0x17: { /* strcmp */
            char a[256], b[256];
            get_string_from_mem(cpu, mem, cpu->r[4], a, 256);
            get_string_from_mem(cpu, mem, cpu->r[5], b, 256);
            cpu->r[2] = (u32)strcmp(a, b);
            break;
        }
        case 0x1B: { /* strlen */
            char a[256];
            get_string_from_mem(cpu, mem, cpu->r[4], a, 256);
            cpu->r[2] = strlen(a);
            break;
        }
        case 0x1C: case 0x33: { /* malloc */
            static u32 heap_ptr = 0x80100000;
            u32 size = cpu->r[4];
            cpu->r[2] = heap_ptr;
            heap_ptr += (size + 3) & ~3u;
            if (heap_ptr > 0x801F0000) heap_ptr = 0x80100000;
            break;
        }
        case 0x1F: case 0x34: cpu->r[2] = 1; break;               /* free */
        case 0x25: { u32 c = cpu->r[4]; cpu->r[2] = (c >= 'a' && c <= 'z') ? c - 32 : c; break; }
        case 0x28: { /* bzero */
            u32 target = cpu->r[4] & 0x1FFFFF, size = cpu->r[5];
            if (size <= RAM_SIZE && target <= RAM_SIZE - size) memset(&mem->ram[target], 0, size);
            break;
        }
        case 0x2A: { /* memset */
            u32 target = cpu->r[4] & 0x1FFFFF, size = cpu->r[5];
            if (size <= RAM_SIZE && target <= RAM_SIZE - size) memset(&mem->ram[target], (u8)cpu->r[6], size);
            cpu->r[2] = cpu->r[4];
            break;
        }
        case 0x2B: { /* memcpy */
            u32 dst = cpu->r[4] & 0x1FFFFF, src = cpu->r[5] & 0x1FFFFF, size = cpu->r[6];
            if (size <= RAM_SIZE && dst <= RAM_SIZE - size && src <= RAM_SIZE - size)
                memmove(&mem->ram[dst], &mem->ram[src], size);
            cpu->r[2] = cpu->r[4];
            break;
        }
        case 0x39: cpu->r[2] = cpu->r[4]; break;                  /* InitHeap */
        case 0x3F: { /* printf */
            char fmt[128];
            get_string_from_mem(cpu, mem, cpu->r[4], fmt, 128);
            dlog("[PS1] %s", fmt);
            break;
        }
        case 0x44: cpu->r[2] = 0; break;                          /* FlushCache */
        case 0x70: case 0x71: case 0x72: cpu->r[2] = 0; break;    /* _bu_init/_96_init/_96_remove */
        default: cpu->r[2] = 0; break;
        }
        break;

    case 0xB0:
        switch (func) {
        case 0x00: cpu->r[2] = 1; break;                          /* alloc_kernel_memory */
        case 0x07: case 0x08: case 0x09: case 0x0C: case 0x0D: cpu->r[2] = 1; break; /* events */
        case 0x0B: cpu->r[2] = 1; break;                          /* TestEvent */
        case 0x12: /* InitPad(buf1, len1, buf2, len2) */
            cpu->hle_pad_buf1 = cpu->r[4];
            cpu->hle_pad_buf2 = cpu->r[6];
            cpu->r[2] = 1;
            break;
        case 0x13: cpu->r[2] = 1; break;                          /* StartPad */
        case 0x14: cpu->r[2] = 1; break;                          /* StopPad  */
        case 0x17: break;                                         /* ReturnFromException */
        case 0x18: case 0x19: cpu->r[2] = 0; break;
        case 0x32: { /* open */
            char ps1_path[128], dc_path[128];
            get_string_from_mem(cpu, mem, cpu->r[4], ps1_path, 128);
            translate_path(ps1_path, dc_path);
            file_t fd = fs_open(dc_path, O_RDONLY);
            int handle = -1;
            if (fd != FILEHND_INVALID)
                for (int i = 0; i < MAX_FDS; i++) if (open_files[i] == 0) { open_files[i] = fd; handle = i; break; }
            cpu->r[2] = handle;
            break;
        }
        case 0x34: { /* read */
            int handle = cpu->r[4];
            u32 buf_addr = cpu->r[5] & 0x1FFFFF;
            u32 size = cpu->r[6];
            if (handle >= 0 && handle < MAX_FDS && open_files[handle] && buf_addr + size <= RAM_SIZE)
                cpu->r[2] = fs_read(open_files[handle], &mem->ram[buf_addr], size);
            else cpu->r[2] = -1;
            break;
        }
        case 0x36: { /* close */
            int handle = cpu->r[4];
            if (handle >= 0 && handle < MAX_FDS && open_files[handle]) { fs_close(open_files[handle]); open_files[handle] = 0; }
            cpu->r[2] = handle;
            break;
        }
        case 0x3D: printf("%c", (char)cpu->r[4]); break;          /* putchar */
        case 0x3F: { /* puts */
            char str[256];
            get_string_from_mem(cpu, mem, cpu->r[4], str, 256);
            dlog("[PS1] %s", str);
            break;
        }
        case 0x56: cpu->r[2] = 0x000000C0; break;                 /* GetC0Table */
        case 0x57: cpu->r[2] = 0x000000B0; break;                 /* GetB0Table */
        case 0x5B: cpu->r[2] = 0; break;                          /* ChangeClearPad */
        default: cpu->r[2] = 0; break;
        }
        break;

    case 0xC0:
        cpu->r[2] = 0;
        break;

    default:
        cpu->r[2] = 0;
        break;
    }

    u32 ra = cpu->r[31];
    if (ra == 0 || (ra & 0x1FFFFFFF) >= RAM_SIZE) {
        dlog("HLE: invalid RA %08lX, stopping", (unsigned long)ra);
        cpu->stop = 1;
        return;
    }
    cpu->pc = ra & ~3u;
}
