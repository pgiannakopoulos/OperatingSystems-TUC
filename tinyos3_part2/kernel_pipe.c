#include "util.h"
#include "tinyos.h"
#include "kernel_cc.h"
#include "kernel_streams.h"
#include "kernel_sched.h"
#include "kernel_proc.h"
#include "kernel_pipe.h"

/*Put a character to the buffer of the pipe. 
Returns 1 on success, otherwise 0*/
int buf_put(PIPECB* pipe, char c)
{
    if (pipe->w == pipe->r  &&  pipe->full){
        return 0;                  //buffer overrun
    }
    *pipe->w++ = c;                // insert c into buffer
    if (pipe->w >= pipe->tail)     // check if we are at the end of circular buffer
        pipe->w = pipe->buffer;    // return to the begin

    if (pipe->w == pipe->r)        // check if we are try to go out of the buffer
        pipe->full = 1;            // the buffer is full
    return 1;              
}

/*Get a character from the buffer of the pipe. 
Returns 1 on success, otherwise 0*/
int buf_get(PIPECB* pipe, char *pc)
{
    if (pipe->w == pipe->r  &&  !pipe->full)
        return 0;                  // the buffer empty  FAIL

    *pc = *pipe->r++;              // pick up the next char to be returned
    if (pipe->r >= pipe->tail)     // check if we are at the end of circular buffer
        pipe->r = pipe->buffer;    // return to the begin

    pipe->full = 0;               // if you read at least one character, the buffer is not full.
    return 1;             
}


/*Read data from the pipe 
Returns the size that we read on success, otherwise 0*/
int pipe_read(void* this, char *buf, unsigned int size){

	PIPECB* mypipe = (PIPECB *)this;

	/*Check for invalid pointers */
	if (mypipe ==NULL || mypipe->reader == NULL){ 
		return -1;
	}

	int count = 0;

	for (int i = 0; i < size; i++){

		/*If the writer is gone and the buffer is empty, return*/
		if (mypipe->writer ==NULL && mypipe->w == mypipe->r  &&  !mypipe->full)
		{
			return count; //end of data
		}

		/*While the buffer is empty go to sleep and wake up the writer */
		while(buf_get(mypipe, &buf[i]) == 0){
			kernel_broadcast(& mypipe->isFull);		
			kernel_wait(& mypipe->isEmpty,SCHED_PIPE);
		}
		count++;
	}

	return count;
}

/*Close the reader of the pipe 
Returns 0 on success, otherwise -1*/
int pipe_close_reader(void* this){

	PIPECB* mypipe = (PIPECB *)this;

	/*Check for invalid pointers */
	if (mypipe ==NULL)
	{
		return -1;
	}
	mypipe->reader = NULL;
	kernel_broadcast(& mypipe->isFull); //wake up the writer

	/*If the write is out, erase the pipe*/
	if (mypipe->writer == NULL){
		free(mypipe);
	}

	return 0;
}

/*Write data to the pipe 
Returns the size that we wrote on success, otherwise 0*/
int pipe_write(void* this, const char* buf, unsigned int size){

	PIPECB* mypipe = (PIPECB *)this;

	/*Check for invalid pointers */	
	if (mypipe ==NULL || mypipe->writer== NULL || mypipe->reader== NULL)
	{
		return -1;
	}

	int count = 0;

	for (int i = 0; i < size; i++)
	{
		/*While the buffer is full go to sleep and wake up the reader */
		while(buf_put(mypipe, buf[i]) == 0){
			kernel_broadcast(& mypipe->isEmpty);		
			kernel_wait(& mypipe->isFull,SCHED_PIPE);
		}
		count++;
	}

	return count;
}

/*Close the writer of the pipe 
Returns 0 on success, otherwise -1*/
int pipe_close_writer(void* this){
	PIPECB* mypipe = (PIPECB *)this;
	/*Check for invalid pointers */
	if (mypipe ==NULL)
	{
		return -1;
	}
	mypipe->writer = NULL;

	kernel_broadcast(& mypipe->isEmpty);//wake up the reader

	/*If the reader is out, erase the pipe*/
	if (mypipe->reader == NULL){
		free(mypipe);
	}

	return 0;
}

/*The reader cannot write to the pipe.*/
int pipe_reader_null(void* this, char* buf, unsigned int size){
	return -1;
}

/*The writer cannot reader from the pipe.*/
int pipe_writer_null(void* this, const char* buf, unsigned int size){
	return -1;
}

/* File operations for the reader */
static file_ops pipeReadOps = {
	.Open = NULL,
	.Read = pipe_read,
	.Write = pipe_writer_null,
	.Close = pipe_close_reader
};

/* File operations for the writer */
static file_ops pipeWriteOps = {
	.Open = NULL,
	.Read = pipe_reader_null,
	.Write = pipe_write,
	.Close = pipe_close_writer
};

/* Initialization of the pipe control block */
PIPECB* init_pipe(){

	/*Allocate memory*/
	PIPECB* mypipe = (PIPECB*)malloc(sizeof(PIPECB));

	if (mypipe == NULL)
	{
		return NULL;
	}

	mypipe->w = mypipe->r = mypipe->buffer;       // init to any slot in buffer
    mypipe->tail = & mypipe->buffer[BUF_SIZE];    // the last valid slot in buffer
    mypipe->full = 0;                             // buffer is empty at the beginning
	mypipe->isEmpty = COND_INIT;
	mypipe->isFull = COND_INIT;

	/* Put zeros to the buffer */
	for (int i = 0; i < BUF_SIZE; i++)
	{
		mypipe->buffer[i] = 0;
	}

	return mypipe;
}


int sys_Pipe(pipe_t* pipe)
{
	/* Initialize the pipecb */
	PIPECB* mypipe = init_pipe();     

	Fid_t fid[2];
	FCB* fcb[2];

	/* Try to get the 2 required fids*/
	if(FCB_reserve(2, fid, fcb)==0)
	{
		return -1;
	}

	/*Connect the FCBs and the pipe so as to have access. */
	mypipe->reader = fcb[0];
	pipe->read = fid[0];
	fcb[0]->streamobj = mypipe;
	fcb[0]->streamfunc = &pipeReadOps;

	mypipe->writer = fcb[1];
	pipe->write = fid[1];
	fcb[1]->streamobj = mypipe;
	fcb[1]->streamfunc = &pipeWriteOps;

	return 0;
}

