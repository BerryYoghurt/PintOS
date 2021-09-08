#include "userprog/pagedir.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "threads/init.h"
#include "threads/pte.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "vm/frame-table.h"
#include "vm/supp-table.h"
#include "vm/swap.h"

static uint32_t *active_pd (void);
static void invalidate_pagedir (uint32_t *);

/* Creates a new page directory that has mappings for kernel
   virtual addresses, but none for user virtual addresses.
   Returns the new page directory, or a null pointer if memory
   allocation fails. */
uint32_t *
pagedir_create (void) 
{
  uint32_t *pd = palloc_get_page (0);
  if (pd != NULL)
    memcpy (pd, init_page_dir, PGSIZE);
  return pd;
}

/* Destroys page directory PD, freeing all the pages it
   references.
   It also destroys the supplementary page table, frees all its pages
   and frees any file_supp structs associated with it. */
void
pagedir_destroy (uint32_t *pd, uint32_t **supp_pagedir) 
{
  uint32_t *pde;

  if (pd == NULL)
    return;

  ASSERT (pd != init_page_dir);
  //if I understand correctly, this only frees the user pages.
  for (pde = pd; pde < pd + pd_no (PHYS_BASE); pde++)
    if (*pde & PTE_P) 
      {
        uint32_t *pt = pde_get_pt (*pde);
        uint32_t *pte;
        
        for (pte = pt; pte < pt + PGSIZE / sizeof *pte; pte++)
        {
          /* Concurrent eviction of pages and exiting of processes opens a big can of worms..
            (NULL pointers (if pd is freed before other process completes eviction)
             Memory leakage (if stack is written to swap because other process has no idea 
             that this process is exiting and this process cannot be sure of anything!))
            It is better to lock here so that I can be sure no other process can
            evict a frame which I think is present. */
          lock_acquire (&replacement_lock);
          if (*pte & PTE_P)
          {
            ASSERT(*pte & PTE_U);
            ASSERT (*pte & PTE_M);

            if(*pte & PTE_FILESYS)
            {
              frame_flush (pte_get_page (*pte));
              frame_free (pte_get_page (*pte));
              free((void*)supp_get_entry (supp_pagedir, (*pte & PTE_ADDR) >> 12));
            }
            else  
              frame_free (pte_get_page (*pte));
          }
          else if(*pte & PTE_M)
          {
            if(*pte & PTE_FILESYS)
            {
              free((void*)supp_get_entry (supp_pagedir, (*pte & PTE_ADDR)>>12));
            }
            else
            {
              swap_free (supp_get_entry (supp_pagedir, (*pte & PTE_ADDR) >> 12));
            }
          }
          lock_release (&replacement_lock);
        }
        palloc_free_page (pt);
      }
  palloc_free_page (pd);
  supp_destroy (supp_pagedir);
}

/* Returns the address of the page table entry for virtual
   address VADDR in page directory PD.
   If PD does not have a page table for VADDR, behavior depends
   on CREATE.  If CREATE is true, then a new page table is
   created and a pointer into it is returned.  Otherwise, a null
   pointer is returned. */
static uint32_t *
lookup_page (uint32_t *pd, const void *vaddr, bool create)
{
  uint32_t *pt, *pde;

  ASSERT (pd != NULL);

  /* Shouldn't create new kernel virtual mappings. */
  ASSERT (!create || is_user_vaddr (vaddr));

  /* Check for a page table for VADDR.
     If one is missing, create one if requested. */
  pde = pd + pd_no (vaddr);
  if (*pde == 0) 
    {
      if (create)
        {
          pt = palloc_get_page (PAL_ZERO);
          if (pt == NULL) 
            return NULL; 
      
          *pde = pde_create (pt);
        }
      else
        return NULL;
    }

  /* Return the page table entry. */
  pt = pde_get_pt (*pde);
  return &pt[pt_no (vaddr)];
}

/* Adds a mapping in page directory PD for user virtual page
   UPAGE with INFO in the corresponding supplementary pte.
   UPAGE must not already be mapped.

   If WRITABLE is true, the new page is read/write;
   otherwise it is read-only.
   Returns true if successful, false if memory allocation
   failed. */
bool
pagedir_set_page (uint32_t *pd, 
                  void *upage, 
                  bool writable, 
                  bool file, 
                  uint32_t info)
{
  uint32_t *pte;

  ASSERT (pg_ofs (upage) == 0);
  ASSERT (is_user_vaddr (upage));
  ASSERT (pd != init_page_dir);

  pte = lookup_page (pd, upage, true);

  if (pte != NULL) 
    {
      ASSERT ((*pte & PTE_P) == 0);
      ASSERT ((*pte & PTE_M) == 0);
      
      /* This pte could have been mapped before and then unmapped by munmap */
      uint32_t supp_idx = (*pte)>>12;
      if(*pte == 0) //never mapped before
        supp_idx = supp_add_entry (thread_current ()->supp_pagedir, info);
      else
        supp_set_entry (thread_current ()->supp_pagedir, supp_idx, info);
      *pte = pte_create_user (supp_idx, writable, file);
      return true;
    }
  else
    return false;
}

/* Looks up the physical address that corresponds to user virtual
   address UADDR in PD.  Returns the kernel virtual address
   corresponding to that physical address, or a null pointer if
   UADDR is unmapped. */
void *
pagedir_get_page (uint32_t *pd, const void *uaddr) 
{
  uint32_t *pte;

  ASSERT (is_user_vaddr (uaddr));
  
  pte = lookup_page (pd, uaddr, false);
  if (pte != NULL && (*pte & PTE_P) != 0)
    return pte_get_page (*pte) + pg_ofs (uaddr);
  else
    return NULL;
}


/*Marks the page present after associating it with this physical address*/
void
pagedir_set_present (uint32_t *pd, const void *upage, const void *kpage)
{
  ASSERT (vtop (kpage) >> PTSHIFT < init_ram_pages);
  ASSERT (pg_ofs (kpage) == 0);
  ASSERT (pg_ofs (upage) == 0);

  uint32_t *pte = lookup_page (pd, upage, false);
  ASSERT (*pte & PTE_M);
  *pte = (*pte & ~PTE_ADDR) | ((uint32_t)vtop(kpage));
  *pte |= PTE_P;
}

bool 
pagedir_is_present (uint32_t *pd, const void *upage)
{
  uint32_t *pte = lookup_page (pd, upage, false);
  ASSERT (*pte & PTE_M);
  return *pte & PTE_P;
}


/* Returns the index of the supplementary page table entry. 
   The address must be mapped but not present. */
uint32_t
pagedir_get_supp (uint32_t *pd, const void *upage)
{
  uint32_t *pte = lookup_page (pd, upage, false);
  ASSERT (pte != NULL);
  ASSERT ((*pte & PTE_P) == 0);
  return (*pte) >> 12;
}


/* Sets the index of the supplementary page table entry. */
void
pagedir_set_supp (uint32_t *pd, const void *upage, uint32_t supp_idx)
{
  uint32_t *pte = lookup_page (pd, upage, false);
  ASSERT (pte != NULL);
  ASSERT ((*pte & PTE_P) == 0);
  *pte = (supp_idx<<12) | (*pte & ~PTE_ADDR);
}

/* Marks user virtual page UPAGE "not present" in page
   directory PD.  Later accesses to the page will fault.  Other
   bits in the page table entry are preserved.
   UPAGE need not be mapped. */
void
pagedir_clear_page (uint32_t *pd, void *upage) 
{
  uint32_t *pte;

  ASSERT (pg_ofs (upage) == 0);
  ASSERT (is_user_vaddr (upage));

  pte = lookup_page (pd, upage, false);
  if (pte != NULL && (*pte & PTE_P) != 0)
    {
      *pte &= ~PTE_P;
      invalidate_pagedir (pd);
    }
}

/* Returns true if the PTE for virtual page VPAGE in PD is dirty,
   that is, if the page has been modified since the PTE was
   installed.
   Returns false if PD contains no PTE for VPAGE. */
bool
pagedir_is_dirty (uint32_t *pd, const void *vpage) 
{
  uint32_t *pte = lookup_page (pd, vpage, false);
  return pte != NULL && (*pte & PTE_D) != 0;
}

/* Set the dirty bit to DIRTY in the PTE for virtual page VPAGE
   in PD. */
void
pagedir_set_dirty (uint32_t *pd, const void *vpage, bool dirty) 
{
  uint32_t *pte = lookup_page (pd, vpage, false);
  if (pte != NULL) 
    {
      if (dirty)
        *pte |= PTE_D;
      else 
        {
          *pte &= ~(uint32_t) PTE_D;
          invalidate_pagedir (pd);
        }
    }
}

/* Returns true if the PTE for virtual page VPAGE in PD has been
   accessed recently, that is, between the time the PTE was
   installed and the last time it was cleared.  Returns false if
   PD contains no PTE for VPAGE. */
bool
pagedir_is_accessed (uint32_t *pd, const void *vpage) 
{
  uint32_t *pte = lookup_page (pd, vpage, false);
  return pte != NULL && (*pte & PTE_A) != 0;
}

/* Sets the accessed bit to ACCESSED in the PTE for virtual page
   VPAGE in PD. */
void
pagedir_set_accessed (uint32_t *pd, const void *vpage, bool accessed) 
{
  uint32_t *pte = lookup_page (pd, vpage, false);
  if (pte != NULL) 
    {
      if (accessed)
        *pte |= PTE_A;
      else 
        {
          *pte &= ~(uint32_t) PTE_A; 
          invalidate_pagedir (pd);
        }
    }
}


/* Returns true if there exits a mapping for vpage in pd, even
   if it's not present in physical memory. */
bool
pagedir_is_mapped (uint32_t *pd, const void *vpage)
{
  uint32_t *pte = lookup_page (pd, vpage, false);
  return pte != NULL && (*pte & PTE_M);
}


/* Unmaps the address vpage. The address must not be present. 
  All other bits stay the same. */
void
pagedir_set_unmapped (uint32_t *pd, const void *vpage)
{
  ASSERT (pd != NULL);
  uint32_t *pte = lookup_page (pd, vpage, false);
  ASSERT (*pte & PTE_M);
  ASSERT ((*pte & PTE_P) == 0);
  ASSERT (*pte & PTE_FILESYS); //we only unmap files
  *pte &= ~(uint32_t)PTE_M;
}


/* Returns true if writable. */
bool
pagedir_is_writable (uint32_t *pd, const void *upage)
{
  uint32_t *pte = lookup_page (pd, upage, false);
  return *pte & PTE_W;
}

/* Should this page be read/written from filesys or from swap? */
bool
pagedir_is_filesys (uint32_t *pd, const void *upage)
{
  uint32_t *pte = lookup_page (pd, upage, false);
  ASSERT (*pte & PTE_M);
  return *pte & PTE_FILESYS;
}

/* Used only for writable segments of an executable. Turns this pte
into a swappable entry (since writable segments of an executable can have
data written into them. However, they are written back to swap and not 
to the original file both because the file is denied writes, and this process
could need this data later. */
void
pagedir_set_swappable (uint32_t *pd, const void *upage)
{
  uint32_t *pte = lookup_page (pd, upage, false);
  ASSERT (*pte & PTE_M);
  ASSERT (!(*pte & PTE_P));
  ASSERT (*pte & PTE_FILESYS);
  ASSERT (*pte & PTE_W);
  *pte &= ~(uint32_t)PTE_FILESYS;
}

/* Loads page directory PD into the CPU's page directory base
   register. */
void
pagedir_activate (uint32_t *pd) 
{
  if (pd == NULL)
    pd = init_page_dir;

  /* Store the physical address of the page directory into CR3
     aka PDBR (page directory base register).  This activates our
     new page tables immediately.  See [IA32-v2a] "MOV--Move
     to/from Control Registers" and [IA32-v3a] 3.7.5 "Base
     Address of the Page Directory". */
  asm volatile ("movl %0, %%cr3" : : "r" (vtop (pd)) : "memory");
}

/* Returns the currently active page directory. */
static uint32_t *
active_pd (void) 
{
  /* Copy CR3, the page directory base register (PDBR), into
     `pd'.
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 3.7.5 "Base Address of the Page Directory". */
  uintptr_t pd;
  asm volatile ("movl %%cr3, %0" : "=r" (pd));
  return ptov (pd);
}

/* Seom page table changes can cause the CPU's translation
   lookaside buffer (TLB) to become out-of-sync with the page
   table.  When this happens, we have to "invalidate" the TLB by
   re-activating it.

   This function invalidates the TLB if PD is the active page
   directory.  (If PD is not active then its entries are not in
   the TLB, so there is no need to invalidate anything.) */
static void
invalidate_pagedir (uint32_t *pd) 
{
  if (active_pd () == pd) 
    {
      /* Re-activating PD clears the TLB.  See [IA32-v3a] 3.12
         "Translation Lookaside Buffers (TLBs)". */
      pagedir_activate (pd);
    } 
}

bool
pagedir_is_destoryed (void)
{
  return active_pd () == init_page_dir;
}