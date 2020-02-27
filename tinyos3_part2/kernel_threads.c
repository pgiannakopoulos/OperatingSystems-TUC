
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_cc.h"

/*Delete the PTCB*/
void release_PTCB(rlnode* node){
  free(node->ptcb);
  rlist_remove(node);
}

/* This function generates IDs for TCBs*/
Tid_t id_generator(){

  static unsigned int id = 2; /*ID = 1 is the main thread*/

  return (Tid_t)id++;
}

/*The Main function of every TCB*/
void start_thread()
{
  int exitval;
  PTCB* myptcb = CURTHREAD->owner_ptcb;
  Task call =  myptcb->task;
  exitval = call(myptcb->argl, myptcb->args);

  ThreadExit(exitval);
}


/** 
  @brief Create a new thread in the current process.
  */
Tid_t sys_CreateThread(Task task, int argl, void* args)
{

  PCB* pcb = CURPROC;  

  /* Create the PTCB */
  PTCB* myptcb = (struct pt_control_block*)malloc(sizeof(struct pt_control_block));
  
  if (myptcb == NULL)
  {
    printf("We are out of memory! \n");
    return NOTHREAD;
  }

  /* Initializing the PTCB*/
  myptcb->cv = COND_INIT;
  myptcb->task = task;
  myptcb->argl = argl;
  myptcb->args = args;
  myptcb->pcb = pcb;
  myptcb->joinable = 1;
  myptcb->exited = 0;
  myptcb->tid = id_generator();
  myptcb->ref_counter = 0;

  /*Create the Thread*/
  TCB* tcb = spawn_thread(pcb, start_thread);

  /*Link TCB with PTCB*/
  myptcb->tcb = tcb;
  tcb->owner_ptcb = myptcb;

  /*Link PTCB with PCB */
  /*Create the node of ptcb*/
  rlnode_init(&(myptcb->node), myptcb); 

  /*Add the node to the ptcb list*/
  rlist_push_back(&(pcb->ptcb_list), &(myptcb->node)); 

  pcb->active_threads++; /*It counts the active threads of the pcb*/

  wakeup(tcb); /*Make the thread READY for scheduling*/

  return (Tid_t)myptcb->tid;
}

/**
  @brief Return the Tid of the current thread.
 */
Tid_t sys_ThreadSelf()
{
  return (Tid_t) CURTHREAD->owner_ptcb->tid;
}

/**
  @brief Join the given thread.
  */
int sys_ThreadJoin(Tid_t tid, int* exitval)
{
  /*A thread cannot Join itself*/
  if(sys_ThreadSelf() == tid){
    return -1;
  }
  PCB* pcb = CURPROC;
  rlnode* ptcb_node = & pcb->ptcb_list; /*this is the head of ptcb list*/

  while(ptcb_node != NULL){ /* Check if there is a node*/
    if (ptcb_node->ptcb->tid == tid){ /* Check if the ID is found*/
      if (ptcb_node->ptcb->joinable == 0) /*Check it is joinable*/
      {
        return -1;
      }else{
        /*Count how many threads wait for this TCB*/
        ptcb_node->ptcb->ref_counter++;
        while(ptcb_node->ptcb->exited == 0){ /*Check is the thread has finished*/
          kernel_wait(&(ptcb_node->ptcb->cv), SCHED_USER);
        }
      }
      
      if(exitval != NULL){
        *exitval = ptcb_node->ptcb->exitval; /*save the exit value*/
      }
      ptcb_node->ptcb->ref_counter--;
      /*Check if there are other thread that wait the specific
      exit value. If not, delete the PTCB*/ 
      if(ptcb_node->ptcb->ref_counter <= 0){
        release_PTCB(ptcb_node);
      }
      return 0; 
    }

    ptcb_node = ptcb_node->next;
  }
  return -1;
}

/**
  @brief Detach the given thread.
  */
int sys_ThreadDetach(Tid_t tid)
{
  PCB* pcb = CURPROC;
  rlnode* ptcb_node = &(pcb->ptcb_list);

/*Search the tid and make it detachable*/
  while(ptcb_node != NULL){
    if (ptcb_node->ptcb->tid == tid){
      if (ptcb_node->ptcb->exited == 0)
      {
        ptcb_node->ptcb->joinable = 0;
        return 0;
      }
    }
    ptcb_node = ptcb_node->next;   
  }

  return -1;
}

/**
  @brief Terminate the current thread.
  */
void sys_ThreadExit(int exitval)
{
  TCB* tcb = CURTHREAD;
  PTCB* myptcb = tcb->owner_ptcb;
  myptcb->exitval = exitval; /*Save the exitval to PTCB for Join*/
  myptcb->tcb = NULL;
  myptcb->exited = 1; /*Mark the thread as exited*/

  /*Reduce the number of active threads of the process*/
  CURPROC->active_threads--; 
  kernel_broadcast(&(myptcb->cv)); /*Wake up ThreadJoin*/

  /*If the thread is the last one of this process, call Exit,
  otherwise relase the TCB*/
  if (CURPROC->active_threads <= 0){
    sys_Exit(exitval);
  }else{
    kernel_sleep(EXITED,SCHED_USER);
  }
}
