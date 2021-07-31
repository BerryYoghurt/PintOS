#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "lib/kernel/stdio.h"

static void syscall_handler (struct intr_frame *);
static void validate_filename (char *);
static void validate_four_bytes (void*);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
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
      *(bool*)&(f->eax) = syscall_create((char*)args[0],(unsigned)args[1]); /*is this correct?*/
    break;
  case SYS_REMOVE:
      *(bool*)&(f->eax) = syscall_remove((char*)args[0]);
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

  tid_t t = process_execute(file);
  if(t == TID_ERROR)
    return -1;
  return t;
}

int syscall_wait (pid_t child_id)
{
  return process_wait(child_id);
}

bool syscall_create (char *file, unsigned initial_size){
  validate_filename (file);
  //TODO
  return false;
}

bool syscall_remove (char *file){
  validate_filename (file);
  //TODO
  return false;
}

int syscall_open (char *file){
  validate_filename (file);
  //TODO
  return 0;
}

int syscall_filesize (int fd){
  return 0;
}

int syscall_read (int fd, void *buffer, unsigned length){
  return 0;
}

int syscall_write (int fd, const void *buffer, unsigned length)
{
  if(fd == 0)
  {
    printf("Writing to stdin!\n");
    return -1;
  }
  else
  { 
    if(buffer == NULL)
      thread_exit();
      
    const char *temp = (char*)buffer;
    uint32_t *pd = thread_current ()->pagedir;

    while(temp < (char*)buffer+length)
    {
      if(temp >= PHYS_BASE || pagedir_get_page(pd,temp) == NULL)
      {
        thread_exit();
      }
      temp = (char*)pg_round_up((void*)temp);
    }

    if(fd == 1)
    {
      /*not restricting the size of buffer for the time being*/
      putbuf((char*)buffer, length);
      return (int)length;
    }
    else
    {
      return 0;
    }
  }
}
void syscall_seek (int fd, unsigned position){

}
unsigned syscall_tell (int fd){
  return 0;
}
void syscall_close (int fd){

}

void
validate_filename (char * file)
{
  if(file == NULL)
    thread_exit();
  char* temp = file;
  uint32_t *pd = thread_current ()->pagedir;
  do{
    if((void*)temp >= PHYS_BASE || pagedir_get_page(pd, (void*)temp) == NULL){
      thread_exit();
    }
    temp++;
  }while(*temp != '\0');
}