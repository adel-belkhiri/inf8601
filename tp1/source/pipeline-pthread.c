#include <pthread.h>
#include <unistd.h>
#include <stdlib.h>

#include "filter.h"
#include "log.h"
#include "pipeline.h"
#include "queue.h"
#include "thread-pool.h"

const int num_io_threads = 4;
const int num_compute_threads = 4;

#define MAX(x,y) ((x) >= (y)) ? (x) : (y)

int
image_dir_load_file(image_dir_t* image_dir, char* file_name) {
	const size_t buffer_size = 256;

    if (image_dir->stop | file_name == NULL) {
        goto fail_exit;
    }

    int file_id = image_dir->load_current;
    int count = snprintf(file_name, buffer_size, "%s/%04d.png", image_dir->name, file_id);
    if (count >= buffer_size - 1) {
        LOG_ERROR("buffer too small");
        goto fail_exit;
    }

	if (access(file_name, F_OK) < 0) {
        goto fail_exit;
    }
    image_dir->load_current++;
    return file_id;

fail_exit:
    return -1;
}


int
pipeline_pthread(image_dir_t* image_dir) {

	workers_pool_t* w_pool;
	const size_t buffer_size = 256;

	w_pool = init_workers_pool(num_io_threads, num_compute_threads);
	if(w_pool == NULL) {
		 LOG_ERROR("Error initializing threads pool\n");
		 return -1;
	}

	pipeline_t* tsk_pool  = w_pool->pipeline;
	task_t* tsk;
	image_operation_t* op;

	int file_id;
	char* file_name;
	do {

		file_name = (char*) malloc(sizeof(char) * 256);
		file_id = image_dir_load_file(image_dir, file_name);
		if(file_id < 0) {
			free(file_name);
			break;
		}

		op = (image_operation_t*) malloc(sizeof(image_operation_t));
		tsk = (task_t*) malloc (sizeof(task_t));

		if(tsk == NULL || op == NULL) {
			LOG_ERROR("Error allocating memory\n");
			return -1;
		}

		op->dir = image_dir;
		op->image_id = file_id;
		op->image_name = file_name;

		tsk->function = load_image_from_file;
		tsk->stage = LOAD_IMAGE_STAGE;
		tsk->arg = op;

		enqueue_task(tsk_pool, tsk);
	} while (1);

	image_dir->stop = true;

	/* Push NULL to mark the end of tasks */
	enqueue_task(tsk_pool, NULL);

	pthread_mutex_lock(&w_pool->workers_all_idle_mutex);
	w_pool->all_workers_are_idle = false;
	errno = pthread_cond_broadcast(&w_pool->workers_all_idle_cond);
	if (errno != 0) {
		LOG_ERROR_ERRNO("pthread_cond_signal");
		return -1;
	}
	pthread_mutex_unlock(&w_pool->workers_all_idle_mutex);

	/* Wait for child workers here */
	for(int i=0; i< w_pool->num_workers_alive; i++){
		pthread_join(w_pool->workers[i]->pthread, NULL);
	}

	return 0;
}
