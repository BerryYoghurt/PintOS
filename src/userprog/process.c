#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <list.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "vm/frame-table.h"
#include "vm/supp-table.h"
#include "vm/mmap.h"

/*A graph to keep track of parent-child relationships*/
static struct list process_graph;
static struct lock graph_lock;

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static list_predicate_func child_with_id;

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *fn_copy, *exec_name, *save_ptr, *temp_storage;
  tid_t tid;

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  /*No need to allocate memory for the executable name because
    16 bytes are already reserved for it in the struct thread.*/
  exec_name = strtok_r(fn_copy, " ", &save_ptr);
  temp_storage = (char*)malloc(strlen(exec_name)+1);
  if(temp_storage == NULL)
    return TID_ERROR;
  strlcpy(temp_storage, exec_name, strlen(exec_name)+1);
  strlcpy(fn_copy, file_name, PGSIZE);
  
  /* Create a new thread to execute FILE_NAME. */
  tid = thread_create (temp_storage, PRI_DEFAULT, start_process, fn_copy);
  free((void*)temp_storage);

  /*This page will be deallocated whether the loading is successful or not.
    If thread creation is successfull, it will be deallocated in start_process.
    Else, it will be deallocated here.*/
  if (tid == TID_ERROR)
  {
    palloc_free_page (fn_copy);
    return TID_ERROR;
  } 

  /*wait on the semaphore to ensure that the process started successfully*/
  struct child *chld = list_entry(
                            list_find(&thread_current ()->as_parent->children,
                                      child_with_id, (void*)tid),
                            struct child,
                            elem);
  sema_down(&chld->sema);

  if (chld->state == IN_CREATION) //loading failed
  {
    sema_down (&chld->sema); //wait till child exits
    list_remove (&chld->elem);
    ASSERT (list_find(&thread_current ()->as_parent->children,
                        child_with_id, (void*)tid) == NULL);
    free(chld);
    return TID_ERROR;
  }

  return tid;
}

/*Predicate to find a child in a list of children*/
bool
child_with_id(const struct list_elem *e, void *aux){
  int id = (int)(aux);
  struct child *c = list_entry(e, struct child, elem);
  return c->child_id == id;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  char *file_name = file_name_;
  struct intr_frame if_;
  bool success;
  struct thread * t = thread_current();

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (file_name, &if_.eip, &if_.esp);

  /* If load failed, quit. */
  palloc_free_page (file_name);

  /*creation done*/
  if (!success){
    sema_up(&t->as_child->sema);
    thread_exit ();
  }else{
    t->as_child->state = RUNNING;
    sema_up(&t->as_child->sema);
  }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{
  struct list_elem *e = list_find(&thread_current ()->as_parent->children,
                                      child_with_id, (void*)child_tid);
  if(e == NULL)
  {
    /*This could happen if child_tid is not a child of this process,
    or it has been waited for before.*/
    return -1;
  }
  struct child *c = list_entry(e, struct child, elem);
  sema_down(&c->sema);
  int status = c->status;
  list_remove(&c->elem);
  free(c);
  return status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd, **supp_pagedir;

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  supp_pagedir = cur->supp_pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd, supp_pagedir); //TODO destroy here the supplementary ptes
      
      /*Question: why do we need to deny write for the whole running duration
        not just for the loading duration? virtual memory? YES*/
      file_close (cur->executable); //file cannot be nonnull if pagedir is nonnull (see load)
      
      //printf must be here so that even if kernel kills the process, it will print
      printf("%s: exit(%d)\n",cur->name,cur->as_child->status);
    }

  
  /* Free memory mapped files */
  struct list *l = &cur->mmapped_files;
  while(!list_empty (l))
  {
    struct list_elem *e = list_pop_front (l);
    struct mapping *m = list_entry (e, struct mapping, elem);
    file_close (m->file);
    free((void*)m);
  }

  /* Free opened files */
  l = &cur->opened_files;
  while (!list_empty (l))
  {
    struct list_elem *e = list_pop_front (l);
    struct file *f = list_entry (e, struct file, elem);
    file_close (f);
  }

  /* clean up struct parent */
  l = &cur->as_parent->children;
  while (!list_empty (l))
  {
    //add another semaphore to protect child.. not to mark lifetime
    struct list_elem *e = list_pop_front (l);
    struct child *c = list_entry (e, struct child, elem);
    sema_down (&c->protection);
    switch (c->state)
    {
    case IN_CREATION: //failed creation
      NOT_REACHED ()
      break;
    case RUNNING:     //Thanks I don't need you anymore
      c->state = ZOMBIFIED;
      sema_up (&c->protection);
      break;
    case TERMINATED:  //I didn't pick up its return value
      free (c);
      break;
    case ZOMBIFIED:
      NOT_REACHED ()
      break;
    default:
      NOT_REACHED ()
    }
  }

  /* remove the struct parent for this thread */
  list_remove (&cur->as_parent->elem);
  free(cur->as_parent);

  /* clean up struct child */
  if (cur->as_child == NULL) //initial thread is not a child of anybody
    return;

  sema_down (&cur->as_child->protection);
  switch (cur->as_child->state)
  {
  case IN_CREATION:
    //parent should free me
    sema_up (&cur->as_child->protection); //modification of child OK
    sema_up (&cur->as_child->sema);   //lifecycle [termination]
    break;
  case RUNNING:
    cur->as_child->state = TERMINATED;
    sema_up (&cur->as_child->protection);
    sema_up (&cur->as_child->sema);
    break;
  case TERMINATED:
    NOT_REACHED ()
  case ZOMBIFIED:
    free (&cur->as_child);
    break;
  default:
    NOT_REACHED ()
  }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/*Initializes the data structures needed for keeping track of
  which processes are decendents of which*/
void
process_init (void)
{
  list_init(&process_graph);
  lock_init(&graph_lock);
  struct parent *p = (struct parent *)malloc(sizeof (struct parent));
  if(p != NULL){
    list_init(&p->children);
    thread_current ()->as_parent = p;
    thread_current ()->as_child = NULL;
    list_push_back(&process_graph, &p->elem);
  }else{
    PANIC("initial thread parent struct not allocated\n");
  }
}


/*
  This function is called by the parent thread to register
  child_thread as its own child (so that it can wait on it if
  the child is a user process), and to register the child
  as a new potential parent (even if the child were a kernel thread,
  it should still be allowed to run child user processes).

  Returns true if the child was registered correctly
  */
bool
process_register_child(struct thread *child_thread, thread_func func)
{
  struct child *c = (struct child*)malloc(sizeof (struct child));
  if(c == NULL)
    return false;

  c->child_id = child_thread->tid;
  c->status = -1;
  if(func == start_process)
    c->state = IN_CREATION;
  else
    c->state = ZOMBIFIED; //the parent will never wait on it
  sema_init(&c->sema, 0);
  sema_init (&c->protection, 1);
  child_thread->as_child = c;

  /* register me as a child of my parent to allow it to wait on me
    Don't register me if I am a kernel thread*/
  if(c->state == IN_CREATION)
  {
    struct parent *p = thread_current ()->as_parent;
    list_push_front(&p->children, &c->elem);  
  }

  /*Register me as an independent parent*/
  struct parent *p = (struct parent*)malloc(sizeof (struct parent));
  if(p == NULL)
    return false;
  
  list_init(&p->children);
  child_thread->as_parent = p;
  lock_acquire(&graph_lock);
    list_push_back(&process_graph, &p->elem);
  lock_release(&graph_lock);

  return true;
}


/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (const char * cmdline, void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  t->supp_pagedir = supp_table_create ();
  process_activate ();

  /* Open executable file. */
  file = filesys_open (t->name); //instead of file_name, which still includes the arguments
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", t->name);
      goto done; 
    }

  t->executable = file;
  file_deny_write (file);

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (file_name, esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  return success;
}

/* load() helpers. */

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  struct thread *t = thread_current ();

  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Allocate file_supp for VM */
      struct file_supp *info = (struct file_supp *)malloc (sizeof(struct file_supp));
      if(info == NULL)
        return false;
      info->bytes = page_read_bytes;
      info->file = file;
      info->offset = ofs;
      ofs += page_read_bytes;

      /* Create the mapping */
      bool success = !pagedir_is_mapped (t->pagedir, upage)
                     && pagedir_set_page (t->pagedir, 
                                          upage,
                                          writable,
                                          true,
                                          (uint32_t)info);
      
      if(!success)
        return false;

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (const char *cmdline, void **esp) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  ASSERT (kpage != NULL);

  /* Make sure this page was not mapped, create its supplementary entry,
  and then add it to the present pages (frames) */
  struct thread *t = thread_current ();
  success =  !pagedir_is_mapped (t->pagedir, ((uint8_t *) PHYS_BASE) - PGSIZE)
          &&  pagedir_set_page (t->pagedir, 
                                ((uint8_t *) PHYS_BASE) - PGSIZE,
                                true,
                                false,
                                0)
          && frame_create (t->pagedir, kpage, ((uint8_t *) PHYS_BASE) - PGSIZE, true);
                    
  if (success)
  {
    int argc = 0, size = strlen(cmdline)+1;
    char *save_ptr, *token;
    void * top_of_stack = PHYS_BASE;

    /*copy cmdline to the allocated page so that I can split it with strtok_r*/
    token = (char*)top_of_stack - size;
    strlcpy(token, cmdline, size);
    top_of_stack = (void *)token;

    for (token = strtok_r (token, " ", &save_ptr); token != NULL;
                token = strtok_r (NULL, " ", &save_ptr))
      {
        argc++;
        /*store the addresses on the stack temporarily*/
        top_of_stack = (char**)top_of_stack - 1;
        *(char**)top_of_stack = token;
      }

    char* adr[argc];
    for(int i = argc-1; i >= 0; i--)
      {
        adr[i] = *(char**)top_of_stack;
        top_of_stack = (char**)top_of_stack + 1;
      }

    /*word align*/
    if((unsigned)top_of_stack % 4 != 0){
      top_of_stack -= (unsigned)top_of_stack % 4;
    }

    /*push null word*/
    top_of_stack = (int*)top_of_stack - 1;
    *(int*)top_of_stack = 0;

    /*push contents of argv*/
    for(int i = argc-1; i >= 0; i--){
      top_of_stack = (char**)top_of_stack - 1;
      *(char **)top_of_stack = adr[i];
    }
    /*push argv*/
    *((char***)top_of_stack - 1) = top_of_stack;
    top_of_stack = (char***)top_of_stack - 1;
    /*push argc*/
    top_of_stack = (int*)top_of_stack - 1;
    *(int *)top_of_stack = argc;

    /*push dummy return address*/
    top_of_stack = (void **)top_of_stack - 1;
    *(void **)top_of_stack = (void*)0xaaaaaaaa;

    *esp = top_of_stack;
    //hex_dump((uintptr_t)*esp, *esp, 0x50,true);
    frame_unpin (kpage);
  }
  else
    palloc_free_page (kpage);
    
  return success;
}

