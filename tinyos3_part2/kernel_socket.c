
#include "tinyos.h"
#include "kernel_sched.h"
#include "kernel_streams.h"
#include "kernel_pipe.h"
#include "kernel_proc.h"
#include "kernel_cc.h"

/* The possible states of a socket */
typedef enum {
	UNBOUND,
	LISTENER,
	PEER
}Socket_type;

/* The control block of the listener */
typedef struct Listener_Control_Block
{
	CondVar cv;    //Condition variable for synchronisation of requests
	rlnode queue;  //a queue for the requests
}LISTENERCB;

/* Just declare this for the compiler */
typedef struct socket_control_block SOCKETCB;

/* The control block of the peer */
typedef struct Peer_Control_Block
{
	PIPECB* pipe_read;       //the pipe that we are going to read
	PIPECB* pipe_write;      //the pipe that we are going to write
	SOCKETCB* peer;          //the socketcb of the other socket
}PEERCB;

/* Each type of socket has common fields with UNBOUND
but lister and peer have some extra fields for proper function */
typedef union Socket_struct_type{
	LISTENERCB listener_struct;
	PEERCB peer_struct;
}SST;

typedef struct socket_control_block
{
	Socket_type type;   //the socket type
	FCB* fcb;			//the socket fcb
	Fid_t fid;			//the socket fid
	SST struct_type;    //the extra fields that we use (for peer or listener)
	port_t port;        //the preferred port
	int ref_counter;    //the number of the pointers to this socket	
}SOCKETCB;

/*The request control block */
typedef struct connection_request
{
	SOCKETCB* socket_req;  //the control block the socket which made the request
	CondVar cv;			   //Condition variable for synchronisation of the request
	int served;		       //if the request is served, is 1
	int activeListener;    //if the lister is active, is 1
	rlnode node;           //the host node for easy access
}REQUESTCB;

/*The Port Map*/
SOCKETCB* PORT_MAP[MAX_PORT+1];

/*Read data from the pipe 
Returns the size that we read on success, otherwise 0*/
int socket_read(void* this, char* buf, unsigned int size){
	SOCKETCB* mysocket = (SOCKETCB*)this;

	PIPECB* mypipe = mysocket->struct_type.peer_struct.pipe_read; //get the pipe for read
	return pipe_read(mypipe, buf, size);
}

/*Write data to the socket 
Returns the size that we wrote on success, otherwise 0*/
int socket_write(void* this, const char* buf, unsigned int size){
	SOCKETCB* mysocket = (SOCKETCB*)this;

	PIPECB* mypipe = mysocket->struct_type.peer_struct.pipe_write; //get the pipe for write
	return pipe_write(mypipe, buf, size);
}

/*Close the writer of the socket 
Returns 0 on success, otherwise -1*/
int socket_close(void* this){
	SOCKETCB* mysocket = (SOCKETCB*)this;
	port_t port = mysocket->port;

	/*In case the socker is peer */
	if (mysocket->type == PEER)
	{
		SOCKETCB* otherPeer = mysocket->struct_type.peer_struct.peer;

		/*Check if the other socket is still active */
		if(otherPeer !=NULL){
			otherPeer->ref_counter--;                          //tell him that we leave
			otherPeer->struct_type.peer_struct.peer = NULL;
		}
		
		/* Close our pipes */
		if (mysocket != NULL)
		{
			pipe_close_writer(mysocket->struct_type.peer_struct.pipe_write);
			pipe_close_reader(mysocket->struct_type.peer_struct.pipe_read);
		}
	}

	/*In case the socker is listener */
	if(mysocket->type == LISTENER){
		rlnode* mynode;

		/*Reject all the pending requests. In this way Connect function will fail */
		while(!is_rlist_empty(& mysocket->struct_type.listener_struct.queue)){
			mynode = rlist_pop_front(& mysocket->struct_type.listener_struct.queue);
			mynode->request->activeListener = 0;
		}

		/*Wake up Accept. In this way Accept function will fail */
		kernel_broadcast(& mysocket->struct_type.listener_struct.cv);
	}

	mysocket->ref_counter--;

	/*If no one uses this socket erase it */
	if (mysocket->ref_counter <=0 && mysocket != NULL)
	{
		free(mysocket);
	}

	/* Recycle the port */
	PORT_MAP[port] = NULL;

	return 0;
}

/* The file operations of the socket */
static file_ops socketOps = {
	.Open = NULL,
	.Read = socket_read,
	.Write = socket_write,
	.Close = socket_close
};

/* Initialize the socket */
Fid_t sys_Socket(port_t port)
{
	/*Check for illegal port */
	if (port < 0 || port > MAX_PORT)
	{
		return NOFILE;
	}

	/*Allocate memory for the socket */
	SOCKETCB* mysocket = (SOCKETCB *)malloc(sizeof(SOCKETCB));

	if (mysocket == NULL)
	{
		printf("No memory for sockets!\n");
		return NOFILE;
	}

	/*Initialize the socket */
	mysocket->type = UNBOUND;

	if(FCB_reserve(1, & mysocket->fid, & mysocket->fcb)==0)
	{
		free(mysocket);
		return NOFILE; //No fid available
	}

	mysocket->ref_counter =0;
	mysocket->port = port;
	mysocket->fcb->streamobj = mysocket;
	mysocket->fcb->streamfunc = &socketOps;
	mysocket->ref_counter++;

	return mysocket->fid;
}

int sys_Listen(Fid_t sock)
{
	PCB* pcb = CURPROC;
	SOCKETCB* mysocket;
	
	/* Check if the file id is legal*/
	if (sock < 0 || sock > MAX_FILEID){	
		return -1;
	}else{
		/* Check if the socket is bound to a port*/
		if (pcb->FIDT[sock] == NULL){
			return -1;
		}
	}
	/* Get the socket object */
	if (pcb->FIDT[sock]->streamfunc == &socketOps)
	{
		mysocket = pcb->FIDT[sock]->streamobj;
		if (mysocket->port == NOFILE)
		{
			return -1;
		}
	}else{
		return -1;
	}

	port_t port = mysocket->port;

	if (port== NOPORT)
	{
		return -1;
	}else{
		/*Check if the port is occupied by another listener */
		/*Check if the socket has already been initialized */
		if (PORT_MAP[port] != NULL || mysocket->type != UNBOUND)
		{
			return -1;
		}
	}

	/* Initialiaze the Listener */
	mysocket->type = LISTENER;
	mysocket->struct_type.listener_struct.cv = COND_INIT;
	rlnode_init(& mysocket->struct_type.listener_struct.queue, NULL);

	PORT_MAP[port] = mysocket;

	return 0;
}

Fid_t sys_Accept(Fid_t lsock)
{
	/*Checks if the file id is legal*/
	if (lsock < 0 || lsock > MAX_FILEID)
	{	
		return NOFILE;
	}else{
		if (CURPROC->FIDT[lsock] == NULL)
		{
			return NOFILE;
		}
	}
	SOCKETCB* listener;
	if (CURPROC->FIDT[lsock]->streamfunc == &socketOps)
	{
		listener = CURPROC->FIDT[lsock]->streamobj;
		if (listener->port == NOFILE)
		{
			return NOFILE;
		}
	}else{
		return NOFILE;
	}

	if (listener->type != LISTENER){
		return NOFILE;
	}

	/*Initializing */
	SOCKETCB* peer = NULL;
	SOCKETCB* server_socket = NULL;
	PIPECB* pipe1 = NULL;
	PIPECB* pipe2 = NULL;
	rlnode* request_node = NULL;
	REQUESTCB* myrequest = NULL;
	port_t lport = listener->port;

	/*Check if there is a request */
	while(is_rlist_empty(& listener->struct_type.listener_struct.queue))
	{	
		/* Check if while waiting, the listening socket lsock was closed*/
		kernel_wait(& listener->struct_type.listener_struct.cv, SCHED_USER);

		if (PORT_MAP[lport] == NULL)
		{
			return NOFILE;
		}
	}
		
	request_node = rlist_pop_front(& listener->struct_type.listener_struct.queue);
	myrequest = request_node->request;

	peer = myrequest->socket_req;

	/* Check if the available file ids for the process are exhausted */
	Fid_t server_id = sys_Socket(listener->port);
	if (server_id == NOFILE)
	{
		return NOFILE;
	}
	server_socket = CURPROC->FIDT[server_id]->streamobj;

	pipe1 = init_pipe();
	pipe2 = init_pipe();

	if (pipe1 == NULL || pipe2 == NULL)
	{
		return NOFILE;
	}

	/* Configure the server pipe*/
	pipe1->reader = server_socket->fcb;
	pipe1->writer = peer->fcb;

	/* Configure the peer pipe*/
	pipe2->reader = peer->fcb;
	pipe2->writer = server_socket->fcb;

	/*Make them peers */
	peer->struct_type.peer_struct.pipe_read = pipe2;
	peer->struct_type.peer_struct.pipe_write = pipe1;
	peer->struct_type.peer_struct.peer = server_socket;
	peer->type= PEER;

	server_socket->struct_type.peer_struct.pipe_read = pipe1;
	server_socket->struct_type.peer_struct.pipe_write = pipe2;
	server_socket->struct_type.peer_struct.peer = peer;
	server_socket->type =  PEER;

	/*Connection is established*/
	myrequest->served = 1;
	peer->ref_counter++;
	server_socket->ref_counter++;
	kernel_broadcast(& myrequest->cv);

	return server_socket->fid;
}

int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{
	PCB* pcb = CURPROC;
	SOCKETCB* peer;

	/** Tests for our socket **/
	/* Check for ligal FID*/
	/* Check is smth is bounded to this port */
	if (sock < 0 || sock > MAX_FILEID)
	{
		return -1;
	}else{
		if (pcb->FIDT[sock] == NULL)
		{
			return -1;
		}
	}
	/* Check is a socket is bounded to this FID*/
	if (pcb->FIDT[sock]->streamfunc == &socketOps)
	{
		peer = pcb->FIDT[sock]->streamobj;

		/*Check if this port our socket has an port*/
	}else{
		return -1;
	}
	port_t myport = peer->port;
	/*Check if there is a listener at the port that we want to connect*/
	if (PORT_MAP[myport] != NULL || peer->type != UNBOUND)
	{
		return -1;
	}
	/** Checks for the listener socket**/
	/* Check the port */
	if (port <0 || port >= MAX_PORT)
	{
		return -1;
	}
	/*Check if there is a listener */
	SOCKETCB* listener = PORT_MAP[port];
	if (PORT_MAP[port] != NULL)
	{
		if (listener->type != LISTENER)
		{
			return -1;
		}
	}else{
		return -1;
	}

	/*Create a request*/
	REQUESTCB* myrequest = (REQUESTCB*)malloc(sizeof(REQUESTCB));
	myrequest->socket_req = peer;
	myrequest->cv = COND_INIT;
	myrequest->served = 0;
	myrequest->activeListener = 1;
	rlnode_init(& myrequest->node, myrequest);

	/*Add the request to the queue*/
	rlist_push_back(&listener->struct_type.listener_struct.queue, &myrequest->node);

	/*Sleep */
	kernel_broadcast(& listener->struct_type.listener_struct.cv);
	kernel_timedwait(& myrequest->cv, SCHED_USER, timeout);

	/* Check is the request is served */
	if(myrequest->served == 0 || myrequest->activeListener == 0){
		return -1;
	}
	free(myrequest);

	return 0;
}

int sys_ShutDown(Fid_t sock, shutdown_mode how)
{	
	/*Check if there is a valic id*/
	if (sock < 0 || sock > MAX_FILEID){
		return -1;
	}else{
		if (CURPROC->FIDT[sock] == NULL){
			return -1;
		}
	}
	/*Check if there is a socket*/
	SOCKETCB* mysocket;
	if (CURPROC->FIDT[sock]->streamfunc == &socketOps){
		mysocket = CURPROC->FIDT[sock]->streamobj;
		if (mysocket->port == NOFILE){
			return -1;
		}
	}

	SOCKETCB* peer = mysocket->struct_type.peer_struct.peer;
	switch(how){
		case SHUTDOWN_READ:  //Close our reader and the writer of the other socket
			if(mysocket->struct_type.peer_struct.pipe_read !=NULL)
				pipe_close_reader(mysocket->struct_type.peer_struct.pipe_read);
			mysocket->struct_type.peer_struct.pipe_read = NULL;
			if(peer->struct_type.peer_struct.pipe_write !=NULL)
				pipe_close_writer(peer->struct_type.peer_struct.pipe_write);
			peer->struct_type.peer_struct.pipe_write = NULL;
			break;
		case SHUTDOWN_WRITE:  //Close our writer
			pipe_close_writer(mysocket->struct_type.peer_struct.pipe_write);
			mysocket->struct_type.peer_struct.pipe_write = NULL;
			break;
		case SHUTDOWN_BOTH:   ////Close our reader and both writers of the other socket
			if(mysocket->struct_type.peer_struct.pipe_read !=NULL)
				pipe_close_writer(mysocket->struct_type.peer_struct.pipe_write);
			if(mysocket->struct_type.peer_struct.pipe_write !=NULL)
				pipe_close_reader(mysocket->struct_type.peer_struct.pipe_read);
			if(peer->struct_type.peer_struct.pipe_write !=NULL)
				pipe_close_writer(peer->struct_type.peer_struct.pipe_write);
			mysocket->struct_type.peer_struct.pipe_write = NULL;
			mysocket->struct_type.peer_struct.pipe_read = NULL;
			peer->struct_type.peer_struct.pipe_write = NULL;
			break;
		default:
			return 0;
	}

	return 0;
}