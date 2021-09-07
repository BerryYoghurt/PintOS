#include "supp-table.h"
#include "filesys/file.h"
#include "threads/vaddr.h"
#define ENTRIES (PGSIZE/4) /* Entries per page of supplementary page table */

/* Initialises the supplementary page table for this process
    and returns its kaddr.*/
uint32_t **
supp_table_create (void)
{
    return palloc_get_page (PAL_ZERO);
}

static inline uint32_t
supp_get_size (uint32_t **supp_pagedir)
{
    return (uint32_t)supp_pagedir[ENTRIES-1];
}

static inline void
supp_inc_size (uint32_t **supp_pagedir)
{
    supp_pagedir[ENTRIES-1] = (uint32_t*)((uint32_t)(supp_pagedir[ENTRIES-1]) + 1);
}


/* Adds a new entry and returns its index */
uint32_t
supp_add_entry (uint32_t **supp_pagedir, uint32_t info)
{
    uint32_t size = supp_get_size (supp_pagedir);
    uint32_t top_level = size / ENTRIES, low_level = size % ENTRIES;
    if(low_level == 0)
    {
        supp_pagedir[top_level] = palloc_get_page (PAL_ZERO | PAL_ASSERT);
    }
    supp_pagedir[top_level][low_level] = info;
    supp_inc_size (supp_pagedir);
    return size;
}

/* Returns the index of the swap spot (in case of stack) or 
a pointer to the file_info struct (in case of file) this page is to be found in. */
uint32_t
supp_get_entry (uint32_t **supp_pagedir, uint32_t idx)
{
    ASSERT(idx < supp_get_size (supp_pagedir));
    return supp_pagedir[idx / ENTRIES][idx % ENTRIES];
}

/* Changes the contents of the supplementary pte IDX */
void
supp_set_entry (uint32_t ** supp_pagedir, uint32_t idx, uint32_t info)
{
    ASSERT(idx < supp_get_size (supp_pagedir));
    supp_pagedir[idx / ENTRIES][idx % ENTRIES] = info;
}


/* Frees the memory occupied by this suppelemntary page table. */
void
supp_destroy (uint32_t **supp_pagedir)
{
    uint32_t size = supp_get_size (supp_pagedir);
    uint32_t top_level = size / ENTRIES;
    for(uint32_t i = 0; i <= top_level; ++i)
    {
        palloc_free_page ((void*) supp_pagedir[i]);
    }
    palloc_free_page ((void*) supp_pagedir);
}