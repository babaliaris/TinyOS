
#include <assert.h>
#include "kernel_cc.h"
#include "kernel_proc.h"
#include "kernel_streams.h"



static file_ops open_info_ops;


/* 
 The process table and related system calls:
 - Exec
 - Exit
 - WaitPid
 - GetPid
 - GetPPid

 */

/* The process table */
PCB PT[MAX_PROC];
unsigned int process_count;

PCB* get_pcb(Pid_t pid)
{
  return PT[pid].pstate==FREE ? NULL : &PT[pid];
}

Pid_t get_pid(PCB* pcb)
{
  return pcb==NULL ? NOPROC : pcb-PT;
}

/* Initialize a PCB */
static inline void initialize_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->argl = 0;
  pcb->args = NULL;
  pcb->num_of_threads = 0;

  for(int i=0;i<MAX_FILEID;i++)
    pcb->FIDT[i] = NULL;

  rlnode_init(& pcb->children_list, NULL);
  rlnode_init(& pcb->exited_list, NULL);
  rlnode_init(& pcb->children_node, pcb);
  rlnode_init(& pcb->exited_node, pcb);

  //Initialize ptcb head.
  rlnode_init( &(pcb->ptcb_head), NULL );
  
  pcb->child_exit = COND_INIT;
}


static PCB* pcb_freelist;

void initialize_processes()
{
  /* initialize the PCBs */
  for(Pid_t p=0; p<MAX_PROC; p++) {
    initialize_PCB(&PT[p]);
  }

  /* use the parent field to build a free list */
  PCB* pcbiter;
  pcb_freelist = NULL;
  for(pcbiter = PT+MAX_PROC; pcbiter!=PT; ) {
    --pcbiter;
    pcbiter->parent = pcb_freelist;
    pcb_freelist = pcbiter;
  }

  process_count = 0;

  /* Execute a null "idle" process */
  if(Exec(NULL,0,NULL)!=0)
    FATAL("The scheduler process does not have pid==0");
}


/*
  Must be called with kernel_mutex held
*/
PCB* acquire_PCB()
{
  PCB* pcb = NULL;

  if(pcb_freelist != NULL) {
    pcb = pcb_freelist;
    pcb->pstate = ALIVE;
    pcb_freelist = pcb_freelist->parent;
    process_count++;
  }

  return pcb;
}

/*
  Must be called with kernel_mutex held
*/
void release_PCB(PCB* pcb)
{
  pcb->pstate = FREE;
  pcb->parent = pcb_freelist;
  pcb_freelist = pcb;
  process_count--;
}


/*
 *
 * Process creation
 *
 */

/*
	This function is provided as an argument to spawn,
	to execute the main thread of a process.
*/
void start_main_thread()
{
  int exitval;

  Task call =  CURPROC->main_task;
  int argl = CURPROC->argl;
  void* args = CURPROC->args;

  exitval = call(argl,args);

  Exit(exitval);
}


void start_another_thread()
{

  //If CURTHREAD is not NULL.
  if (CURTHREAD != NULL)
  {

    //Get important data.
    Task  call  = CURTHREAD->tcb_ptcb->task;
    int   argl  = CURTHREAD->tcb_ptcb->argl;
    void* args  = CURTHREAD->tcb_ptcb->args;

    
    //Call the Task and get the exit value.
    if (call != NULL)
    {

      //Call the task.
      CURTHREAD->tcb_ptcb->exit_value  = call(argl, args);

      //Call the thread exit.
      sys_ThreadExit(CURTHREAD->tcb_ptcb->exit_value);
    }

  }

}


/*
	System call to create a new process.
 */
Pid_t sys_Exec(Task call, int argl, void* args)
{
  PCB *curproc, *newproc;
  
  /* The new process PCB */
  newproc = acquire_PCB();

  if(newproc == NULL) goto finish;  /* We have run out of PIDs! */

  if(get_pid(newproc)<=1) {
    /* Processes with pid<=1 (the scheduler and the init process) 
       are parentless and are treated specially. */
    newproc->parent = NULL;
  }
  else
  {
    /* Inherit parent */
    curproc = CURPROC;

    /* Add new process to the parent's child list */
    newproc->parent = curproc;
    rlist_push_front(& curproc->children_list, & newproc->children_node);

    /* Inherit file streams from parent */
    for(int i=0; i<MAX_FILEID; i++) {
       newproc->FIDT[i] = curproc->FIDT[i];
       if(newproc->FIDT[i])
          FCB_incref(newproc->FIDT[i]);
    }
  }


  /* Set the main thread's function */
  newproc->main_task = call;

  /* Copy the arguments to new storage, owned by the new process */
  newproc->argl = argl;
  if(args!=NULL) {
    newproc->args = malloc(argl);
    memcpy(newproc->args, args, argl);
  }
  else
    newproc->args=NULL;


  //Push the Main thread into the PTCB List of this process.
  //rlist_push_back( &(newproc->ptcb_head), &(newproc->main_thread->ptcb_node) );

  /* 
    Create and wake up the thread for the main function. This must be the last thing
    we do, because once we wakeup the new thread it may run! so we need to have finished
    the initialization of the PCB.
   */
  if(call != NULL) {

    //Create The Main Process.
    newproc->main_thread = spawn_thread(newproc, start_main_thread);

    //Increase the number of threads of this process.
    newproc->num_of_threads++;

    //Tell the PTCB of the main thread that it is the main thead.
    newproc->main_thread->tcb_ptcb->is_main = 1;

    //Append the ptcb of the main thread into the process list.
    rlist_append( &(newproc->ptcb_head), &(newproc->main_thread->tcb_ptcb->ptcb_node) );

    wakeup(newproc->main_thread);
  }


finish:
  return get_pid(newproc);
}


/* System call */
Pid_t sys_GetPid()
{
  return get_pid(CURPROC);
}


Pid_t sys_GetPPid()
{
  return get_pid(CURPROC->parent);
}


static void cleanup_zombie(PCB* pcb, int* status)
{
  if(status != NULL)
    *status = pcb->exitval;

  rlist_remove(& pcb->children_node);
  rlist_remove(& pcb->exited_node);

  release_PCB(pcb);
}


static Pid_t wait_for_specific_child(Pid_t cpid, int* status)
{

  /* Legality checks */
  if((cpid<0) || (cpid>=MAX_PROC)) {
    cpid = NOPROC;
    goto finish;
  }

  PCB* parent = CURPROC;
  PCB* child = get_pcb(cpid);
  if( child == NULL || child->parent != parent)
  {
    cpid = NOPROC;
    goto finish;
  }

  /* Ok, child is a legal child of mine. Wait for it to exit. */
  while(child->pstate == ALIVE)
    kernel_wait(& parent->child_exit, SCHED_USER);
  
  cleanup_zombie(child, status);
  
finish:
  return cpid;
}


static Pid_t wait_for_any_child(int* status)
{
  Pid_t cpid;

  PCB* parent = CURPROC;

  /* Make sure I have children! */
  if(is_rlist_empty(& parent->children_list)) {
    cpid = NOPROC;
    goto finish;
  }

  while(is_rlist_empty(& parent->exited_list)) {
    kernel_wait(& parent->child_exit, SCHED_USER);
  }

  PCB* child = parent->exited_list.next->pcb;
  assert(child->pstate == ZOMBIE);
  cpid = get_pid(child);
  cleanup_zombie(child, status);

finish:
  return cpid;
}


Pid_t sys_WaitChild(Pid_t cpid, int* status)
{
  /* Wait for specific child. */
  if(cpid != NOPROC) {
    return wait_for_specific_child(cpid, status);
  }
  /* Wait for any child */
  else {
    return wait_for_any_child(status);
  }

}


void sys_Exit(int exitval)
{
  /* Right here, we must check that we are not the boot task. If we are, 
     we must wait until all processes exit. */
  if(sys_GetPid()==1) {
    while(sys_WaitChild(NOPROC,NULL)!=NOPROC);
  }

  PCB *curproc = CURPROC;  /* cache for efficiency */

  /* Do all the other cleanup we want here, close files etc. */
  if(curproc->args) {
    free(curproc->args);
    curproc->args = NULL;
  }

  /* Clean up FIDT */
  for(int i=0;i<MAX_FILEID;i++) {
    if(curproc->FIDT[i] != NULL) {
      FCB_decref(curproc->FIDT[i]);
      curproc->FIDT[i] = NULL;
    }
  }

  /* Reparent any children of the exiting process to the 
     initial task */
  PCB* initpcb = get_pcb(1);
  while(!is_rlist_empty(& curproc->children_list)) {
    rlnode* child = rlist_pop_front(& curproc->children_list);
    child->pcb->parent = initpcb;
    rlist_push_front(& initpcb->children_list, child);
  }

  /* Add exited children to the initial task's exited list 
     and signal the initial task */
  if(!is_rlist_empty(& curproc->exited_list)) {
    rlist_append(& initpcb->exited_list, &curproc->exited_list);
    kernel_broadcast(& initpcb->child_exit);
  }

  /* Put me into my parent's exited list */
  if(curproc->parent != NULL) {   /* Maybe this is init */
    rlist_push_front(& curproc->parent->exited_list, &curproc->exited_node);
    kernel_broadcast(& curproc->parent->child_exit);
  }

  /* Disconnect my main_thread */
  curproc->main_thread = NULL;

  /* Now, mark the process as exited. */
  curproc->pstate = ZOMBIE;
  curproc->exitval = exitval;

  //Exit the thread.
  sys_ThreadExit(exitval);
}











//Open info data structure.
typedef struct open_info_control_block
{
  int next_pcb;

}OCB;




//--------------------------Open Info Functions--------------------------//

//Open Info OPEN Function.
void *info_open(uint minor)
{
	return NULL;
}


//Open Info READ Function.
int info_read(void* this, char *buf, unsigned int size)
{

  //Cast the buffer to a procinfo object.
  procinfo *info = (procinfo *)buf;

  //Cast this to an OCB object.
  OCB *ocb = (OCB *)this;

  //Get the nect pcb.
  PCB next_pcb = PT[ocb->next_pcb];

  //----------Initialize the info object----------//
  info->alive = next_pcb.pstate;
  info->argl  = next_pcb.argl;

  //Get the args characters.
  char *temp = (char *)next_pcb.args;
  for (int i = 0; i < info->argl; i++)
    info->args[i] = temp[i];

  //Main task and PID.
  info->main_task = next_pcb.main_task;
  info->pid       = ocb->next_pcb;
  info->ppid      = -1;

  //Parent of next pcb.
  PCB *parent = next_pcb.parent;

  //Find parent PID.
  for (int i = 0; i < MAX_PROC; i++)
    if ( &PT[i] == parent){ info->ppid = i; break; }
  
  //Thread count.
  info->thread_count = next_pcb.num_of_threads;
  //----------Initialize the info object----------//


  //Go to the next process.
  ocb->next_pcb++;


  //EOF Reached because we are out of bounds in the process table.
  if ( ocb->next_pcb >= MAX_PROC )
    return 0;
    
  //EOF Reached because the next PCB is free.
  else if ( PT[ocb->next_pcb].pstate == FREE )
    return 0;

  //Else just return the procinfo size.
  else
    return sizeof(procinfo);
}


//Open Info WRITE Function.
int info_write(void* this, const char* buf, unsigned int size)
{

  return -1;
}


//Open Info CLOSE Function.
int info_close(void* this)
{

  //Cast this to an OCB object.
  OCB *ocb = (OCB *)this;

  //Destroy the open info object.
  free(ocb);
  
  return 0;
}

//--------------------------Open Info Functions--------------------------//




//Initialize open info function.
void initialize_openInfo()
{
  open_info_ops.Open  = info_open;
  open_info_ops.Read  = info_read;
  open_info_ops.Write = info_write;
  open_info_ops.Close = info_close;
}





//Open Info System Call.
Fid_t sys_OpenInfo()
{

  //Define two arrays.
	Fid_t fidts[1];
	FCB   *fcbs[1];

	//Reserve 1 FCB.
	if ( !FCB_reserve(1, fidts, fcbs) )
		return NOFILE;


  //Create a new Open Info Object.
  OCB *new_ocb = (OCB *)malloc(sizeof(OCB));


  //-----Initialize the Open Info Object-----//
  new_ocb->next_pcb = 0;
  //-----Initialize the Open Info Object-----//
  

  //Initialize the stream functions.
  fcbs[0]->streamfunc = &open_info_ops;
  fcbs[0]->streamobj  = new_ocb;

  //Return the fid of the open info stream.
	return fidts[0];
}

