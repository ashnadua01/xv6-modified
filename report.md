REPORT

FCFS

1. Include variable createTime in struct proc in proc.h
2. Initialised the newly added variables in allocproc() function in proc.c
3. In scheduler() function in proc.c define an FCFS block.

4. Iterate through the process table. If a process arrives with createTime < current process, assign it current process.
5. lock the selected process and change its state to running
6. switch to that process
7. When process is finished, make c->proc = 0.
8. release its lock

Avg run time : 30
Avg wait time : 125


LOTTERY BASED SCHEDULING (LBS)


1. Add a new field tickets in struct proc in proc.h. Define an LBS block in scheduler() function
2. Each newly created process is assigned 10 lottery tickets
3. The total number of tickets is found first by iterating through process table.
4. In scheduler() function in proc.c define an LBS block.

5. When the scheduler runs, it picks a random number between 0 and the total number of tickets
6. Iterate through proces stable until process with assigned ticket = chosen random number is found
7. Add sys_settickets() function in sysproc.c - uses the predefined function argint() and allows each process to specify number of tickets it wants

Avg run time : 17
Avg wait time : 156


PRIORITY BASED SCHEDULING (PBS)


1. Take an arbitrary default priority value. All processes have priority number less than this
2. We assume higher the priority number, lesser is the priority.
2. Add a new field priority in struct proc in proc.h.
3. Add function sys_set_priority() in sysproc.c - to set priority of each process. Higher the number, lower the priority.
4. In the scheduler() function in proc.c define a PBS block.

5. Iterate over the ptable to find the running process with highest priority and make it run like in FCFS. Lock each process first before comparing its priority and release the lock after comparison is done.


Avg run time : 16
Avg wait time : 129


MULTI LEVEL FEEDBACK QUEUE (MLFQ)

1. Create a total of 5 queues (number 0 to 4)
2. In each queue, each process is allotted a particular number of ticks.
3. Check if process has aged / exhausted its time quantum. Remove that proces from queue and reduce its priority.
4. Iterate through ptable and schedule all those processes whose state is runnable and not in any queue.
5. These are pushed to another queue according to its priority. 
6. Iterate through all queues. The process at the front of each queue is picked, popped from queue and executed.

Avg run time : 14
Avg wait time : 128

ROUND ROBIN(RR)

1. this is the default scheduling

Avg run time : 15
Avg wait time : 162

COMPARISON:

1. FCFS is effective in the sense that processes are executed as they arrive and don't have to wait.
However since it is non-preemptive, some important high-priority processes might have to suffer. Also the run time for FCFS is very high because of the same. 

2. PBS is quite efficient when large number of processes are there and some proceese are more important to be executed than others. 

3. RR has considerably high wait time, with run time similar to other processes. 

4. MLFQ We chose average times to allow for a good comparision. the run time is the same as other processes with the wait time being comprable to PBS for the exact same input.
In general, PBS and MLFQ are the two most efficient scheduling algorithms of all the given. Which is the better choice of the two can depend on number of processes and whether they are I/o bound or CPU bound.

5. LBS also has considerably high wait time, though run time is similar to others.

