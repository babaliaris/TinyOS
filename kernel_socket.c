
#include "tinyos.h"
#include "util.h"
#include "kernel_streams.h"
#include "kernel_sched.h"
#include "kernel_cc.h"

static file_ops sock_ops;

//Define the size of the Pipe Buffer.
#define size_of_buffer 4096 //4 kb.

//Pipe Definitions.
//================================Pipe Control Block================================//
typedef struct Pipe_Control_Block
{

	//Pipe Buffer.
	char buffer[size_of_buffer];

	//Read and write Fids.
	pipe_t pip_t;

	//Read Index.
	int read_index;

	//Write Index.
	int write_index;

	//Buffer size.
	int buffer_size;

	//Condition Variables.
	CondVar haspace, hasdata;

}PIPCB;
//================================Pipe Control Block================================//


PIPCB *create_pipe(Fid_t fid_1, Fid_t fid_2);

//Reader Functions.
int pipe_reader_read(void* this, char *buf, unsigned int size);
int pipe_reader_close(void* this);

//Writer functions.
int pipe_writer_write(void* this, const char* buf, unsigned int size);
int pipe_writer_close(void* this);


//====================Socket Type Enumeration=====================//
enum SOCK_TYPE
{
	SOCK_UNBOUND,
	SOCK_LISTEN,
	SOCK_PEER,
	SOCK_CLOSED
};
//====================Socket Type Enumeration=====================//



//====================Unbound Socket Structure====================//
typedef struct unbound_socket
{
	rlnode node;

}UnbSock;
//====================Unbound Socket Structure====================//



//=====================Listen Socket Structure====================//
typedef struct listen_socket
{

	CondVar req;
	rlnode  queue;

}LisSock;
//=====================Listen Socket Structure====================//



//======================Peer Socket Structure=====================//
typedef struct peer_socket
{

	void *other;

	PIPCB *pipe_send;
	PIPCB *pipe_recv;

	int canWrite;
	int canRead;

}PeerSock;
//======================Peer Socket Structure=====================//



//========================Socket Type Union=======================//
typedef union socket_type_union
{
	UnbSock  unb_sock;
	LisSock  lis_sock;
	PeerSock peer_sock;

}STU;
//========================Socket Type Union=======================//



//=======================Socket Control Block=====================//
typedef struct Socket_Control_Block
{

	int ref_counter;

	//The file id of this socket.
	Fid_t fid;

	//The file control block for this socket.
	FCB *fcb;

	//The port of this socket.
	unsigned int port;

	//The type of this socket.
	enum SOCK_TYPE type;

	//The type object of this socket.
	STU sock_type_obj;

}SCB;
//=======================Socket Control Block=====================//




//=========================Request Structure======================//
typedef struct request
{

	SCB     *socket;
	CondVar conn_cv;
	int    accepted;
	rlnode     node;

}RQS;
//=========================Request Structure======================//





//Port Table.
SCB *port_table[MAX_PORT+1];




//=================================Socket Functions=================================//

//Socket OPEN Function.
void *sock_open(uint minor)
{
	return NULL;
}


//Socket READ Function.
int sock_read(void* this, char *buf, unsigned int size)
{

	//Socket variable.
	SCB *socket;

	//Return value.
	int re_val;

	//Try to cast to socket object.
	if ( this != NULL )
		socket = (SCB *)this;

	
	//Failed.
	else
		return -1;

	
	//The socket must be PEER in order to read data.
	if (socket->type != SOCK_PEER)
		return -1;

	//The read of this socket has been shuted down.
	else if ( !socket->sock_type_obj.peer_sock.canRead )
		return -1;

	//Read from the receive pipe.
	re_val=pipe_reader_read( (void *)socket->sock_type_obj.peer_sock.pipe_recv, buf, size );

	

	return re_val;
}


//Socket WRITE Function.
int sock_write(void* this, const char* buf, unsigned int size)
{
	//Socket variable.
	SCB *socket;

	//Return value.
	int re_val;

	//Try to cast to socket object.
	if ( this != NULL )
		socket = (SCB *)this;

	
	//Failed.
	else
		return -1;

	
	//The socket must be PEER in order to read data.
	if (socket->type != SOCK_PEER)
		return -1;

	//Write has been shuted down.
	else if ( !socket->sock_type_obj.peer_sock.canWrite )
		return -1;

	//Write to the send pipe.
	re_val=pipe_writer_write( (void *)socket->sock_type_obj.peer_sock.pipe_send, buf, size );

	return re_val;
}


//Socket CLOSE Function.
int sock_close(void* this)
{

	//Socket variable.
	SCB *socket;

	//Try to cast to socket object.
	if ( this != NULL )
		socket = (SCB *)this;

	
	//Failed.
	else
		return -1;


	//Temp variable.
	enum SOCK_TYPE type = socket->type;
	
	//Set the socket type as closed.
	socket->type = SOCK_CLOSED;


	//If this is a listener.
	if (type == SOCK_LISTEN)
	{
		//unbind the port.
		port_table[socket->port] = NULL;

		//Wake up the listening socket.
		kernel_broadcast( &(socket->sock_type_obj.lis_sock.req) );
	}


	//If it's a Peer socket, call the pipe close functions.
	else if (type == SOCK_PEER)
	{
		pipe_reader_close( (void *)socket->sock_type_obj.peer_sock.pipe_recv );
		pipe_writer_close( (void *)socket->sock_type_obj.peer_sock.pipe_send );	
	}

	//Free the socket.
	free(socket);

	return 0;
}

//=================================Socket Functions=================================//




//-_-_-_-_-_-_-_-_-_-_-_-_-Initialize Socket-_-_-_-_-_-_-_-_-_-_-_-_-//
void initialize_sockets()
{

	//Initialize port table.
	for (int i = 0; i < MAX_PORT+1; i++)
		port_table[i] = NULL;
	
	//Initialize socket file ops.
	sock_ops.Open  = sock_open;
	sock_ops.Read  = sock_read;
	sock_ops.Write = sock_write;
	sock_ops.Close = sock_close;
//-_-_-_-_-_-_-_-_-_-_-_-_-Initialize Socket-_-_-_-_-_-_-_-_-_-_-_-_-//
}




Fid_t sys_Socket(port_t port)
{
	//The port is illegal.
	if (port >= MAX_PORT+1 || port < 0)
		return NOFILE;

	//Define two arrays.
	Fid_t fidts[1];
	FCB   *fcbs[1];

	//Reserve 1 FCB.
	if ( !FCB_reserve(1, fidts, fcbs) )
		return NOFILE;
	
	//Create a new socket object.
	SCB *new_socket = (SCB *)malloc(sizeof(SCB));


	//----------Initialize The Object----------//
	new_socket->fid         = fidts[0];
	new_socket->fcb         = fcbs[0];
	new_socket->port        = port;
	new_socket->ref_counter = 0;
	new_socket->type        = SOCK_UNBOUND;
	//----------Initialize The Object----------//


	//Initialize File Control Block.
	fcbs[0]->streamfunc = &sock_ops;
	fcbs[0]->streamobj  = new_socket;

	//Return the Fid of the socket.
	return fidts[0];
}




int sys_Listen(Fid_t sock)
{

	//Get the fcb object.
	FCB *sock_fcb = get_fcb(sock);

	//Invalid FCB.
	if (sock_fcb == NULL)
		return -1;

	//Get the socket from the file control block.
	SCB *socket = (SCB *)sock_fcb->streamobj;

	//Bad FCB returned null streamobject.
	if (socket == NULL)
		return -1;

	//Socket is not bounded to any port.
	if (socket->port == NOPORT)
		return -1;

	//The port already has a listener.
	if (port_table[socket->port] != NULL)
		return -1;


	//Socket already initialized.
	if (socket->type != SOCK_UNBOUND)
		return -1;

	
	//Initialize the queue of the listener socket.
	rlnode_init( &(socket->sock_type_obj.lis_sock.queue), NULL);

	//Initialize the cond variable of the listening socket.
	socket->sock_type_obj.lis_sock.req = COND_INIT;

	//Change the type to LISTEN.
	socket->type = SOCK_LISTEN;

	//Bind the socket to the port table.
	port_table[socket->port] = socket;
	
	return 0;
}


Fid_t sys_Accept(Fid_t lsock)
{
	//Get the fcb object.
	FCB *sock_fcb = get_fcb(lsock);

	//Invalid FCB.
	if (sock_fcb == NULL)
		return NOFILE;

	//Get the socket from the file control block.
	SCB *socket = (SCB *)sock_fcb->streamobj;

	//Bad FCB returned null streamobject.
	if (socket == NULL)
		return NOFILE;


	//This socket is not a listening socket.
	if (socket->type != SOCK_LISTEN)
		return NOFILE;
	

	//Wait until there is a request.
	while ( rlist_len( &(socket->sock_type_obj.lis_sock.queue) ) <= 0 )
	{

		//The socket was closed.
		if (socket->type == SOCK_CLOSED)
			return NOFILE;

		//Else wait.
		kernel_wait( &(socket->sock_type_obj.lis_sock.req), SCHED_USER );
	}

	//The socket was closed.
	if (socket->type == SOCK_CLOSED)
		return NOFILE;

	
	//Get the first request on the queue.
	rlnode *req_node = rlist_pop_front( &(socket->sock_type_obj.lis_sock.queue) );

	//Get the request object of the node.
	RQS *request = req_node->rqs;

	//Create two pipes.
	PIPCB *pipe1 = create_pipe(socket->fid, request->socket->fid);
	PIPCB *pipe2 = create_pipe(socket->fid, request->socket->fid);

	//Create a new socket to enstablish the connection.
	Fid_t new_sock = sys_Socket(socket->port);

	//Fids are exhausted.
	if (new_sock == NOFILE)
		return NOFILE;

	//Get the new socket object..
	SCB *new_socket = (SCB *) (get_fcb(new_sock)->streamobj);

	//Make the new socket to a PEER socket.
	new_socket->sock_type_obj.peer_sock.other     = (void *)request->socket;
	new_socket->sock_type_obj.peer_sock.pipe_send = pipe1;
	new_socket->sock_type_obj.peer_sock.pipe_recv = pipe2;
	new_socket->sock_type_obj.peer_sock.canRead   = 1;
	new_socket->sock_type_obj.peer_sock.canWrite  = 1;
	new_socket->type = SOCK_PEER;

	//Make the request socket to a Peer socket.
	request->socket->sock_type_obj.peer_sock.other     = (void *)new_socket;
	request->socket->sock_type_obj.peer_sock.pipe_send = pipe2;
	request->socket->sock_type_obj.peer_sock.pipe_recv = pipe1;
	request->socket->sock_type_obj.peer_sock.canRead   = 1;
	request->socket->sock_type_obj.peer_sock.canWrite  = 1;
	request->socket->type = SOCK_PEER;

	//Request accepted.
	request->accepted = 1;

	//Wake up the request socket.
	kernel_broadcast( &(request->conn_cv) );
	
	return new_sock;
}


int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{

	//Get the fcb object.
	FCB *sock_fcb = get_fcb(sock);

	//Invalid FCB.
	if (sock_fcb == NULL)
		return -1;

	//Get the socket from the file control block.
	SCB *socket = (SCB *)sock_fcb->streamobj;

	//Bad FCB returned null streamobject.
	if (socket == NULL)
		return -1;

	//The port is illegal.
	if (port >= MAX_PORT+1 || port < 0)
		return -1;
	
	//The socket in not an UNBOUND SOCKET.
	if (socket->type != SOCK_UNBOUND)
		return -1;

	
	//The port doesn't point to a listener.
	if (port_table[port] == NULL)
		return -1;
	

	//Create a new request object.
	RQS *new_request = (RQS *)malloc(sizeof(RQS));

	//----------Initialize the Request Object----------//
	new_request->accepted = 0;
	new_request->conn_cv  = COND_INIT;
	new_request->socket   = socket;
	rlnode_init( &(new_request->node), new_request );
	//----------Initialize the Request Object----------//

	//Push back the request to the lintener.
	rlist_push_back( &(port_table[port]->sock_type_obj.lis_sock.queue), &(new_request->node) );

	//Wake up the listener to handle the request.
	kernel_broadcast( &(port_table[port]->sock_type_obj.lis_sock.req) );

	//Wait until you wake up or the time expires.
	kernel_timedwait( &(new_request->conn_cv), SCHED_USER, timeout);

	//If the request was accepted.
	if (new_request->accepted)
		return 0;
	
	//Request did not accepted.
	return -1;
}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{

	//Get the fcb object.
	FCB *sock_fcb = get_fcb(sock);

	//Invalid FCB.
	if (sock_fcb == NULL)
		return -1;

	//Get the socket from the file control block.
	SCB *socket = (SCB *)sock_fcb->streamobj;

	//Bad FCB returned null streamobject.
	if (socket == NULL)
		return -1;
	
	//If the socket is not a peer socket, just return successfully.
	else if (socket->type != SOCK_PEER)
		return 0;

	
	//Shutdown Read.
	if (how == SHUTDOWN_READ || how == SHUTDOWN_BOTH){

		//Close the writer of the other socket.
		SCB *s = (SCB *)socket->sock_type_obj.peer_sock.other;
		s->sock_type_obj.peer_sock.canWrite = 0;

		//Close the reader of this socket.
		socket->sock_type_obj.peer_sock.canRead = 0;
	}
	

	//Shutdown write.
	if (how == SHUTDOWN_WRITE || how == SHUTDOWN_BOTH)
	{
		//Close the writer of this socket.
		socket->sock_type_obj.peer_sock.canWrite = 0;
		
		//Make sure that the reader on the other socket will read until EOF.
		socket->sock_type_obj.peer_sock.pipe_send->pip_t.write = -1;
	}
	
	return 0;
}
