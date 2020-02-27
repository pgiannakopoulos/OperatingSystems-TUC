#include "tinyos.h"
#include "kernel_cc.h"
#include "kernel_streams.h"

#define BUF_SIZE 8192 //The size of the pipe buffer

/** Pipe Control Block */
typedef struct pipe_control_block
{
	char buffer[BUF_SIZE];    //a buffer to move data
	char *w, *r, *tail;       //pointers for easy access
	int full;                 //If the buffer is full, its value is 1
	FCB* reader;		      //The FCB of the reader thread-process
	FCB* writer; 	          //The FCB of the writer thread-process
	CondVar isEmpty, isFull;  //Condition variables for synchronisation of reader and writer
	
}PIPECB;

PIPECB* init_pipe();

int pipe_read(void* this, char *buf, unsigned int size);

int pipe_write(void* this, const char* buf, unsigned int size);

int pipe_close_reader(void* this);

int pipe_close_writer(void* this);