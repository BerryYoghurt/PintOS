#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "threads/synch.h"

enum child_state 
{
  IN_CREATION,    /* Duration from thread_create to end of start_process*/
  RUNNING,        /* From the end of start_process to process_exit */
  TERMINATED,     /* From process_exit till process_wait (which deallocates the struct) */
  ZOMBIFIED       /* If parent exits without waiting */
};

/*The structs parent and child have to exist independently of both
the parent and the child so that we will need less special cases
if one terminates before the other. It is easier this way.

They also must preserve the parent child relationship so as not to
allow any process to wait on one that is not directly its child*/
struct parent
  {
    struct list children;     /* The only thread that can modify this list is 
      the parent itself so no need for synchronization*/
    //struct hash_elem hash_elem;
    struct list_elem elem;
  };

struct child
  {
    tid_t child_id;
    int status;            /*Termination status*/
    struct semaphore sema; /*semaphore to mark lifetime events of child. Is upped once
      when child creation is complete, and once when child terminates*/
    enum child_state state; 
    struct semaphore protection; /*concurrent changes to child in parent and child*/
    struct list_elem elem;
  };

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (void);
void process_init (void);
bool process_register_child (struct thread *, tid_t);

#endif /* userprog/process.h */
