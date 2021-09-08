#ifndef VM_SUPP_TABLE_H
#define VM_SUPP_TABLE_H
#include "userprog/pagedir.h"
#include "threads/palloc.h"
#include <stdint.h>

/* The supplementary page table has the following top-level format:
   >One page stores the number of entries currently in the table and references to
   the other pages that store the actual entries.
   >It also stores (as the last word) the number of entries currently in the table.
   This assumes that the kernel portion of the process's address space will not be less
   than 2^10 pages (i.e. not less than 4MB, which is quite reasonable) 

   The other pages (those referenced by the "supplementary page directory")
   store one uint32_t per pte.
   
   The values stored per pte should be interpreted as follows:
   if the page is from a file, then the supplementary entry contains a pointer
   to a struct that has the file, the offset, and the number of bytes
   else the supplementary entry contains the index of the swap slot used*/


struct file_supp{
    struct file *file;
    uint32_t offset;    /* The offset from which to read/write */
    uint32_t bytes;     /* The number of bytes to read/write (and pad the rest with zero) */
};

uint32_t **supp_table_create (void);

uint32_t supp_add_entry (uint32_t** supp_pagedir, uint32_t info);
uint32_t supp_get_entry (uint32_t** supp_pagedir, uint32_t idx);
void supp_set_entry (uint32_t ** supp_pagedir, uint32_t idx, uint32_t info);
void supp_destroy (uint32_t **);

#endif