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
                            /*TODO remember the caveat of the kernel accessing the frame*/
    struct semaphore sema;          /* Semaphore to protect fte for purposes of eviction */
};

struct lock replacement_lock;

void frame_init (void);
//struct fte* frame_get_lru (void);
void frame_free (void *);
bool frame_create (uint32_t*, void*, void*, bool);
void *frame_replace (void);
void frame_pin (void*);
void frame_unpin (void*);
void *frame_fetch_page (uint32_t*, void*, bool);
void frame_flush (void*);

#endif