#include "frame-table.h"
#include <debug.h>
#include "../threads/malloc.h"
#include "userprog/pagedir.h"
#include "threads/vaddr.h"

static struct hash frame_table;
static struct lock table_lock;


/* Obtains the hash of the frame using the physical address */
static unsigned
frame_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
    const struct fte* frame = hash_entry(e, struct fte, hash_elem);
    return hash_int ((unsigned int)frame->physical_address);
}

/* Compares two frames using physical addresses */
static bool
frame_less_func (const struct hash_elem *a,
                 const struct hash_elem *b,
                 void *aux UNUSED)
{
    const struct fte* f1 = hash_entry(a, struct fte, hash_elem);
    const struct fte* f2 = hash_entry(b, struct fte, hash_elem);
    return f1->physical_address < f2->physical_address;
}

/* Initialises the frame table with a number of frames equal
 to the available slots in the user page pool */
void 
frame_init (void/*size_t frame_cnt, uint8_t starting_at*/)
{
    hash_init( &frame_table,
             frame_hash_func,
             frame_less_func,
             NULL);
    lock_init(&table_lock);
    lock_init(&replacement_lock);
}


/* Frees the memory allocated for this frame. 
 This function must only be called from pagedir_destroy, 
 if a process creation fails, then only the page and not the frame should be freed
 because the page should not have been mapped yet*/
void
frame_free (void *kpage)
{
    ASSERT (pagedir_is_destoryed ());
    //When you implement sharing, only free if no other process is
    //using this frame. You should also remove the current process's pd

    struct fte target;
    target.physical_address = kpage;

    lock_acquire(&table_lock);
    struct fte *frame = hash_entry(hash_delete(&frame_table, &target.hash_elem),
                                   struct fte,
                                   hash_elem);
    /*if I can find the frame, then no other thread is using this frame. Because:
    Uses should be either creation, replacement or pinning for kernel usage.
    Creation is irrelevant here.
    Pinning for kernel usage should not be the case because any process is single threaded.
    Replacement: This frame is not being replaced because it was marked present after we
    have acquired the replacement lock*/
    lock_release (&table_lock);

    /*if it were null, then it should not have been present and frame free
     should not have been called!*/
    ASSERT(frame != NULL); 
    free((void*) frame);
}


/* Evicts a frame, removes its entry from the frame table,
 and returns its kernel virtual address. */
void *
frame_replace (void)
{
    return NULL;
}


/* Flushes the frame depending on its type */
void
frame_flush (void *kpage)
{

}

/* Creates a new pinned frame with these associations
 and inserts it into the table.*/
bool
frame_create (uint32_t *pd, void *kpage, void *upage, bool pin)
{
    struct fte *frame = (struct fte*) malloc (sizeof(struct fte));
    if(frame == NULL)
        return false;
    frame->physical_address = kpage;
    frame->virtual_address = upage;
    frame->pd = pd;
    sema_init(&frame->sema, pin?0:1);
    lock_acquire (&table_lock);
    hash_insert (&frame_table, &frame->hash_elem);
    lock_release (&table_lock);
    return true;
}

/* Retruns the frame table entry of the frame which was used
 least recently. Note: the user pool must be full when
 you call this function */
/*struct fte*
frame_get_lru (void)
{
    struct hash_iterator i;
    struct fte* lru = NULL;

    lock_acquire (&table_lock);
    hash_first (&i, &frame_table);
    while (hash_next (&i))
    {
        struct fte* frame = hash_entry (hash_cur(&i), struct fte, hash_elem);
        
    }
    sema_down (&lru->sema);
    //flush_frame (lru);
    sema_up (&lru->sema);
    hash_delete(&frame_table, &lru->hash_elem);
    lock_release (&table_lock);

    return lru;
}*/


/* Obtains the frame table entry corresponding to this physical address */
static struct fte*
frame_get_address (void* addr)
{
    struct fte frame;
    frame.physical_address = addr;
    return hash_entry(hash_find(&frame_table, &frame.hash_elem), struct fte, hash_elem);
}


/* Tries to pin frame. */
void
frame_pin (void *kpage)
{
    struct fte* fte = frame_get_address (kpage);
    return sema_try_down (&fte->sema);
}

void
frame_unpin (void *kpage)
{
    struct fte* fte = frame_get_address (kpage);
    if(sema_try_down (&fte->sema))
        NOT_REACHED ();
    sema_up (&fte->sema);
}

/* Fetches a page if not already there and returns its kernel virtual address. */
void *
frame_fetch_page (uint32_t *pd, void *uaddr, bool pin)
{
    lock_acquire (&replacement_lock);
    void *kpage = pagedir_get_page (pd, uaddr);
    if(kpage == NULL)
    {
        //if sharing is enabled, check type to find read only etc
        kpage = frame_replace ();
        //switch on type, fetch page and put in kpage.. bla bla bla
        frame_create (pd, kpage, uaddr, pin);
    }
    else
        if (pin)
            frame_pin (kpage);
    lock_release (&replacement_lock);
}