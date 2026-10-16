#ifndef GTE_H
#define GTE_H

#include "mips.h"

void gte_init(mips_t *cpu);
void gte_load_matrix(mips_t *cpu);
void gte_rtps(mips_t *cpu);
void gte_execute(mips_t *cpu, u32 cmd);

#endif
