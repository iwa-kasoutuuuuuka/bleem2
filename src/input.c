#include <kos.h>
#include "mips.h"

/*
 * Dreamcast controller -> PS1 digital pad (active low, 16 bits)
 *  bit 0 Select  3 Start  4 Up  5 Right  6 Down  7 Left
 *  bit 8 L2  9 R2  10 L1  11 R1  12 Triangle  13 Circle  14 Cross  15 Square
 */

u16 g_pad_state = 0xFFFF, g_pad_state2 = 0xFFFF;

static u16 read_pad(int index) {
    maple_device_t *cont = maple_enum_type(index, MAPLE_FUNC_CONTROLLER);
    u16 b = 0xFFFF;
    if (cont) {
        cont_state_t *st = (cont_state_t *)maple_dev_status(cont);
        if (st) {
            if (st->buttons & CONT_A)          b &= ~(1 << 14);  /* Cross    */
            if (st->buttons & CONT_B)          b &= ~(1 << 13);  /* Circle   */
            if (st->buttons & CONT_X)          b &= ~(1 << 15);  /* Square   */
            if (st->buttons & CONT_Y)          b &= ~(1 << 12);  /* Triangle */
            if (st->buttons & CONT_START)      b &= ~(1 << 3);
            if (st->buttons & CONT_DPAD_UP)    b &= ~(1 << 4);
            if (st->buttons & CONT_DPAD_RIGHT) b &= ~(1 << 5);
            if (st->buttons & CONT_DPAD_DOWN)  b &= ~(1 << 6);
            if (st->buttons & CONT_DPAD_LEFT)  b &= ~(1 << 7);
            if (st->ltrig > 64)                b &= ~(1 << 10);  /* L1 */
            if (st->rtrig > 64)                b &= ~(1 << 11);  /* R1 */
            if (st->buttons & CONT_D)          b &= ~(1 << 0);   /* Select (D on fishing/arcade pads) */
            if (st->joyy < -64)                b &= ~(1 << 4);
            if (st->joyy >  64)                b &= ~(1 << 6);
            if (st->joyx < -64)                b &= ~(1 << 7);
            if (st->joyx >  64)                b &= ~(1 << 5);
        }
    }
    return b;
}

void input_poll(void) {
    g_pad_state = read_pad(0);
    g_pad_state2 = read_pad(1);          /* second Dreamcast controller -> PS1 port 2 */
}
