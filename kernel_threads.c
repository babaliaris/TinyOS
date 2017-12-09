
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"

/** 
  @brief Create a new thread in the current process.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{

  //Spawn a new thread.
  TCB *new_thread = spawn_thread(CURPROC, start_another_thread);

  //Store the main task and args into the ptcb of this thread.
  new_thread->tcb_ptcb->task = task;
  new_thread->tcb_ptcb->argl = argl;
  new_thread->tcb_ptcb->args = args;

  //Increate the number of threads of the CURPROC;
  CURPROC->num_of_threads++;

  //Append the ptcb of this new thread into the process list.
  rlist_append( &(CURPROC->ptcb_head), &(new_thread->tcb_ptcb->ptcb_node) );

  //Wake up this thread.
  wakeup(new_thread);

  //Return the memory of the ptcb.
	return (Tid_t)new_thread->tcb_ptcb;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
	return (Tid_t) CURTHREAD;
}

/**
  @brief Join the given thread.
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{

  //Get the ptcb from the tid.
  PTCB *ptcb = (PTCB *)tid;

  //Join works when: tid is pointing to a thread which is not detached and not exited.
  //                 but also when the tid is pointing to a threand which belongs to the current thread!!!!
  if ( !ptcb->is_detached && !ptcb->exited_flag && ptcb->pcb == CURPROC)
  {
      //Increase the reference counter.
      ptcb->ref_cnt++;

      while(ptcb->exited_flag == 0)
        kernel_wait( &(ptcb->joinVar) , SCHED_USER);

      //Reduce the reference counter.
      ptcb->ref_cnt--;

      //Get the exited value.
      if(exitval)
        *exitval = ptcb->exit_value;

      
      //If the PTCB of the tid thread do not have other threads
      //sleeping inside the condVar, destroy it!!!
      if (ptcb->ref_cnt <= 0)
      {
        rlist_remove( &(ptcb->ptcb_node) );
        free(ptcb);
      }

      //Return successfully.
      return 0;
    }
  

  //You don't have to wait because the thread is already dead.
  if (ptcb->exited_flag)
    return 0;
    
  //Return not success.
  return -1;
}

/**
  @brief Detach the given thread.
  */
int sys_ThreadDetach(Tid_t tid)
{

  //Get the ptcb from the tid.
  PTCB *ptcb = (PTCB *)tid;

  //Detach works when: The thread is not exited and it belongs to the current thread!!!
  if ( !ptcb->exited_flag && ptcb->pcb == CURPROC)
  {

    //Make this ptcb detached.
    ptcb->is_detached = 1;

    //Broadcast the joinVar of this ptcb.
    Cond_Broadcast( &(ptcb->joinVar) );

    //Return successfully.
    return 0;

  }

  //Return not success.
  return -1;
}

/**
  @brief Terminate the current thread.
  */
void sys_ThreadExit(int exitval)
{
  
  //Set the exited flag to true.
  CURTHREAD->tcb_ptcb->exited_flag = 1;

  //Wake up all the threads that are sleeping in this thread's condVar.
  kernel_broadcast( &(CURTHREAD->tcb_ptcb->joinVar) );

  //Decrease the number of theads of this process.
  CURPROC->num_of_threads--;

  //If this is the main thread, return the exit value to the PCB.
  if (CURTHREAD->tcb_ptcb->is_main)
    CURPROC->exitval = exitval;


  //If this is the last thread that is exiting, clean up!!!
  if (CURPROC->num_of_threads == 0)
  {

    //Just a temp variable.
    rlnode *temp;

    //While the list is not empty.
    while ( !is_rlist_empty( &(CURPROC->ptcb_head) ) )
    {

      //Pop front.
      temp = rlist_pop_front( &(CURPROC->ptcb_head) );

      //Free current PTCB.
      free(temp->ptcb);
    }

  }

  //Stop the Thread.
  kernel_sleep(EXITED, SCHED_USER);
}