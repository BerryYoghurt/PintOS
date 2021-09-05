#ifndef VM_SUPP_TABLE_H
#define VM_SUPP_TABLE_H
#include "userprog/pagedir.h"
#include "threads/palloc.h"
#include <stdint.h>

/* The supplementary page table has the following top-level format:
   One page stores the number of entries currently in the table and references to
   the other pages that store the actual entries.
   The other pages (those referenced by the "supplementary page directory")
   store one uint32_t per pte.
   
   The values stored per pte should be interpreted as follows:
   if the   */

enum pte_type {
    EXEC_DATA,      /* Page of the executable that has (some) nonzero data */
    EXEC_ZERO,      /* Page of the executable that is all zero */
    MMAP_FILE,      /* Page of a memory mapped file */
    STACK           /* Page of the stack */
}

uint32_t *supp_add_entry (uint32_t);
uint32_t supp_get_entry (uint32_t *);

#endif