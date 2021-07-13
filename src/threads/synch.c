/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

static list_less_func lock_priority_cmp;
static list_less_func waiter_priority_cmp;

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
     decrement it.

   - up or "V": increment the value (and wake up one waiting
     thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) 
{
  //printf("<sema_init>\n");
  ASSERT (sema != NULL);

  sema->value = value;
  list_init (&sema->waiters);
  //printf("</sema_init>\n");
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */
void
sema_down (struct semaphore *sema) 
{
  //printf("<sema_down>\n");
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
    {
      list_push_back (&sema->waiters, &thread_current ()->elem);
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
  //printf("</sema_down>\n");
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) 
{
  //printf("<sema_try_down>\n");
  enum intr_level old_level;
  bool success;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (sema->value > 0) 
    {
      sema->value--;
      success = true; 
    }
  else
    success = false;
  intr_set_level (old_level);
  //printf("</sema_try_down>\n");

  return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (!list_empty (&sema->waiters)){ 
    struct list_elem *thread_elem = list_min(&sema->waiters,
                                             thread_priority_cmp, 
                                             NULL);
    list_remove(thread_elem);
    thread_unblock (list_entry (thread_elem,
                                struct thread, elem));
  }
  sema->value++;
  intr_set_level (old_level);
  if(intr_context ()){
    intr_yield_on_return();
  }else{
    thread_yield();
  }
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) 
{
  struct semaphore sema[2];
  int i;

  printf ("Testing semaphores...");
  sema_init (&sema[0], 0);
  sema_init (&sema[1], 0);
  thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) 
    {
      sema_up (&sema[0]);
      sema_down (&sema[1]);
    }
  printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) 
{
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) 
    {
      sema_down (&sema[0]);
      sema_up (&sema[1]);
    }
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock)
{
  //printf("<lock_init>\n");
  ASSERT (lock != NULL);

  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
  if(!thread_mlfqs)
    sema_init (&lock->protection, 1);
  //printf("</lock_init>\n");
  //lock->id = lock_allocate_id();
}

/*int lock_allocate_id(){
  static id = 1000;
  return id++;
}*/

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock)
{
  //debug_backtrace();
  //printf("<lock acquire>\n");
  //debug_backtrace_all();
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));
  //ASSERT(thread_current() != NULL);

  if(!thread_mlfqs){
    ASSERT (thread_current ()->waiting_on == NULL);
    thread_current ()->waiting_on = lock;
    //printf("call lock_promote");
    lock_promote(lock, thread_current ()->priority); //PROMOTE ONLY IF THERE IS ACTUALLY SOMEONE HOLDING

    sema_down (&lock->semaphore);
    //lock acquired
    lock->holder = thread_current ();
    thread_current ()->waiting_on = NULL;
    list_push_front(&thread_current ()->locks_held, &lock->elem);

    //by definition, this thread now has the highest priority of the waiters,
    //set the lock priority to the thread's ORIGINAL priority
    lock->priority = thread_current ()->original_priority;
  }else{
    sema_down (&lock->semaphore);
    lock->holder = thread_current ();
  }
  //printf("</lock acquire>\n");
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock)
{
  //printf("<lock_try_acquire>\n");
  bool success;

  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));
  ASSERT (thread_current ()->waiting_on == NULL);

  success = sema_try_down (&lock->semaphore);
  if (success){
    lock->holder = thread_current ();

    if(!thread_mlfqs){
      lock->priority = thread_current ()->original_priority;
      list_push_front(&thread_current ()->locks_held, &lock->elem);
    }
  }
  //printf("</lock_try_acquire>\n");
  return success;
}

/* Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) 
{
  //printf("<lock_release>\n");
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));
  ASSERT (thread_current ()->waiting_on == NULL);

  lock->holder = NULL;

  if(!thread_mlfqs){ //tweak priorities

    lock->priority = -1;
    list_remove(&lock->elem);

    int highest_priority_lock = lock_max(&thread_current ()->locks_held);
    if(highest_priority_lock > thread_current ()->original_priority){
      thread_current ()->priority = highest_priority_lock;
    }else{
      thread_current ()->priority = thread_current ()->original_priority;
    }
  }

  
  sema_up (&lock->semaphore);
  //printf("</lock_release>\n");
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);

  return lock->holder == thread_current ();
}

/* Comparator for lock priorities, 
  returns true if lock a has a higher priority*/
bool lock_priority_cmp(const struct list_elem *a, const struct list_elem *b, void* aux UNUSED)
{
  struct lock* lock_a = list_entry(a, struct lock, elem);
  struct lock* lock_b = list_entry(b, struct lock, elem);
  return lock_a->priority > lock_b->priority;
}

/* Returns the priority of thehighest priority lock in a list, 
   or -1 if the list is empty*/
int lock_max(struct list* lock_list)
{
  ASSERT(!thread_mlfqs); //lock priority doesn't make sense otherwise

  struct list_elem* lock_elem = list_min(lock_list, lock_priority_cmp, NULL);
  if(lock_elem != list_tail(lock_list))
    return list_entry(lock_elem, struct lock, elem)->priority;
  else
    return -1;
}

/* Promotes the lock to the new priority if higher than the current,
  and ripples to the holder of the lock, and so on
  TODO, could impose a limit on recusion depth if needed*/
void lock_promote(struct lock *lock, int new_priority)
{
  ASSERT(!thread_mlfqs);
  ASSERT(lock != NULL);

  sema_down(&lock->protection);
  
  if(new_priority > lock->priority){

    lock->priority = new_priority;

    if(lock->holder != NULL && new_priority > lock->holder->priority){

      lock->holder->priority = new_priority;

      if(lock->holder->waiting_on != NULL){
        sema_up(&lock->protection);
        lock_promote(lock->holder->waiting_on, new_priority);
        return;
      }
    }
  }

  sema_up(&lock->protection);

}


/* One semaphore in a list. */
struct semaphore_elem 
  {
    struct list_elem elem;              /* List element. */
    struct semaphore semaphore;         /* This semaphore. */
    int priority;                       /* Waiting thread priority */
  };

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);

  list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) 
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  sema_init (&waiter.semaphore, 0);
  waiter.priority = thread_current ()->priority;
  list_push_back (&cond->waiters, &waiter.elem);
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  if (!list_empty (&cond->waiters)){
    struct list_elem* highest_priority = list_min( &cond->waiters,
                                                   waiter_priority_cmp,
                                                   NULL);
    list_remove(highest_priority);
    sema_up (&list_entry (highest_priority,
                          struct semaphore_elem, elem)->semaphore);
  }
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}

bool waiter_priority_cmp( const struct list_elem *a,
                          const struct list_elem *b,
                          void *aux UNUSED){
  struct semaphore_elem *thread_a = list_entry(a, struct semaphore_elem, elem);
  struct semaphore_elem *thread_b = list_entry(b, struct semaphore_elem, elem);

  return thread_a->priority > thread_b->priority;
}
