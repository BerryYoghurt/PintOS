#include "frame-table.h"
#include <debug.h>
#include "../threads/malloc.h"
#include "userprog/pagedir.h"
#include "threads/vaddr.h"
#include "threads/pte.h"
#include "vm/supp-table.h"
#include "swap.h"

#define CLOCK_LOOP(COND, ACT)               \
        hash_first (&i, &frame_table);      \
        while (hash_next (&i) && !found)    \
        {                                   \
            to_evict = hash_entry (hash_cur(&i), struct fte, hash_elem);           \
            if(!to_evict->pinned)           \
            {                               \
                if(COND)                    \
                    found = true;           \
                else                        \
                    {ACT;}                  \
            }                               \
        }


static struct hash frame_table;
/* When this lock is held, some thread is making changes to the hash table */
static struct lock table_lock;
/* When this lock is held, some thread is making changes to the frames and whether 
pages are present. */
struct lock replacement_lock;

static struct fte *frame_get_address(void *);
static void private_flush (struct fte *);

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


/* Deletes the entry for this kpage from the table, frees the fte,
 sets the pte not present, frees the physical frame, and returns back
 the supplementary entry index to the pte.

 if a process creation fails, then only the page and not the frame should be freed
 because the page should not have been mapped yet*/
void
frame_free (void *kpage)
{
    ASSERT (pg_ofs (kpage) == 0);
    //ASSERT (pagedir_is_destoryed ());
    ASSERT (lock_held_by_current_thread (&replacement_lock));
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

    pagedir_clear_page (frame->pd, frame->virtual_address);

    /* return the supp_idx back */
    pagedir_set_supp (frame->pd, frame->virtual_address, frame->supp_entry);

    /* Free the actual page */
    palloc_free_page (kpage);
    free((void*) frame);
}


/* Evicts a frame, flushes, removes and frees its entry from the frame table,
 and returns its kernel virtual address.
 This kernel virtual address is not associated with any process.
 This frame is effectively pinned as it is not in the frame table to begin with.*/
void *
frame_replace (void)
{
    bool lock_held_here = false;
    if (!lock_held_by_current_thread (&replacement_lock))
    {
        lock_acquire (&replacement_lock);
        lock_held_here = true;
    }
    lock_acquire (&table_lock);
    
    //ensure this function is only called when there are no more available user pages
    ASSERT (palloc_get_multiple (PAL_USER, 1) == NULL);

    struct fte *to_evict = NULL;
    bool found = false;

    /* The improved clock policy (i.e. the one that uses both D and A) 
      It is guaranteed that at least one frame is found because at least 
      the current thread will not access its pages in the meantime*/


    struct hash_iterator i;

    /*find A = 0, D = 0*/
    CLOCK_LOOP (!pagedir_is_accessed (to_evict->pd, to_evict->virtual_address)
            && !pagedir_is_dirty (to_evict->pd, to_evict->virtual_address),)
    /* A = 0, D = 1*/
    CLOCK_LOOP (!pagedir_is_accessed(to_evict->pd, to_evict->virtual_address), 
                pagedir_set_accessed (to_evict->pd, to_evict->virtual_address, false))
    /* A = 1, D = 0*/
    CLOCK_LOOP (!pagedir_is_accessed (to_evict->pd, to_evict->virtual_address)
            && !pagedir_is_dirty (to_evict->pd, to_evict->virtual_address),)
    /* A = 1, D = 1*/
    CLOCK_LOOP (!pagedir_is_accessed(to_evict->pd, to_evict->virtual_address),)

    hash_delete (&frame_table, &to_evict->hash_elem);
    lock_release (&table_lock);

    /* Edit the pte. The order is important. You must mark the pte not present
    before flushing so that no modifications to the page can be done after flushing */
    pagedir_clear_page (to_evict->pd, to_evict->virtual_address);
    private_flush (to_evict);
    pagedir_set_supp (to_evict->pd, to_evict->virtual_address, to_evict->supp_entry);

    if(lock_held_here)
        lock_release (&replacement_lock);

    void *kpage = to_evict->physical_address;
    free ((void*) to_evict);

    return kpage;
}


static void
private_flush (struct fte *fte)
{
    ASSERT (fte != NULL);
    if (pagedir_is_filesys (fte->pd, fte->virtual_address))
    {
        if (pagedir_is_dirty (fte->pd, fte->virtual_address))
        {
            //write to filesys
            struct file_supp *info = (struct file_supp*)supp_get_entry (
                                                            fte->supp_pd,
                                                            fte->supp_entry);
            ASSERT (!(info->executable && pagedir_is_writable (fte->pd, fte->virtual_address)));
            
            /* Must use the kernel virtual address in case the flushing is done
            from the replacement algorithm */
            file_write_at (info->file, fte->physical_address, info->bytes, info->offset);
            pagedir_set_dirty (fte->pd, fte->virtual_address, false);
        }
    }
    else
    {
        if(!swap_out (fte->physical_address, fte->supp_pd, fte->supp_entry))
            PANIC ("Could not swap out stack page!");
    } 
}

/* Flushes the frame depending on its type.
The replacement lock must be held by the current thread*/
void
frame_flush (void *kpage)
{
    ASSERT (lock_held_by_current_thread (&replacement_lock));
    struct fte *fte = frame_get_address (kpage);
    private_flush (fte);
}

/* Creates a new pinned frame with these associations
 and inserts it into the table.
 It also marks the pte present.*/
bool
frame_create (void *kpage, 
              void *upage, 
              bool pin)
{
    ASSERT (pg_ofs(kpage) == 0);
    ASSERT (pg_ofs(upage) == 0);
    struct fte *frame = (struct fte*) malloc (sizeof(struct fte));
    if(frame == NULL)
        return false;
    /* A frame is always created on behalf of the current thread,
    whether due to stack (creation or growth), executable, or page fault.*/
    struct thread *t = thread_current ();
    frame->physical_address = kpage;
    frame->virtual_address = upage;
    frame->pd = t->pagedir;
    frame->supp_pd = t->supp_pagedir;
    frame->pinned = pin;
    frame->supp_entry = pagedir_get_supp (t->pagedir, upage);
    pagedir_set_present (t->pagedir, upage, kpage);
    lock_acquire (&table_lock);
    hash_insert (&frame_table, &frame->hash_elem);
    lock_release (&table_lock);
    return true;
}



/* Obtains the frame table entry corresponding to this physical address */
static struct fte*
frame_get_address (void* addr)
{
    struct fte frame;
    frame.physical_address = addr;
    return hash_entry(hash_find(&frame_table, &frame.hash_elem), struct fte, hash_elem);
}

/* Unpins the frame that has the address KPAGE*/
void
frame_unpin (void *kpage)
{
    kpage = (void*)((uint32_t)kpage & PTE_ADDR);
    struct fte* fte = frame_get_address (kpage);
    ASSERT (fte != NULL);
    ASSERT (fte->pinned);
    fte->pinned = false;
}


/* Searches for the page containing UADDR in PD.
   If it is there, nothing more is done,
   Else it allocates a new frame and sets up the fte and pte.
   The user address must be mapped */
bool
frame_fetch_page (void *uaddr, bool pin)
{
    //TODO: pinned being bool might cause some problems if the pinned
    //frame is shared.. 
    uaddr = (void*)((uint32_t)uaddr & PTE_ADDR);
    /* Frame fetch (whether in syscalls or in page faults) is always
    done on behalf of the current thread. */
    struct thread *t = thread_current ();
    ASSERT (pagedir_is_mapped (t->pagedir, uaddr));

    lock_acquire (&replacement_lock);
    if(pagedir_is_present (t->pagedir, uaddr))
    {
        if(pin)
            frame_get_address (pagedir_get_page (t->pagedir, uaddr))->pinned = true;
    }
    else
    {
        //if sharing is enabled, check if files are shared etc
        void *kpage = palloc_get_page (PAL_USER | PAL_ZERO);
        /* Populate */
        if(pagedir_is_filesys (t->pagedir, uaddr))
        {
            struct file_supp *temp;
            temp = (struct file_supp *)supp_get_entry (t->supp_pagedir,
                                                       pagedir_get_supp (t->pagedir, uaddr));
            ASSERT (temp->bytes <= PGSIZE);

            file_read_at (temp->file, kpage, temp->bytes, temp->offset);

            /* Next time, flush to and fetch from swap */
            if(temp->executable && pagedir_is_writable (t->pagedir, uaddr))
            {
                pagedir_set_swappable (t->pagedir, uaddr);
                supp_set_entry (t->supp_pagedir,
                                pagedir_get_supp (t->pagedir, uaddr),
                                0);
                free ((void*)temp);
                ASSERT (!pagedir_is_filesys (t->pagedir, uaddr));
                ASSERT (pagedir_is_mapped (t->pagedir, uaddr));
            }
        }
        else
        {
            swap_in (kpage, supp_get_entry (t->supp_pagedir,
                                            pagedir_get_supp (t->pagedir, uaddr)));
        }
        if(!frame_create (kpage, uaddr, pin))
            return false;
    }

    lock_release (&replacement_lock);
    return true;
}

void
frame_done ()
{
    ASSERT (hash_empty (&frame_table));
    hash_destroy (&frame_table, NULL);
}