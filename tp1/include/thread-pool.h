#ifndef INCLUDE_THREAD_POOL_H_
#define INCLUDE_THREAD_POOL_H_

#include <stdint.h>
#include <pthread.h>
#include <sys/types.h>
#include "queue.h"
#include "image.h"


/* Image filtering status */
enum {
	FILTER_SCALE_UP_OP,
	FILTER_DESATURATE_OP,
	FILTER_FLIP_HORIZ_OP,
	FILTER_SOBEL_OP
};


/* Task category */
enum {
	UNKNOWN,
	LOAD_IMAGE_STAGE,
	FILTER_IMAGE_STAGE,
	SAVE_IMAGE_STAGE,
	DONE
};


/* Worker threads types */
typedef enum worker_type {
	UNKNOWN_TYPE_THREAD = 0,
	IO_THREAD 			= 1,
	COMPUTE_THREAD 		= 2
} worker_type_t;


/* Task */
typedef struct task{
	uint8_t stage;
	int  (*function)(void* arg1);
	void*  arg;
} task_t;


/* A structure to track the progress of tasks within the pipeline */
typedef struct image_operation {
	image_dir_t* dir;
	char* image_name;
	image_t* image;

	int image_id;
	uint8_t filter_status : 4;

} image_operation_t;


/* Pipeline with 3 stages */
typedef struct pipeline {
	queue_t* io_load_q;
	queue_t* compute_filter_q;
	queue_t* io_save_q;

	pthread_mutex_t pipeline_idle_mutex;
	pthread_cond_t pipeline_idle_cond;
	volatile bool pipeline_is_idle;
	volatile bool pipeline_stop_when_idle;
} pipeline_t;


typedef struct workers_pool workers_pool_t;

/* Worker Thread */
typedef struct worker{
	int        id;
	worker_type_t type;
	pthread_t  pthread;
	int keepalive;
	workers_pool_t*  workers_pool;
} worker_t;


/* Workers Pool */
struct workers_pool{
	worker_t**   workers;
	volatile int num_io_workers;
	volatile int num_compute_workers;
	volatile int num_workers_alive;

	volatile int num_io_load_workers_alive;
	volatile int num_io_save_workers_alive;
	volatile int num_compute_workers_alive;

	/* used for workers count etc */
	pthread_mutex_t  workers_count_lock;

	volatile bool all_workers_are_idle;

	pthread_mutex_t  workers_all_idle_mutex;
	pthread_cond_t   workers_all_idle_cond;

	pipeline_t*  pipeline;
};


workers_pool_t*
init_workers_pool(int io_threads_count, int compute_threads_count);

void
free_workers_pool(workers_pool_t* w_pool);

task_t*
dequeue_task(pipeline_t* manager, worker_t* worker);

int
enqueue_task(pipeline_t* manager, task_t* task) ;

int
load_image_from_file(void* arg1);

#endif /* INCLUDE_THREAD_POOL_H_ */
