#ifndef VM_FRAME_TABLE_H
#define VM_FRAME_TABLE_H

#include <hash.h>
#include "../threads/synch.h"
#include "../threads/thread.h"
/* */

struct fte{
    struct hash_elem hash_elem;     /* Element for hash table */
    void* physical_address;         /* Key to use in hash table */
    void* virtual_address;          /* Virtual address of this frame */
    uint32_t *pd;                   /* The page directory this frame is registered in */
    uint32_t **supp_pd;             /* The associated supplementary page directory */
    bool pinned;          /* I changed the semaphore because there is a replacement lock anyway */
    uint32_t supp_entry;            /* The index of the supplementary pte */
};

extern struct lock replacement_lock;

void frame_init (void);
void *frame_replace (void);
void frame_flush (void *kpage);
void frame_free (void *);
bool frame_create (void* kpage, void* upage, bool pin);
void frame_unpin (void*);
bool frame_fetch_page (void*, bool);
void frame_done (void);

#endif