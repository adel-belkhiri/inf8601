#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include "log.h"
#include "filter.h"
#include "thread-pool.h"
#include <sys/prctl.h>

const unsigned short FILTER_OPS_NUMBER = 4;
volatile bool keep_all_workers_alive = true;

/* Save image to a PNG file */
static int
save_image_to_file(void* arg1) {
	if(arg1 == NULL) {
		LOG_ERROR("Error");
		goto fail_exit;
	}

	task_t* task = (task_t*) arg1;
	image_operation_t* ti = task->arg;
	image_t* image = ti->image;

	if(task->stage != SAVE_IMAGE_STAGE) {
		LOG_ERROR("File processing is at wrong pipeline stage\n");
		goto fail_exit;
	}

	task->stage = DONE;
	int ret = image_dir_save(ti->dir, ti->image);

	if(ret) {
		LOG_ERROR("Fail to save file %s\n", ti->image_name);
		goto fail_exit;
	}

	printf(".");
	fflush(stdout);
	return ret;

fail_exit:
	return -1;
}

/**
 * Perform filtering operations on an image file
 */
static int
apply_filter_to_image(void* arg1) {
	if(arg1 == NULL) {
		LOG_ERROR("Error");
		goto fail_exit;
	}

	task_t* task = (task_t*) arg1;

	image_operation_t* op = task->arg;
	image_t* image1 = op->image;
	image_t* image2 = NULL;

	uint8_t mask = (1 << FILTER_OPS_NUMBER) - 1;
	while((op->filter_status & mask) != mask) {

		if((op->filter_status & (1 << FILTER_SCALE_UP_OP)) == 0) {
			image2 = filter_scale_up(image1, 2);
			if(image2 == NULL)
				goto fail_exit;
			op->filter_status |= (1 << FILTER_SCALE_UP_OP);

			image_destroy(image1);
			image1 = image2;
		}

		if ((op->filter_status & (1 << FILTER_DESATURATE_OP)) == 0)
		{
			image2 = filter_desaturate(image1);
			if(image2 == NULL)
				goto fail_exit;
			op->filter_status |= (1 << FILTER_DESATURATE_OP);

			image_destroy(image1);
			image1 = image2;
		}

		if ((op->filter_status & (1 << FILTER_FLIP_HORIZ_OP)) == 0)
		{
			image2 = filter_horizontal_flip(image1);
			if(image2 == NULL)
				goto fail_exit;
			op->filter_status |= (1 << FILTER_FLIP_HORIZ_OP);

			image_destroy(image1);
			image1 = image2;
		}

		if((op->filter_status & (1 << FILTER_SOBEL_OP)) == 0) {
			image2 = filter_sobel(image1);
			if(image2 == NULL)
				goto fail_exit;
			op->filter_status |= (1 << FILTER_SOBEL_OP);

			image_destroy(image1);
			image1 = image2;
		}
	}

	op->image = image2;

	task->stage = SAVE_IMAGE_STAGE;
	task->function = save_image_to_file;
	return 0;

fail_exit:
	LOG_ERROR("Problem applying filter to image\n");
	return -1;
}

/*
 * Load an image from a PNG file
 */
int
load_image_from_file(void* arg1) {
	if(arg1 == NULL) {
		LOG_ERROR("Parameters error ");
		goto fail_exit;
	}

	task_t* task = (task_t*) arg1;
	if(task->stage != LOAD_IMAGE_STAGE) {
		LOG_ERROR("Error");
		goto fail_exit;
	}

	/* load the file */
	image_operation_t* op = task->arg;
    image_t* img = image_create_from_png(op->image_name);
    if (img == NULL) {
        LOG_ERROR("Error");
		goto fail_exit;
    }

    img->id = op->image_id;

	/* update the `task_image`*/
    op->filter_status = 0;
    op->image = img;

	task->stage = FILTER_IMAGE_STAGE;
	task->function = apply_filter_to_image;

	return 0;

fail_exit:
	return -1;
}

static void
free_task(task_t* task){
	if(!task)
		return;

	image_operation_t* op = (image_operation_t*) task->arg;

	if(!op)
		return;

	if(op->image_name)
		free(op->image_name);

	if(op->image)
		image_destroy(op->image);

	free(op);
	free(task);
}

static void
worker_sig_handler(int sig_num) {
	if(sig_num == SIGINT){
		printf("\n\rSIGINT received, stopping pipeline\n");
		__atomic_store_n(&keep_all_workers_alive, false, __ATOMIC_RELAXED);
	}
}

/**
 * Function executed by workers in order to process tasks
 */
static void
func(const void* arg) {

	if(arg == NULL)
		return;

	worker_t* worker = (worker_t*) arg;
	workers_pool_t* worker_pool = worker->workers_pool;

	/* Set a name for this worker */
	char worker_name[64];
	snprintf(worker_name, 32, "worker-%d", worker->id);

	prctl(PR_SET_NAME, worker_name);

	/* then register for a interrupt signal */
	struct sigaction act;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	act.sa_handler = worker_sig_handler;
	if (sigaction(SIGINT, &act, NULL) != 0) {
		LOG_ERROR("thread cannot handle SIGINT signals\n");
	}

	/* Increment the number of alive workers */
	pthread_mutex_lock(&worker_pool->workers_count_lock);
	worker_pool->num_workers_alive += 1;
	if(worker->type == IO_THREAD) {
		if(worker->id & 1)
			worker_pool->num_io_save_workers_alive += 1;
		else
			worker_pool->num_io_load_workers_alive += 1;
	}
	else {
		if(worker->type == COMPUTE_THREAD)
			worker_pool->num_compute_workers_alive += 1;
	}
	pthread_mutex_unlock(&worker_pool->workers_count_lock);

	while(keep_all_workers_alive && worker->keepalive){

		pthread_mutex_lock(&worker_pool->workers_all_idle_mutex);
		while(worker_pool->all_workers_are_idle) {
			errno = pthread_cond_wait(&worker_pool->workers_all_idle_cond, &worker_pool->workers_all_idle_mutex);
			if (errno != 0) {
				LOG_ERROR("pthread_cond_wait");
				break;
			}
		}
		pthread_mutex_unlock(&worker_pool->workers_all_idle_mutex);

		int (*func)(void*);
		pipeline_t** pipeline = &(worker_pool->pipeline);
		if(pipeline == NULL) {
			LOG_ERROR("Pipeline NULL\n");
			return;
		}

		task_t *task = dequeue_task(*pipeline, worker);

		if (task) {
			func = task->function;

			/* Call the processing function */
			int ret = func((void*) task);

			if(ret == 0) {
				enqueue_task(*pipeline, task);
			}
			else {
				LOG_ERROR("Task processing failed\n");
				free_task(task);
			}
		}
	}

	pthread_mutex_lock(&worker_pool->workers_count_lock);
	worker_pool->num_workers_alive --;
	if(worker->type == IO_THREAD) {
		if(worker->id & 1)
					worker_pool->num_io_save_workers_alive --;
				else
					worker_pool->num_io_load_workers_alive --;
	}
	else {
		if(worker->type == COMPUTE_THREAD)
			worker_pool->num_compute_workers_alive --;
	}
	pthread_mutex_unlock(&worker_pool->workers_count_lock);
}

/**
 * Worker thread initialization
 */
static int
init_worker (workers_pool_t* pool, worker_t** worker, int id, uint8_t type){

	*worker = (worker_t*) malloc(sizeof(worker_t));
	if (*worker == NULL) {
		LOG_ERROR("Could not allocate memory for worker thread\n");
		return -1;
	}

	(*worker)->workers_pool = pool;
	(*worker)->keepalive = true;
	(*worker)->id       = id;
	(*worker)->type 	= type;

	pthread_create(&(*worker)->pthread, NULL, (void * (*)(void *)) func, (*worker));
	//pthread_setcancelstate((*worker)->pthread, PTHREAD_CANCEL_ENABLE);
	return 0;
}

/**
 * Pipeline initialization
 */
int
init_pipeline(pipeline_t** pipeline, size_t size) {
	if(pipeline == NULL)
		return -1;

	(*pipeline) = (pipeline_t*) malloc(sizeof(pipeline_t));
	if((*pipeline) == NULL) {
		LOG_ERROR("Could not allocate memory for task pipeline\n");
		return -1;
	}

	/* create the io_load_q queue */
	(*pipeline)->io_load_q = queue_create(size);
	if((*pipeline)->io_load_q == NULL)
		return -2;

	/* create the io_save_q queue */
	(*pipeline)->io_save_q = queue_create(size);
	if((*pipeline)->io_save_q == NULL)
		return -2;

	/* create the compute_filter_q queue */
	(*pipeline)->compute_filter_q = queue_create(size);
	if((*pipeline)->compute_filter_q == NULL)
		return -2;

	pthread_mutex_init(&((*pipeline)->pipeline_idle_mutex), NULL);
	pthread_cond_init(&((*pipeline)->pipeline_idle_cond), NULL);
	(*pipeline)->pipeline_is_idle = false;
	(*pipeline)->pipeline_stop_when_idle = false;

	return 0;
}

/**
 * Free a pipeline
 */
static void
free_pipeline(pipeline_t* pipeline) {
	if(pipeline != NULL) {
	    pthread_mutex_destroy(&pipeline->pipeline_idle_mutex);
	    pthread_cond_destroy(&pipeline->pipeline_idle_cond);

		queue_destroy(pipeline->io_load_q);
		queue_destroy(pipeline->io_save_q);
		queue_destroy(pipeline->compute_filter_q);

		free(pipeline);
	}
}

/**
 * Free worker internal data structures
 */
static void
free_worker (workers_pool_t* w_pool, int id){

	if(w_pool && (id >= 0) && (id<w_pool->num_workers_alive)){
		free(w_pool->workers[id]);
	}
}

/**
 * Free workers pool
 */
void free_workers_pool(workers_pool_t* w_pool) {
	if(w_pool == NULL){
		return;
	}

	if(w_pool->workers){
		for(int i = 0; i<w_pool->num_workers_alive; i++)
			free_worker(w_pool, i);
		free (w_pool->workers);
	}

	if(w_pool->pipeline)
		free_pipeline(w_pool->pipeline);

    pthread_mutex_destroy(&w_pool->workers_count_lock);
    pthread_mutex_destroy(&w_pool->workers_all_idle_mutex);
    pthread_cond_destroy(&w_pool->workers_all_idle_cond);

	free(w_pool);
}

/*
 * Initialize workers pool
 *
 * @p io_threads_count: number of IO worker threads
 * @p compute_threads_count: number of compute worker threads
 * @return a pointer of the workers pool.
 */
workers_pool_t* 
init_workers_pool(int io_threads_count, int compute_threads_count){

	/* Check parameters validity */
	if (io_threads_count < 1 || compute_threads_count < 1){
		return NULL;
	}

	/* Make new thread pool */
	workers_pool_t* w_pool;
	w_pool = (workers_pool_t*) malloc(sizeof(workers_pool_t));
	if (w_pool == NULL){
		LOG_ERROR("Could not allocate memory for workers pool\n");
		return NULL;
	}

	w_pool->num_io_workers = io_threads_count;
	w_pool->num_compute_workers = compute_threads_count;

	w_pool->num_workers_alive   = 0;
	w_pool->num_io_load_workers_alive   = 0;
	w_pool->num_io_save_workers_alive   = 0;
	w_pool->num_compute_workers_alive   = 0;

	w_pool->all_workers_are_idle = true;


	/* Initialize the job queue */
	if (init_pipeline(&w_pool->pipeline, 500) < 0){
		LOG_ERROR("Could not initialize the pipeline\n");
		free(w_pool);
		return NULL;
	}

	/* Make threads in pool */
	int threads_count = io_threads_count + compute_threads_count;
	w_pool->workers = (worker_t**) malloc(threads_count * sizeof(worker_t *));
	if (w_pool->workers == NULL){
		LOG_ERROR("Could not allocate memory for worker threads\n");
		free_pipeline(w_pool->pipeline);
		free(w_pool);
		return NULL;
	}

	/* Initialize pthreads mutex and conditions */
	pthread_mutex_init(&(w_pool->workers_count_lock), NULL);
	pthread_mutex_init(&w_pool->workers_all_idle_mutex, NULL);
	pthread_cond_init(&w_pool->workers_all_idle_cond, NULL);

	/* Initialize IO Threads */
	int i;
	for (i=0; i < io_threads_count; i++){
		init_worker(w_pool, &w_pool->workers[i], i, IO_THREAD);
	}

	/* Initialize Compute Threads */
	for (; i < threads_count; i++){
		init_worker(w_pool, &w_pool->workers[i], i, COMPUTE_THREAD);
	}

	/* Wait for threads to initialize */
	while (w_pool->num_workers_alive != threads_count)
	{};

	return w_pool;
}

/**
 * Dequeue a task from pipeline queues based on worker type
 *
 * @p pipeline: the pipeline
 * @p worker: worker thread
 * @return a task
 */ 
task_t*
dequeue_task(pipeline_t* pipeline, worker_t* worker) {
	if(pipeline == NULL || worker == NULL)
		return NULL;

	queue_t* queue = NULL;
	task_t * task = NULL;

	if(worker->type == IO_THREAD) {
		/* IO Workers having odd IDs are responsible for saving files on disk */
		if(worker->id & 1) {
			task = queue_pop(pipeline->io_save_q);
			if(task == NULL) {
				queue_push(pipeline->io_save_q, NULL);
				worker->keepalive = 0;
			}
		}
		else {
			/* IO Workers having even IDS are responsible for loading files */
			task = queue_pop(pipeline->io_load_q);
			if(task == NULL) {
				queue_push(pipeline->io_load_q, NULL);
				if(worker->workers_pool->num_io_load_workers_alive == 1)
					queue_push(pipeline->compute_filter_q, NULL);
				worker->keepalive = 0;
			}
		}
	}
	else
		if (worker->type == COMPUTE_THREAD) {
			task = queue_pop(pipeline->compute_filter_q);
			if(task == NULL) {
				queue_push(pipeline->compute_filter_q, NULL);
				if(worker->workers_pool->num_compute_workers_alive == 1)
					queue_push(pipeline->io_save_q, NULL);
				worker->keepalive = 0;
			}
	}

	return task;
}

/**
 *  Enqueue a task to the task manager 
 *
 *  @p pipeline:
 *  @p task:
 *  @return
 */
int
enqueue_task(pipeline_t* pipeline, task_t* task) {
	if(pipeline == NULL) {
		return -1;
	}

	/* New or NULL task : queue it to the io_load_q queue*/
	if(task == NULL || task->stage == LOAD_IMAGE_STAGE) {
		queue_push(pipeline->io_load_q, task);
		return 0;
	}
	else if(task->stage == FILTER_IMAGE_STAGE) {

		queue_push(pipeline->compute_filter_q, task);
		return 0;
	}
	else if (task->stage == SAVE_IMAGE_STAGE) {
		/* filtering is done : image need to be saved on disk */
		queue_push(pipeline->io_save_q, task);
		return 0;
	}
	else if (task->stage == DONE) {
		free_task(task);
		return 0;
	}

	return -1;
}

