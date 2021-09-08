#ifndef VM_SWAP_H
#define VM_SWAP_H
#include "devices/block.h"
#include <stdbool.h>

void swap_init (void);
void swap_done (void);
bool swap_out (void*, uint32_t**, uint32_t);
void swap_in (void*, uint32_t);
void swap_free (uint32_t);

#endif