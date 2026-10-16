#include <kos.h>
#include <stdio.h>
#include <stdlib.h>
#include "mips.h"

/* VMU Memory Card Support for bleem2 - 128KB Constant Mapping */

#define MC_SIZE 131072
static u8 *mc_buffer = NULL;

void vmu_save_init() {
    mc_buffer = (u8 *)malloc(MC_SIZE);
    if (!mc_buffer) {
        printf("PS1 VMU: CRITICAL ERROR - Out of memory for Card buffer!\n");
        return;
    }
    memset(mc_buffer, 0, MC_SIZE);

    // デフォルトでスロット A1 をチェック
    const char *save_path = "/vmu/a1/BLEEM2.VMS";
    FILE *f = fopen(save_path, "rb");
    if (f) {
        fread(mc_buffer, 1, MC_SIZE, f);
        fclose(f);
        printf("PS1 VMU: Loaded existing save from %s\n", save_path);
    } else {
        printf("PS1 VMU: No existing save found. Initializing new card.\n");
    }
}

int vmu_sync_to_disk() {
    if (!mc_buffer) return -1;

    const char *save_path = "/vmu/a1/BLEEM2.VMS";
    FILE *f = fopen(save_path, "wb");
    if (!f) {
        printf("PS1 VMU: Failed to open %s for writing.\n", save_path);
        return -1;
    }

    fwrite(mc_buffer, 1, MC_SIZE, f);
    fclose(f);
    printf("PS1 VMU: Success! Sync'd memory card to %s\n", save_path);
    return 0;
}

/* PS1 メモリーカード・プロトコルのエミレート */
static int mc_state = 0; // 0=IDLE, 1=SENT_ADDR, etc.
static u32 mc_addr = 0;

u8 vmu_sio_exchange(u8 data) {
    if (!mc_buffer) return 0xFF;

    u8 res = 0x00;

    switch(mc_state) {
        case 0: // Command wait
            if (data == 0x81) { mc_state = 1; res = 0xFF; } // Card Select (0x81 usually returns 0xFF then device ID 0x5A)
            else res = 0xFF;
            break;
        case 1: // Device ID phase
            res = 0x5A; mc_state = 2;
            break;
        case 2: // Device ID phase 2
            res = 0x5D; mc_state = 3;
            break;
        case 3: // Command code
            if (data == 0x52) { mc_state = 4; res = 0x00; } // 'R' (Read)
            else if (data == 0x57) { mc_state = 6; res = 0x00; } // 'W' (Write)
            else mc_state = 0;
            break;
        case 4: // Read: address high
            mc_addr = (data << 8); mc_state = 5; res = 0x00;
            break;
        case 5: // Read: address low
            mc_addr |= data; 
            // セクタ読み出し開始
            mc_state = 0; res = 0x5C; 
            break;
        default: mc_state = 0; break;
    }
    return res;
}
