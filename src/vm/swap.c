#include "swap.h"
#include "threads/synch.h"
#include "bitmap.h"
#include "threads/vaddr.h"
#include "supp-table.h"

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

struct lock swap_lock;
struct bitmap *bitmap;

/* Initialises the lock and the bitmap. */
void
swap_init ()
{
    struct block *b = block_get_role (BLOCK_SWAP);
    ASSERT (b != NULL);
    lock_init (&swap_lock);
    bitmap = bitmap_create ( block_size (b) / SECTORS_PER_PAGE );
}

/* Frees the memory used by the bitmap. */
void
swap_done ()
{
    bitmap_destroy (bitmap);
}

/* Writes the stack page at KPAGE to a swap slot and puts 
 the index of the swap slot in the supplementary page table SUPP_PAGEDIR
 at SUPP_IDX.
 
 Retruns true if everything successful*/
bool
swap_out (void *kpage, uint32_t ** supp_pagedir, uint32_t supp_idx)
{
    ASSERT(pg_ofs(kpage) == 0);

    lock_acquire (&swap_lock);
    uint32_t idx = bitmap_scan_and_flip (bitmap, 0, 1, false);
    lock_release (&swap_lock);

    if(idx == BITMAP_ERROR)
        return false;
    
    struct block *b = block_get_role (BLOCK_SWAP);
    uint32_t start = idx * SECTORS_PER_PAGE;
    for(int i = 0; i < SECTORS_PER_PAGE; i++)
        block_write (b, start + i, kpage + BLOCK_SECTOR_SIZE * i);

    supp_set_entry (supp_pagedir, supp_idx, idx);
    return true;
}


/* Reads in the swappable page in swap slot IDX to the page at KPAGE. */
void
swap_in (void *kpage, uint32_t idx)
{
    ASSERT (bitmap_test (bitmap, idx));

    struct block *b = block_get_role (BLOCK_SWAP);
    uint32_t start = idx * SECTORS_PER_PAGE;
    for(int i = 0; i < SECTORS_PER_PAGE; i++)
        block_read (b, start + i, kpage + BLOCK_SECTOR_SIZE * i);

    // no need for lock here.. no race condition can occur because this bit is set
    // anyway and only this thread is freeing it
    bitmap_reset (bitmap, idx);
}


void
swap_free (uint32_t idx)
{
    ASSERT (bitmap_test (bitmap, idx));
    bitmap_reset (bitmap, idx);
}