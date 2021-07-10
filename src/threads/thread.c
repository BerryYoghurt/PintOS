#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#include "threads/malloc.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* List of sleeping threads.*/
static struct list sleeping_threads;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;


/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/*Information needed for sleeping*/
struct sleeping_thread
  {
    struct list_elem elem;
    int priority;
    struct semaphore sem;
    int64_t wake_up_time;
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */
static fxdpoint_t load_avg;     /* Estimate of threads ready to run. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, 
                         const char *name, 
                         int priority, 
                         int nice, 
                         int recent_cpu);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
static list_less_func sleep_cmp;
static thread_action_func mlfqs_calculate_priority;

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  //printf("<thread_init>\n");
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);
  list_init (&sleeping_threads);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  if(!thread_mlfqs){
    init_thread (initial_thread, "main", PRI_DEFAULT, 0, 0);
  }else{
    init_thread (initial_thread, "main", PRI_MAX, 0, 0);
    mlfqs_calculate_priority(initial_thread,NULL);
  }
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
  //printf("</thread_init>\n");
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  //printf("<thread_start>\n");
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
  //printf("</thread_start>\n");
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  //printf("<thread_tick>\n");
  struct thread *t = thread_current ();

  /*wake up all threads that should wake up now or should have waken up before*/
  int64_t now = timer_ticks();
  //no need for semaphore because this function runs in an interrupt context anyway
  struct list_elem *element = list_begin(&sleeping_threads);
  struct sleeping_thread *sleeping_thread;
  while(element != list_end(&sleeping_threads)){

    sleeping_thread = list_entry(element, struct sleeping_thread, elem);

    if(sleeping_thread->wake_up_time <= now){

      sema_up(&(sleeping_thread->sem));
      element = list_remove(element);

    }else{
      break;
    }
  }

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
  
  //printf("</thread_tick>\n");
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  //printf("<thread create>\n");
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  if(!thread_mlfqs){ //not safe to call thread_current in init_thread
    init_thread (t, name, priority, 0, 0);
  }else{
    //since all (nice, recent_cpu) are inherited from parent, no need to recalculate
    init_thread (t,
                 name, 
                 0, 
                 thread_current ()->nice, 
                 thread_current ()->recent_cpu);
    mlfqs_calculate_priority(t,NULL);
  }
  
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);
  thread_yield();
  //printf("thread size = %d\n", sizeof(struct thread));
  //printf("</thread create>\n");

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  //printf("<thread_block>\n");
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
  //printf("</thread_block>\n");
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  //printf("<thread_unblock>\n");
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_push_back (&ready_list, &t->elem);
  t->status = THREAD_READY;
  intr_set_level (old_level);
  //printf("</thread_unblock>\n");
}

/* comparator for sleeping threads
  returns true if waking up time of thread a is before thread b
  or both wake up at the same time but the priority of a is more than b.

  I don't think I must recalculate the priority while the thread is sleeping
  because (in the case of priority donation) the thread will certainly wake up
  within a certain time frame, after which the scheduler sees its elevated priority.
  Same thing for priority changes due to mlfq I guess.*/
bool sleep_cmp(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED){
  struct sleeping_thread* thread_a = 
                  list_entry(a, struct sleeping_thread, elem);

  struct sleeping_thread* thread_b = 
                  list_entry(b, struct sleeping_thread, elem);
  
  return thread_a->wake_up_time < thread_b->wake_up_time ||
           (thread_a->wake_up_time == thread_b->wake_up_time && 
            thread_a->priority > thread_b->priority);
}

/*Adds the thread to sleeping list and blocks it.*/
void
thread_sleep(int64_t sleep_start, int64_t sleep_duration){
  //printf("<thread_sleep>\n");
  //debug_backtrace_all();
  struct sleeping_thread* sleeping_thread = (struct sleeping_thread*) malloc(sizeof (struct sleeping_thread));

  //printf("idle thread id = %d initial thread id = %d \n", idle_thread)
  ASSERT(sleeping_thread != NULL) //what checks should I put here?
  /*thread_sleep is only ever called from timer_sleep, and interrupts should be enabled*/
  ASSERT(intr_get_level() == INTR_ON);
  

  sema_init(&(sleeping_thread->sem),0);
  sleeping_thread->wake_up_time = sleep_start + sleep_duration;
  if(!thread_mlfqs){
    sleeping_thread->priority = thread_current ()->original_priority;
  }else{
    sleeping_thread->priority = thread_current ()->priority;
  }

  ASSERT(thread_current());
  ASSERT(!intr_context());
  //disabling interrupts is more suitable because
  //this list can be modified from the timer interrupt
  enum intr_level old_level = intr_disable();
  list_insert_ordered(&sleeping_threads, &(sleeping_thread->elem), sleep_cmp, NULL);
  intr_set_level(old_level);

  sema_down(&(sleeping_thread->sem));
  //thread woke up
  free(sleeping_thread);
  //printf("</thread_sleep>\n");
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  //printf("<thread_yield>\n");
  struct thread *cur = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread) 
    list_push_back (&ready_list, &cur->elem);
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
  //printf("</thread_yield>\n");
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  //printf("<thread_foreach>\n");
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
  //printf("</thread_foreach>\n");
}

/* Sets the current thread's priority to NEW_PRIORITY.
  Does nothing if mlfqs */
void
thread_set_priority (int new_priority) 
{
  /*the thread is certainly not waiting on any lock (or else it
     would not be able to set its priority), but may be holding locks.
     If it's holding locks, its effective priority should be the maximum
     of held locks and new_priority*/

  ASSERT(thread_current ()->waiting_on == NULL);
  //printf("<thread_set_priority>\n");

  if(!thread_mlfqs){
    int old_effective_priority = thread_current ()->priority;
    thread_current ()->original_priority = new_priority;
    int highest_lock_priority = lock_max(&thread_current ()->locks_held);

    if(highest_lock_priority > new_priority){
      thread_current ()->priority = highest_lock_priority;
    }else{
      thread_current ()->priority = new_priority;
    } 

    if(old_effective_priority > thread_current ()->priority){
      thread_yield();
    }
  }

  //printf("</thread_set_priority>\n");
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}

bool thread_priority_cmp(const struct list_elem *a,
                             const struct list_elem *b,
                             void *aux UNUSED){
  
  struct thread *thread_a = list_entry(a, struct thread, elem);
  struct thread *thread_b = list_entry(b, struct thread, elem);

  return thread_a->priority > thread_b->priority;
}


/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice) 
{
  ASSERT(thread_mlfqs);
  ASSERT(nice <= 20 && nice >= -20);
  thread_current ()->nice = nice;
  mlfqs_calculate_priority(thread_current(),NULL);
  thread_yield();
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) 
{
  return thread_current ()->nice;
}

/* Calculates priority for mlfq*/
void
mlfqs_calculate_priority(struct thread *t, void *aux UNUSED){
  ASSERT(thread_mlfqs);
  int ans = to_int_trunc(to_fxdpoint(PRI_MAX) 
                          - (t->recent_cpu / 4) 
                          + to_fxdpoint(t->nice * 2));
  if(ans < PRI_MIN)
    t->priority = PRI_MIN;
  else if(ans > PRI_MAX)
    t->priority = PRI_MAX;
  else
    t->priority = ans;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  //no problem in this multiplication, most probably will not overflow
  return to_int_round(load_avg*100); 
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  //I am afraid it might overflow
  return to_int_round(thread_current()->recent_cpu * 100);
}
/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  ASSERT(t != NULL);
  ASSERT(t->magic == THREAD_MAGIC);//just so that it would print a message
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t,
             const char *name,
             int priority,
             int nice, 
             int recent_cpu)
{
  //printf("<init_thread>\n");
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  
  if(!thread_mlfqs){
    t->priority = priority;
    t->original_priority = priority;
    list_init(&t->locks_held);
    t->waiting_on = NULL;
  }else{
    t->nice = nice;
    t->recent_cpu = recent_cpu;
  }

  t->magic = THREAD_MAGIC;

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
  //printf("</init_thread>\n");
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else{
    //whether mlfq or round robin, both choose highest effective priority
    struct list_elem *thread_elem = list_min(&ready_list, thread_priority_cmp, NULL);
    list_remove(thread_elem);
    return list_entry (thread_elem, struct thread, elem);
  }
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
