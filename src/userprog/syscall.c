#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "lib/kernel/stdio.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/input.h"
#include "threads/pte.h"
#include <list.h>
#include "threads/malloc.h"
#include "vm/frame-table.h"
#include "vm/supp-table.h"
#include "vm/mmap.h"

static void syscall_handler (struct intr_frame *);
static void validate_filename (char *);
static void unpin_filename (char *);
static void validate_four_bytes (void*);
static void validate_buffer (const void*, unsigned, const bool);
static void unpin_buffer (const void*, unsigned);
static struct file *find_file (int);
static list_predicate_func file_with_descriptor;
static list_predicate_func mapping_with_id;

static struct lock filesys_lock; /*A lock to be used everywhere the filesystem is 
                                   accessed because I have no idea how it works or what
                                   needs synchronisation and what doesn't*/

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&filesys_lock);
}

static void
syscall_handler (struct intr_frame *f) 
{
  /* If any address here is not mapped, this means that the process is faulty,
  not that I should grow the stack, because this address should have already been
  accessed by the process (and data was put in it)*/
  void* esp = f->esp;
  validate_four_bytes(esp);
  int intr_number = *((int*)esp);
  esp = (int*)esp + 1;
  void* args[3];
  for(int i = 0; i < 3 ; i++, esp = (void**)esp + 1){
    //I think esp should never reach PHYS_BASE because the top of the stack
    //has at least return address, argc, argv; so even if the syscall needed 0 args,
    //these three would be taken instead.
    validate_four_bytes(esp);
    args[i] = *(void**)esp;
  }

  switch (intr_number)
  {
  case SYS_HALT:
      syscall_halt();
    break;
  case SYS_EXIT:
      syscall_exit((int)args[0]);
    break;
  case SYS_EXEC:
      f->eax = syscall_exec((char*)args[0]);
    break;
  case SYS_WAIT:
      f->eax = syscall_wait((pid_t)args[0]);
    break;
  case SYS_CREATE:
      f->eax = syscall_create((char*)args[0],(unsigned)args[1])? 1 : 0;
    break;
  case SYS_REMOVE:
      f->eax = syscall_remove((char*)args[0])? 1 : 0;
    break;
  case SYS_OPEN:
      f->eax = syscall_open((char*)args[0]);
    break;
  case SYS_FILESIZE:
      f->eax = syscall_filesize((int)args[0]);
    break;
  case SYS_READ:
      f->eax = syscall_read((int)args[0], (void*)args[1], (unsigned)args[2]);
    break;
  case SYS_WRITE:
      f->eax = syscall_write((int)args[0], (void*)args[1], (unsigned)args[2]);
    break;
  case SYS_SEEK:
      syscall_seek((int)args[0], (unsigned)args[1]);
    break;
  case SYS_TELL:
      f->eax = syscall_tell((int)args[0]);
    break;
  case SYS_CLOSE:
      syscall_close((int)args[0]);
    break;
  case SYS_MMAP:
      f->eax = syscall_mmap((int)args[0], (void*)args[1]);
      break;
  case SYS_MUNMAP:
      syscall_munmap((mapid_t)args[0]);
      break;
  default:
    printf("Undefined system call %d\n",intr_number);
    break;
  }
}

/* Ensure these addresses are mapped. */
static void
validate_four_bytes(void *ptr)
{
  uint32_t *pd = thread_current ()->pagedir;
  bool valid = ptr < PHYS_BASE && pagedir_is_mapped(pd, ptr);
  ptr = (char*)ptr + 3;
  valid = valid && ptr < PHYS_BASE && pagedir_is_mapped(pd, ptr);
  if(!valid)
    thread_exit();
}

void syscall_halt (void)
{
  shutdown_power_off();
}

void syscall_exit (int status)
{
  struct thread * t = thread_current ();
  t->as_child->status = status;
  /*PROCESS EXIT IS CALLED FROM THREAD EXIT*/
  thread_exit();
  NOT_REACHED()
}

pid_t syscall_exec (char *file)
{
  validate_filename(file); 
  tid_t t = process_execute(file);
  unpin_filename (file);
  if(t == TID_ERROR)
    return -1;
  return t;
}

int syscall_wait (pid_t child_id)
{
  return process_wait(child_id);
}

bool 
syscall_create (char *file, unsigned initial_size)
{
  validate_filename (file);
  lock_acquire (&filesys_lock);
  bool ret = filesys_create (file, initial_size);
  lock_release (&filesys_lock);
  unpin_filename (file);
  return ret;
}

bool 
syscall_remove (char *file)
{
  validate_filename (file);
  lock_acquire (&filesys_lock);
  bool ret = filesys_remove (file);
  lock_release (&filesys_lock);
  unpin_filename (file);
  return ret;
}

int 
syscall_open (char *file)
{
  validate_filename (file);
  struct thread * t = thread_current ();
  lock_acquire (&filesys_lock);
  struct file * f = filesys_open (file);
  lock_release (&filesys_lock);
  unpin_filename (file);

  if(f == NULL)
    return -1;

  if(list_empty (&t->opened_files))
    f->descriptor = 2;
  else
    f->descriptor = 1 + list_entry(list_begin (&t->opened_files), struct file, elem)->descriptor;
  list_push_front (&t->opened_files, &f->elem);
  return f->descriptor;
}

int 
syscall_filesize (int fd)
{
  struct file *f = find_file (fd);
  if(f == NULL)
    thread_exit ();
  lock_acquire (&filesys_lock);
  int ret = file_length (f);
  lock_release (&filesys_lock);
  return ret;
}


/* Predicate to pick struct file with descriptor equal to AUX */
static bool
file_with_descriptor (const struct list_elem * e, void *aux)
{
  struct file *f = list_entry (e, struct file, elem);
  return f->descriptor == (int) aux;
}

static struct file *
find_file (const int fd)
{
  struct thread * t = thread_current ();
  struct list_elem *e = list_find (&t->opened_files, file_with_descriptor, (void*)fd);
  if(e == NULL)
    return NULL;
  return list_entry (e, struct file, elem);
}

int 
syscall_read (int fd, void *buffer, unsigned length)
{
  if(fd == 1)
  {
    thread_exit ();
  }
  else
  {
    validate_buffer (buffer, length, true);

    if(fd == 0)
    {
      char * buf = buffer;
      for (unsigned i = 0; i < length; i++)
      {
        //should I put a lock here? can 2 processes get input at the same time??
        buf[i] = input_getc ();
      }
      unpin_buffer (buffer, length);
      return length;
    }
    else
    {
      struct file *f = find_file (fd);
      if(f == NULL)
        thread_exit ();
      //TODO remove this lock after you understand how inode.data.length works
      lock_acquire (&filesys_lock);
        int ret = file_read (f, buffer, length);
      lock_release (&filesys_lock);
      unpin_buffer (buffer, length);
      return ret;
    }
  }
}

int 
syscall_write (int fd, const void *buffer, unsigned length)
{
  if(fd == 0)
  {
    thread_exit ();
  }
  else
  { 
    validate_buffer (buffer, length, false);

    if(fd == 1)
    {
      /*not restricting the size of buffer for the time being*/
      putbuf((char*)buffer, length);
      unpin_buffer (buffer, length);
      return (int)length;
    }
    else
    {
      struct file *f = find_file (fd);
      if(f == NULL)
        thread_exit ();
      //todo remove lock
      lock_acquire (&filesys_lock);
        int ret = file_write (f, buffer, length);
      lock_release (&filesys_lock);
      unpin_buffer (buffer, length);
      return ret;
    }
  }
}


/* If a process tries to write to read-only page, it should be killed. 
I had thought this could be done in the fault handler (and indeed it can), but
I think it's better to validate that here so that if any other write happens
to a read only page from the kernel I can detect it (because THAT will certainly be a bug)*/
static void
validate_buffer (const void *buffer, unsigned int length, const bool write)
{
    if(buffer == NULL)
      thread_exit();
      
    const char *temp = (char*)buffer;
    uint32_t *pd = thread_current ()->pagedir;

    while(temp < (char*)buffer+length)
    {
      if(temp >= (char*)PHYS_BASE 
      || !pagedir_is_mapped(pd,temp)
      || (write && !pagedir_is_writable (pd, temp)))
      {
        thread_exit();
      }
      
      if(!frame_fetch_page (pd, (void*)((uint32_t)temp & PTE_ADDR), true))
        PANIC ("Frame fetch failed in syscall");

      temp = (char*)pg_round_up((void*)temp + 1);
    }
}

static void
unpin_buffer (const void *buffer, unsigned int length)
{
  const char *temp = (char*)buffer;
  uint32_t *pd = thread_current ()->pagedir;

  while(temp < (char*)buffer+length)
  {
    frame_unpin (pagedir_get_page (pd, temp));

    temp = (char*)pg_round_up((void*)temp + 1);
  }
}

void 
syscall_seek (int fd, unsigned position){
  if(fd == 0 || fd == 1)
    {
      thread_exit ();
    }

  struct file *f = find_file (fd);
  if(f == NULL)
    return;
  f->pos = position;
}

unsigned 
syscall_tell (int fd)
{
  if(fd == 0 || fd == 1)
    {
      thread_exit ();
    }

  struct file *f = find_file (fd);
  if(f == NULL)
    return -1;
  return f->pos;
}

void 
syscall_close (int fd)
{
  if(fd == 0 || fd == 1)
    {
      thread_exit ();
    }

  struct file *f = find_file (fd);
  if(f == NULL)
    thread_exit ();
  list_remove (&f->elem);
  lock_acquire (&filesys_lock);
    file_close (f);
  lock_release (&filesys_lock);
}


mapid_t
syscall_mmap (int fd, void *addr)
{
  int ans = -1, allocated = 0;
  if(addr == (void*)0 || pg_ofs(addr) != 0)
    return -1;

  struct file *old = find_file (fd);
  if(old == NULL)
    return -1;
  
  uint32_t size = file_length (old);
  if(size == 0)
    return -1;

  struct thread *t = thread_current ();
  struct file *file = file_reopen (old);
  bool success = true;
  for (void *curr_pg = addr; (uint32_t)curr_pg < (uint32_t)addr + size; curr_pg += PGSIZE)
  {
    if (curr_pg >= PHYS_BASE || pagedir_is_mapped (t->pagedir, curr_pg))
    {
      success = false;
      goto done;
    }
    struct file_supp *info = (struct file_supp *)malloc(sizeof(struct file_supp));
    if(info == NULL)
    {
      success = false;
      goto done;
    }
    info->file = file;
    info->offset = (uint32_t)curr_pg - (uint32_t)addr;
    info->bytes = (size - info->offset > PGSIZE) ? PGSIZE : size - info->offset;
    if(!pagedir_set_page (t->pagedir, curr_pg, true, true, (uint32_t)info))
    {
      free(info);
      success = false;
      goto done;
    }
    ++allocated;
  }

  struct mapping *m = (struct mapping *) malloc (sizeof(struct mapping));
  if(m == NULL)
  {
    success = false;
    goto done;
  }
  m->file = file;
  m->addr = addr;
  if(list_empty (&t->mmapped_files))
    m->id = ans = 0;
  else
    m->id = ans = 1 + list_entry (list_begin (&t->mmapped_files),
                                  struct mapping,
                                  elem)->id;
  list_push_front (&t->mmapped_files, &m->elem);

  done:
    if(!success)
    {
      for (void *curr_pg = addr; allocated-- ; curr_pg += PGSIZE)
        {
          free((void *)supp_get_entry(t->supp_pagedir, 
                                      pagedir_get_supp(t->pagedir, curr_pg)));
          pagedir_set_unmapped (t->pagedir, curr_pg);
        }   
      file_close (file);
    }

  return ans;
}


static bool
mapping_with_id (const struct list_elem *e, void *aux)
{
  struct mapping *m = list_entry (e, struct mapping, elem);
  return m->id == (int)aux;
}

void
syscall_munmap (mapid_t mapid)
{
  struct thread *t = thread_current ();
  struct list_elem *e = list_find (&t->mmapped_files,
                                    mapping_with_id,
                                    (void*)mapid);
  if(e == NULL)
    return;
  struct mapping *m = list_entry (e, struct mapping, elem);

  uint32_t size = file_length (m->file);
  void *kpage;
  for (void *curr_pg = m->addr; 
      (uint32_t)curr_pg < (uint32_t)m->addr + size; 
      curr_pg += PGSIZE)
    {
      ASSERT (pagedir_is_file (t->pagedir, curr_pg));
      lock_acquire (&replacement_lock);
      if(pagedir_is_present (t->pagedir, curr_pg))
        {
          kpage = pagedir_get_page (t->pagedir, curr_pg);
          frame_flush (kpage);
          frame_free (kpage);
        }
      lock_release (&replacement_lock);
      free((void *)supp_get_entry(t->supp_pagedir, 
                                  pagedir_get_supp(t->pagedir, curr_pg)));
      pagedir_set_unmapped (t->pagedir, curr_pg);
    }

  file_close (m->file);
  list_remove (&m->elem); //TODO.. bug in the way I treat id's!!! what if 1 2 3 4, free 3, and I want 4!!
  free((void*)m);
}


/* Ensure filename is mapped, fetch its pages if not present and pins them.
   Remember to unpin at the end*/
static void
validate_filename (char * file)
{
  if(file == NULL)
    thread_exit();
  char* temp = file - 1;
  uint32_t *pd = thread_current ()->pagedir;
  uintptr_t curr_pg, prev_pg = -1;
  do{
    temp++;
    curr_pg = pg_no (temp);
    if((void*)temp >= PHYS_BASE || !pagedir_is_mapped(pd, (void*)temp)){
      thread_exit();
    }
    if(curr_pg != prev_pg)
    {
      if(!frame_fetch_page (pd, (void*)((uint32_t)temp & PTE_ADDR), true))
        PANIC ("Frame fetch failed in syscall!");
    }
    prev_pg = curr_pg;
  }while(*temp != '\0');
}


/* Unpins the pages occupied by file */
static void
unpin_filename (char *file)
{
  char* temp = file - 1;
  uint32_t *pd = thread_current ()->pagedir;
  uint32_t curr_pg, prev_pg = -1;
  do{
    temp++;
    curr_pg = pg_no (temp);
    if(curr_pg != prev_pg)
    {
      frame_unpin (pagedir_get_page (pd, temp));
    }
    prev_pg = curr_pg;
  }while(*temp != '\0');
}