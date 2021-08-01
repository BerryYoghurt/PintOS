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

static void syscall_handler (struct intr_frame *);
static void validate_filename (char *);
static void validate_four_bytes (void*);
static void validate_buffer (const void*, unsigned);
static struct file *find_file (int);
static list_predicate_func file_with_descriptor;

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
  /*Any invalid address here shall be caught in page_fault safely
  because no locks are held and no dynamic memory is allocated*/
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
  default:
    printf("Undefined system call %d\n",intr_number);
    break;
  }
}

static void
validate_four_bytes(void *ptr)
{
  uint32_t *pd = thread_current ()->pagedir;
  bool valid = ptr < PHYS_BASE && pagedir_get_page(pd, ptr) != NULL;
  ptr = (char*)ptr + 3;
  valid = valid && ptr < PHYS_BASE && pagedir_get_page(pd, ptr) != NULL;
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
  //TODO, add the executable to the files opened by the thread to deny writes
  tid_t t = process_execute(file);
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
  return ret;
}

bool 
syscall_remove (char *file)
{
  validate_filename (file);
  lock_acquire (&filesys_lock);
  bool ret = filesys_remove (file);
  lock_release (&filesys_lock);
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

  if(f == NULL)
    return -1;

  f->descriptor = list_size (&t->opened_files) + 2; //for now
  list_push_back (&t->opened_files, &f->elem);
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

static struct file *
find_file (const int fd)
{
  struct thread * t = thread_current ();
  struct list_elem *e = list_find (&t->opened_files,
                                    file_with_descriptor,
                                    (void*)fd);
  if(e == NULL)
    return NULL;
  return list_entry (e, struct file, elem);
}

/* Predicate to pick struct file with descriptor equal to AUX */
static bool
file_with_descriptor (const struct list_elem * e, void *aux)
{
  struct file *f = list_entry (e, struct file, elem);
  return f->descriptor == (int) aux;
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
    validate_buffer (buffer, length);

    if(fd == 0)
    {
      char * buf = buffer;
      for (unsigned i = 0; i < length; i++)
      {
        //should I put a lock here? can 2 processes get input at the same time??
        buf[i] = input_getc ();
      }
      return length;
    }
    else
    {
      struct file *f = find_file (fd);
      if(f == NULL)
        thread_exit ();
      lock_acquire (&filesys_lock);
        int ret = file_read (f, buffer, length);
      lock_release (&filesys_lock);
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
    validate_buffer (buffer, length);

    if(fd == 1)
    {
      /*not restricting the size of buffer for the time being*/
      putbuf((char*)buffer, length);
      return (int)length;
    }
    else
    {
      struct file *f = find_file (fd);
      if(f == NULL)
        thread_exit ();
      lock_acquire (&filesys_lock);
        int ret = file_write (f, buffer, length);
      lock_release (&filesys_lock);
      return ret;
    }
  }
}

static void
validate_buffer (const void *buffer, unsigned int length)
{
    if(buffer == NULL)
      thread_exit();
      
    const char *temp = (char*)buffer;
    uint32_t *pd = thread_current ()->pagedir;

    while(temp < (char*)buffer+length)
    {
      if(temp >= (char*)PHYS_BASE || pagedir_get_page(pd,temp) == NULL)
      {
        thread_exit();
      }
      temp = (char*)pg_round_up((void*)temp + 1);
    }
}

void syscall_seek (int fd, unsigned position){
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

void
validate_filename (char * file)
{
  if(file == NULL)
    thread_exit();
  char* temp = file - 1;
  uint32_t *pd = thread_current ()->pagedir;
  do{
    temp++;
    if((void*)temp >= PHYS_BASE || pagedir_get_page(pd, (void*)temp) == NULL){
      thread_exit();
    }
  }while(*temp != '\0');
}