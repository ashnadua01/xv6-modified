#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct proc *front(struct Queue *q);

struct proc *front(struct Queue *q);
void pushQueue(struct Queue *q, struct proc *element);
void popQueue(struct Queue *q);

extern void sgenrand(unsigned long);
extern long genrand(void);
extern long random_gen(long);

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct Queue mlfq[MAXNUM];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void frefindProcess(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    char *pa = kalloc();
    if (pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int)(p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void procinit(void)
{
  struct proc *p;

  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    initlock(&p->lock, "proc");
    p->state = UNUSED;
    p->kstack = KSTACK((int)(p - proc));
  }
#ifdef MLFQ
  for (int i = 0; i < MAXNUM; i++)
  {
    mlfq[i].front = 0;
    mlfq[i].back = 0;
    mlfq[i].size = 0;
  }
#endif
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu *
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc *
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int allocpid()
{
  int pid;

  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free processes, or a memory allocation fails, return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == UNUSED)
    {
      goto found;
    }
    else
    {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  p->creationTime = ticks;
  p->totalRunTime = 0;

  // Allocate a trapframe page.
  if ((p->trapframe = (struct trapframe *)kalloc()) == 0)
  {
    frefindProcess(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if (p->pagetable == 0)
  {
    frefindProcess(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  // PBS Related fields
  p->priority = 60;
  p->numberOfRuns = 0;
  p->runTime = 0;

  // MLFQ
  p->priority = 0;
  p->checkQueue = 0;
  p->timeQuantum = 1;
  p->queueCreationTime = ticks;

  for (int i = 0; i < MAXNUM; i++)
  {
    p->queueRunTime[i] = 0;
  }

  // Initialize alarm
  p->alarm_interval = 0;
  p->alarm_passed = 0;
  p->alarm_handler = 0;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
frefindProcess(struct proc *p)
{
  if (p->trapframe)
    kfree((void *)p->trapframe);
  p->trapframe = 0;
  if (p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if (pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if (mappages(pagetable, TRAMPOLINE, PGSIZE,
               (uint64)trampoline, PTE_R | PTE_X) < 0)
  {
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if (mappages(pagetable, TRAPFRAME, PGSIZE,
               (uint64)(p->trapframe), PTE_R | PTE_W) < 0)
  {
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
    0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
    0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
    0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
    0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
    0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
    0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00};

// Set up first user process.
void userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;

  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;     // user program counter
  p->trapframe->sp = PGSIZE; // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if (n > 0)
  {
    if ((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0)
    {
      return -1;
    }
  }
  else if (n < 0)
  {
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy user memory from parent to child.
  if (uvmcopy(p->pagetable, np->pagetable, p->sz) < 0)
  {
    frefindProcess(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // copy trace mask
  np->tracemask = p->tracemask;

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for (i = 0; i < NOFILE; i++)
    if (p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void reparent(struct proc *p)
{
  struct proc *pp;

  for (pp = proc; pp < &proc[NPROC]; pp++)
  {
    if (pp->parent == p)
    {
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void exit(int status)
{
  struct proc *p = myproc();

  if (p == initproc)
    panic("init exiting");

  // Close all open files.
  for (int fd = 0; fd < NOFILE; fd++)
  {
    if (p->ofile[fd])
    {
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);

  acquire(&p->lock);

  p->xstate = status;
  p->exitTime = ticks;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (pp = proc; pp < &proc[NPROC]; pp++)
    {
      if (pp->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if (pp->state == ZOMBIE)
        {
          // Found one.
          pid = pp->pid;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                   sizeof(pp->xstate)) < 0)
          {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          frefindProcess(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || killed(p))
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

int waitx(uint64 addr, int *runTime, int *waitTime)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (np = proc; np < &proc[NPROC]; np++)
    {
      if (np->parent == p)
      {
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if (np->state == ZOMBIE)
        {
          // Found one.
          pid = np->pid;
          *runTime = np->totalRunTime;
          *waitTime = np->exitTime - np->creationTime - np->totalRunTime;
          if (addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                   sizeof(np->xstate)) < 0)
          {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          frefindProcess(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || p->killed)
    {
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock); // DOC: wait-sleep
  }
}

int check(struct proc *p)
{
  if (p->runTime + p->waitTime == 0)
  {
    return 0;
  }
  else
  {
    return 1;
  }
}

int nice_priority(struct proc *p)
{
  int niceness, dp;
  int x = check(p);
  if (x == 0)
  {
    niceness = 5;
  }
  else
  {
    niceness = p->waitTime * 10;
    niceness /= (p->runTime + p->waitTime);
  }
  dp = p->priority - niceness + 5;
  if (dp < 0)
  {
    dp = 0;
  }
  int retVal;
  if (dp > 100)
  {
    retVal = 100;
  }
  else
  {
    retVal = dp;
  }
  return retVal;
}

void set_priority(int priority, int pid, int *old)
{
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      acquire(&p->lock);
      *old = p->priority;
      p->priority = priority;
      p->runTime = 0;
      release(&p->lock);
      if (*old > priority)
        yield();
    }
  }
}

void update_time(void)
{
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->state == RUNNING)
    {
      p->totalRunTime++;
#ifdef PBS
      p->runTime++;
#endif
#ifdef MLFQ
      p->queueRunTime[p->priority]++;
      p->timeQuantum--;
#endif
    }
#ifdef PBS
    else if (p->state == SLEEPING)
    {
      p->waitTime++;
    }
#endif
    release(&p->lock);
  }
  #ifdef MLFQ
    // for(p = proc; p < &proc[NPROC]; p++){
    //   if(p->pid >= 4 && p->pid <= 13){
    //     printf("%d %d %d\n", p->pid, p->priority, ticks);
    //   }
    // }
  #endif
}

void change(struct proc *p, struct cpu *c)
{
#ifdef RR
  release(&p->lock);
#endif
#ifdef FCFS
  c->proc = 0;
  release(&p->lock);
#endif
#ifdef PBS
  c->proc = 0;
  release(&p->lock);
#endif
#ifdef LBS
  c->proc = 0;
  release(&p->lock);
#endif
#ifdef MLFQ
  c->proc = 0;
  p->queueCreationTime = ticks;
  release(&p->lock);
#endif
  return;
}

void execute(struct proc *processToExecute, struct cpu *c)
{
#ifdef FCFS
  processToExecute->state = RUNNING;
  c->proc = processToExecute;
#endif
#ifdef LBS
  processToExecute->state = RUNNING;
  c->proc = processToExecute;
#endif
#ifdef PBS
  processToExecute->state = RUNNING;
  processToExecute->numberOfRuns++;
  processToExecute->runTime = 0;
  processToExecute->waitTime = 0;
  c->proc = processToExecute;
#endif
#ifdef MLFQ
  processToExecute->timeQuantum = (processToExecute->priority) * 2;
  processToExecute->state = RUNNING;
  c->proc = processToExecute;
  processToExecute->numberOfRuns++;
#endif
  return;
}

void exchange(struct Queue *q, int current)
{
  struct proc *temp = q->processes[current];
  q->processes[current] = q->processes[(current + 1) % (NPROC + 1)];
  q->processes[(current + 1) % (NPROC + 1)] = temp;
  return;
}

void changeMLFQqueue(struct Queue *q, struct proc *p)
{
  q->back--;
  q->size--;

  if (q->back < 0)
  {
    q->back = NPROC;
  }

  p->checkQueue = 0;
  return;
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.

void scheduler(void)
{
  struct cpu *c = mycpu();

  c->proc = 0;
  for (;;)
  {
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();
#ifdef RR
    struct proc *p;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);
        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      change(p, c);
    }
#endif
#ifdef FCFS
    struct proc *p;
    // iterate through process table
    for (p = proc; p < &proc[NPROC]; p++)
    {
      struct proc *findProcess = 0;
      for (p = proc; p < &proc[NPROC]; p++)
      {
        acquire(&p->lock);
        if (p->state == RUNNABLE)
        {
          if (findProcess == 0)
          {
            findProcess = p;
          }
          else if (findProcess->creationTime > p->creationTime)
          {
            findProcess = p;
          }
        }
        release(&p->lock);
      }

      if (findProcess != 0)
      {
        acquire(&findProcess->lock);
        // execute the process
        if (findProcess->state == RUNNABLE)
        {
          execute(findProcess, c);
          swtch(&c->context, &findProcess->context);
        }
        change(findProcess, c);
      }
    }
#endif
#ifdef PBS
    struct proc *p;
    struct proc *findProcess = 0;
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state == RUNNABLE)
      {
        if (findProcess == 0)
        {
          findProcess = p;
          continue;
        }
        else if (nice_priority(findProcess) > nice_priority(p))
        {
          release(&findProcess->lock);
          findProcess = p;
          continue;
        }
        else if (nice_priority(findProcess) == nice_priority(p))
        {
          if (findProcess->numberOfRuns > p->numberOfRuns || findProcess->numberOfRuns == p->numberOfRuns)
          {
            int flag = 0;
            if (findProcess->numberOfRuns == p->numberOfRuns)
            {
              flag = 1;
            }
            if (flag == 1)
            {
              if (findProcess->creationTime < p->creationTime)
              {
                release(&findProcess->lock);
                findProcess = p;
                continue;
              }
            }
            else
            {
              release(&findProcess->lock);
              findProcess = p;
              continue;
            }
          }
        }
      }
      release(&p->lock);
    }
    if (findProcess == 0)
    {
      continue;
    }
    execute(findProcess, c);
    swtch(&c->context, &findProcess->context);
    change(findProcess, c);
#endif
#ifdef LBS
    int tickets_assigned = 0;
    int totalTickets = 0;

    for (p = proc; p < &proc[NPROC]; p++)
    {
      if (p->state == RUNNABLE)
        //   continue;
        // else
        totalTickets = totalTickets + p->tickets;
    }
    // we have found the total number of tickets

    long num_choose = random_gen(totalTickets);
    // Loop over process table looking for process to run.
    for (p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if (p->state != RUNNABLE)
      {
        release(&p->lock);
        continue;
      }
      tickets_assigned += p->tickets;
      // we don't need this process if it has ticjets less than findProcess number
      if (tickets_assigned < num_choose)
      {
        release(&p->lock);
        continue;
      }
      execute(p, c);
      swtch(&c->context, &p->context);
      // Process is done running
      change(p, c);
      break;
    }
#endif
#ifdef MLFQ
    struct proc *p;
    struct proc *findProcess = 0;

    // Implement Aging
    // iterate throguh the process table
    for (p = proc; p < &proc[NPROC]; p++)
    {
      if (p->state == RUNNABLE && ticks - p->queueCreationTime >= AGINGNUM)
      {
        p->queueCreationTime = ticks;
        if (p->checkQueue)
        {
          struct Queue *q;
          q = &mlfq[p->priority];
          for (int curr = q->front; curr != q->back; curr = (curr + 1) % (NPROC + 1))
          {
            if (q->processes[curr]->pid == p->pid)
            {
              exchange(q, curr);
            }
          }
          changeMLFQqueue(q, p);
        }
        // reducing the priority of the process
        if (p->priority != 0){
          p->priority--; 
        }
      }
    }
    // iterate through the process table
    for (p = proc; p < &proc[NPROC]; p++)
    {
      // acquire lock to access critical state
      acquire(&p->lock);
      // if process is runnable, but is not in any queue
      // it is pushed insed the queue, and checkqueue is set to 1
      if (p->state == RUNNABLE && p->checkQueue == 0)
      {
      // pushed to queue according to it's priority
        pushQueue(&mlfq[p->priority], p);
        p->checkQueue = 1;
      }
      release(&p->lock);
    }

    // maximum level in MLFQ cheduling is defined as 5
    for (int level = 0; level < MAXNUM; level++)
    {
      // if queue of the particular num is not empty
      while (mlfq[level].size != 0)
      {
        // process at the front of the queue is picked
        // and it is popped from the queues
        p = front(&mlfq[level]);
        acquire(&p->lock);
        popQueue(&mlfq[level]);
        p->checkQueue = 0;
        // if the state is runnable, the process is findProcess to be executed
        if (p->state == RUNNABLE)
        {
          p->queueCreationTime = ticks;
          findProcess = p;
          break;
        }
        release(&p->lock);
      }
      if (findProcess != 0){
        break;
      }
    }
    if (!findProcess){
      continue;
    }
    // the findProcess process is now executed
    execute(findProcess, c);
    swtch(&c->context, &findProcess->context);
    change(findProcess, c);
#endif
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&p->lock))
    panic("sched p->lock");
  if (mycpu()->noff != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first)
  {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock); // DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void wakeup(void *chan)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p != myproc())
    {
      acquire(&p->lock);
      if (p->state == SLEEPING && p->chan == chan)
      {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int kill(int pid)
{
  struct proc *p;

  for (p = proc; p < &proc[NPROC]; p++)
  {
    acquire(&p->lock);
    if (p->pid == pid)
    {
      p->killed = 1;
      if (p->state == SLEEPING)
      {
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int killed(struct proc *p)
{
  int k;

  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if (user_dst)
  {
    return copyout(p->pagetable, dst, src, len);
  }
  else
  {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if (user_src)
  {
    return copyin(p->pagetable, dst, src, len);
  }
  else
  {
    memmove(dst, (char *)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  struct proc *p;
  char *state;

  printf("\n");
  for (p = proc; p < &proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%s\n", state);
#ifdef RR
    printf("%d %s %s", p->pid, state, p->name);
#endif
#ifdef FCFS
    printf("%d %s %s %d", p->pid, state, p->name, p->creationTime);
#endif
#ifdef PBS
    int waitTime = ticks - p->creationTime - p->totalRunTime;
    printf("%d %d %s %s %d %d %d", p->pid, nice_priority(p), state, p->name, p->totalRunTime, waitTime, p->numberOfRuns);
#endif
#ifdef MLFQ
    int waitTime = ticks - p->queueCreationTime;
    printf("%d %d %s %d %d %d %d %d %d %d %d", p->pid, p->priority, state, p->totalRunTime, waitTime, p->numberOfRuns, p->queueRunTime[0], p->queueRunTime[1], p->queueRunTime[2], p->queueRunTime[3], p->queueRunTime[4]);
#endif
    printf("\n");
  }
}

// find process at front of the queue
struct proc *front(struct Queue *q)
{
  struct proc *retVal;
  if (q->front == q->back)
  {
    return 0;
  }
  else
  {
    retVal = q->processes[q->front];
  }
  return retVal;
}

void pushQueue(struct Queue *q, struct proc *element)
{
  if (q->size == NPROC)
  {
    panic("Size Limit exceeded");
  }

  // add element to the back of the queue
  q->processes[q->back] = element;
  q->back++;
  if (q->back == NPROC + 1)
  {
    q->back = 0;
  }
  q->size++;
}

void popQueue(struct Queue *q)
{
  if (q->size == 0)
  {
    panic("Queue is empty");
  }

  // pop element front the front of the queue
  q->front++;
  if (q->front == NPROC + 1)
  {
    q->front = 0;
  }
  q->size--;
}
