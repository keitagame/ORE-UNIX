#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

void memory_init(uint32_t mem_lower, uint32_t mem_upper);
void* alloc_page(void);
void free_page(void* addr);

#endif
