#include "mips.h"
#include <string.h>

/*
 * SIO0 (controller / memory card port) at 0x1F801040-0x1F80104F.
 * Emulates a single digital pad in port 1. No memory card (card commands
 * get no /ACK, so the BIOS reports "no card").
 */

u32 g_sio_first[256];
static struct {
    u8  rx;
    int rx_full;
    u16 mode, ctrl, baud;
    int state;
    int ack;                /* /ACK asserted after the last byte */
    int irq_delay;          /* cycles until the ACK interrupt fires */
} sio;

void sio_init(void) {
    memset(&sio, 0, sizeof(sio));
}

static void sio_transfer(mips_t *cpu, u8 tx) {
    int ack = 0;
    u8 rx = 0xFF;

    extern u16 g_pad_state2;
    int port2 = (sio.ctrl & 0x2000) != 0;
    u16 pad = port2 ? g_pad_state2 : (u16)cpu->pad_state;
    if (!(sio.ctrl & 0x2)) {            /* /JOYn not selected */
        sio.state = 0;
    } else {
        switch (sio.state) {
            case 0:
                g_sio_first[tx & 0xFF]++;
                if (tx == 0x01) { sio.state = 1; ack = 1; }     /* controller access */
                else if (MEMCARD_ENABLED && !port2 && tx == 0x81) { sio.state = 10; ack = 1; memcard_reset(); }   /* memory card */
                else sio.state = 0;
                break;
            case 10:
                rx = memcard_transfer(tx, &ack);
                if (!ack) sio.state = 0;
                break;
            case 1:
                if (tx == 0x42) { rx = 0x41; sio.state = 2; ack = 1; }  /* digital pad id */
                else sio.state = 0;
                break;
            case 2:
                rx = 0x5A; sio.state = 3; ack = 1;
                break;
            case 3:
                rx = pad & 0xFF; sio.state = 4; ack = 1;
                break;
            case 4:
                rx = (pad >> 8) & 0xFF; sio.state = 0; ack = 0;
                break;
            default:
                sio.state = 0;
                break;
        }
    }

    sio.rx = rx;
    sio.rx_full = 1;
    sio.ack = ack;
    if (ack && (sio.ctrl & 0x1000)) sio.irq_delay = 200;
}

void sio_update(mips_t *cpu, u32 cycles) {
    if (sio.irq_delay > 0) {
        if ((int)cycles >= sio.irq_delay) {
            sio.irq_delay = 0;
            cpu->i_stat |= (1u << 7);
        } else {
            sio.irq_delay -= cycles;
        }
    }
}

u32 sio_read(mips_t *cpu, u32 paddr) {
    switch (paddr & 0xF) {
        case 0x0: {                                   /* JOY_DATA */
            u8 v = sio.rx_full ? sio.rx : 0xFF;
            sio.rx_full = 0;
            return v;
        }
        case 0x4: {                                   /* JOY_STAT */
            u32 st = 0x5;                              /* TX ready / TX done */
            if (sio.rx_full) st |= 0x2;
            if (sio.ack) { st |= 0x80; sio.ack = 0; }
            return st;
        }
        case 0x8: return sio.mode;
        case 0xA: return sio.ctrl;
        case 0xE: return sio.baud;
    }
    return 0;
}

void sio_write(mips_t *cpu, u32 paddr, u32 val, int width) {
    (void)width;
    switch (paddr & 0xF) {
        case 0x0: sio_transfer(cpu, (u8)val); break;
        case 0x8: sio.mode = val & 0xFFFF; break;
        case 0xA:
            sio.ctrl = val & 0xFFFF;
            if (val & 0x40) { sio.state = 0; sio.rx_full = 0; sio.ack = 0; sio.ctrl = 0; memcard_reset(); }
            if (!(val & 0x2)) { sio.state = 0; memcard_reset(); }   /* deselect resets the pad / card */
            if (val & 0x10) sio.ack = 0;                    /* acknowledge */
            break;
        case 0xE: sio.baud = val & 0xFFFF; break;
    }
}
