#ifndef JIT_H
#define JIT_H

#include "mips.h"

void jit_init();
int  jit_execute(mips_t *cpu, mem_t *mem, gpu_t *gpu);
void jit_flush_cache();
void jit_invalidate(uint32_t addr, uint32_t len);

#endif
