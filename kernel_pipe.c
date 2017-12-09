
#include "tinyos.h"
#include "kernel_streams.h"
#include "kernel_cc.h"


//Define the size of the Pipe Buffer.
#define size_of_buffer 4096 //4 kb.

//File Operations Static Variables.
static file_ops reader_ops;
static file_ops writer_ops;



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









//=================================Reader Functions=================================//

//Reader OPEN Function.
void *pipe_reader_open(uint minor)
{
	return NULL;
}


//Reader READ Function.
int pipe_reader_read(void* this, char *buf, unsigned int size)
{

	//Get the pipe object.
	PIPCB *pipe;

	//How many bytes did i read.
	unsigned int counter = 0;

	//Successfully get the object.
	if (this != NULL)
		pipe = (PIPCB *)this;

	//Failed.
	else
		return -1;

	
	//Reader has reached the writer but the writer is still alive!
	while ( pipe->buffer_size == 0 && pipe->pip_t.write != -1 )
		kernel_wait( &(pipe->hasdata), SCHED_PIPE );
	

	//EOF Reached.
	if (pipe->buffer_size == 0 && pipe->pip_t.write == -1)
		return 0;
	
	//Read.
	for (int i = 0; i < size; i++)
	{
		//Read the next byte.
		buf[i] = pipe->buffer[pipe->read_index];

		//Increase the read index.
		pipe->read_index++;

		//Reset the read index.
		if (pipe->read_index >= size_of_buffer)
			pipe->read_index = 0;

		//Increase the counter.
		counter++;

		//Decrease the size of the buffer.
		pipe->buffer_size--;
		

		//Buffer is empty.
		if (pipe->buffer_size == 0)
			break;
	}

	//Wake up all those who wait two write data.
	kernel_broadcast( &(pipe->haspace) );
	
	//Return the amount of read bytes.
	return counter;
}


//Reader WRITE Function.
int pipe_reader_write(void* this, const char* buf, unsigned int size)
{
	return -1;
}


//Reader CLOSE Function.
int pipe_reader_close(void* this)
{

	//Get the pipe object.
	PIPCB *pipe;

	//Successfully get the object.
	if (this != NULL)
		pipe = (PIPCB *)this;

	//Failed.
	else
		return -1;

	//The write FCB closed.
	pipe->pip_t.read = -1;


	//The writer is still using the pipe, so don't destroy it.
	if (pipe->pip_t.write != -1)
		return 0;


	//Free the pipe control block because both reader and writer finished.
	//free(pipe);


	//Return Success.
	return 0;
}

//=================================Reader Functions=================================//








//=================================Writer Functions=================================//

//Writer OPEN Function.
void *pipe_writer_open(uint minor)
{
	return NULL;
}


//Writer READ Function.
int pipe_writer_read(void* this, char *buf, unsigned int size)
{
	return -1;
}


//Writer WRITE Function.
int pipe_writer_write(void* this, const char* buf, unsigned int size)
{

	//Get the pipe object.
	PIPCB *pipe;

	//Counter.
	unsigned int counter = 0;

	//Successfully get the object.
	if (this != NULL)
		pipe = (PIPCB *)this;

	//Failed.
	else
		return -1;

	
	//While the buffer is full, wait.
	while (pipe->buffer_size >= size_of_buffer)
		kernel_wait( &(pipe->haspace), SCHED_PIPE );
	
	//Reader has closed.
	if (pipe->pip_t.read == -1)
		return -1;
	

	//Copy the bytes to the pipe buffer.
	for (int i = 0; i < size; i++)
	{
		//Copy the next byte into the pipe buffer.
		pipe->buffer[pipe->write_index] = buf[i];

		//Increase writer index.
		pipe->write_index++;

		//Reset write index.
		if (pipe->write_index >= size_of_buffer)
			pipe->write_index = 0;

		//Increase the counter.
		counter++;

		//Increase the size of the buffer.
		pipe->buffer_size++;

		//Buffer is full.
		if (pipe->buffer_size >= size_of_buffer)
			break;
	}

	//Wake up all those who wait to read data.
	kernel_broadcast( &(pipe->hasdata) );

	//Return number of bytes written into the pipe buffer.
	return counter;
}


//Writer CLOSE Function.
int pipe_writer_close(void* this)
{
	//Get the pipe object.
	PIPCB *pipe;

	//Successfully get the object.
	if (this != NULL)
		pipe = (PIPCB *)this;

	//Failed.
	else
		return -1;

	//The write FCB closed.
	pipe->pip_t.write = -1;


	//Do a final broadcast.
	kernel_broadcast( &(pipe->hasdata) );


	//The reader is still using the pipe, so don't destroy it.
	if (pipe->pip_t.read != -1)
		return 0;


	//Free the pipe control block because both reader and writer finished.
	//free(pipe);


	//Return Success.
	return 0;
}

//=================================Writer Functions=================================//




//Initialize pipe ops.
void initialize_pipe_ops()
{
	//Initialize Readers functions.
	reader_ops.Open  = pipe_reader_open;
	reader_ops.Read  = pipe_reader_read;
	reader_ops.Write = pipe_reader_write;
	reader_ops.Close = pipe_reader_close;

	//Initialize Writers fucntions.
	writer_ops.Open  = pipe_writer_open;
	writer_ops.Read  = pipe_writer_read;
	writer_ops.Write = pipe_writer_write;
	writer_ops.Close = pipe_writer_close;
}




//System Call Pipe.
int sys_Pipe(pipe_t* pip_t_giveaway)
{

	//Define two arrays.
	Fid_t fidts[2];
	FCB   *fcbs[2];

	//Reserve 2 FCBS.
	if ( !FCB_reserve(2, fidts, fcbs) )
		return -1;


	//Initialize pipe_t object.
	pip_t_giveaway->read  = fidts[0];
	pip_t_giveaway->write = fidts[1];

	
	//Create a new pipe control block.
	PIPCB *new_pipe = (PIPCB *)malloc(sizeof(PIPCB));

	//----------Initialize the Structure----------//
	new_pipe->pip_t.read  =  fidts[0];
	new_pipe->pip_t.write =  fidts[1];
	new_pipe->read_index  =         0;
	new_pipe->write_index = 	0;
	new_pipe->buffer_size = 	0;
	new_pipe->hasdata     = COND_INIT;
	new_pipe->haspace     = COND_INIT;
	//----------Initialize the Structure----------//


	//Stream functions initialization.
	fcbs[0]->streamfunc = &reader_ops;
	fcbs[1]->streamfunc = &writer_ops;

	//Initialize the stream objects of the FCBS.
	fcbs[0]->streamobj = new_pipe;
	fcbs[1]->streamobj = new_pipe;

	//Return success.
	return 0;
}




//Create Pipe Function (For Socket Usage).
PIPCB *create_pipe(Fid_t fid_1, Fid_t fid_2)
{
	//Create a new pipe control block.
	PIPCB *new_pipe = (PIPCB *)malloc(sizeof(PIPCB));

	//----------Initialize the Structure----------//
	new_pipe->pip_t.read  =     fid_1;
	new_pipe->pip_t.write =     fid_2;
	new_pipe->read_index  =         0;
	new_pipe->write_index = 	    0;
	new_pipe->buffer_size = 	    0;
	new_pipe->hasdata     = COND_INIT;
	new_pipe->haspace     = COND_INIT;
	//----------Initialize the Structure----------//


	//Return the new pipe.
	return new_pipe;
}
