#include "thread_pool.h"
#include <pthread.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>

struct thread_task 
{
    thread_task_f function;
    void *arg;
    void *result;

    pthread_mutex_t task_mutex;
    pthread_cond_t task_cond;

    enum {
        TASK_NEW,
        TASK_PUSHED,
        TASK_RUNNING,
        TASK_FINISHED,
        TASK_DETACHED
    } status;

    bool is_joined;
    bool is_in_pool;
};

struct thread_pool 
{
    pthread_t *threads;
    struct thread_task **task_queue;
    int queue_size;
    int queue_capacity;
    volatile int active_threads;
    int max_threads;

    pthread_mutex_t pool_lock;
    pthread_cond_t pool_cond;

    volatile bool shutdown;

    volatile int count;
};

static void* 
worker_thread(void *arg) 
{
		struct thread_pool *pool = (struct thread_pool *)arg;

		while (1) {
				pthread_mutex_lock(&pool->pool_lock);

				while (pool->queue_size == 0 && !pool->shutdown)
						pthread_cond_wait(&pool->pool_cond, &pool->pool_lock);

				if (pool->shutdown) {
						__atomic_sub_fetch(&pool->count, 1, __ATOMIC_RELEASE);
						pthread_mutex_unlock(&pool->pool_lock);
						break; 
				}

				struct thread_task *task = pool->task_queue[0];
				for (int i = 0; i < pool->queue_size - 1; i++)
						pool->task_queue[i] = pool->task_queue[i + 1];
				pool->queue_size--;
				pool->task_queue[pool->queue_size] = NULL;

				pthread_mutex_lock(&task->task_mutex);
				bool detached_before_run = (task->status == TASK_DETACHED);
				
				if (!detached_before_run)
						task->status = TASK_RUNNING;

				task->is_in_pool = false;

				pthread_mutex_unlock(&task->task_mutex);

				__atomic_add_fetch(&pool->active_threads, 1, __ATOMIC_RELEASE);
				pthread_mutex_unlock(&pool->pool_lock);

				void *result = task->function(task->arg);
				bool should_free_task = false;

				pthread_mutex_lock(&task->task_mutex);

				task->result = result;

				if (task->status == TASK_DETACHED)
						should_free_task = true;
				else {
						task->status = TASK_FINISHED;
						pthread_cond_broadcast(&task->task_cond);
				}

				pthread_mutex_unlock(&task->task_mutex);

				if (should_free_task) {
						pthread_mutex_destroy(&task->task_mutex);
						pthread_cond_destroy(&task->task_cond);
						free(task);
						task = NULL;
				}

				__atomic_sub_fetch(&pool->active_threads, 1, __ATOMIC_RELEASE);
		}

		return NULL;
}

int 
thread_pool_new(int max_thread_count, struct thread_pool **pool) 
{
    if (max_thread_count < 1 || max_thread_count > TPOOL_MAX_THREADS)
        return TPOOL_ERR_INVALID_ARGUMENT;

    *pool = (struct thread_pool*)calloc(1, sizeof(struct thread_pool));
    if (*pool == NULL) 
				return TPOOL_ERR_UNEXPECTED_ERROR;

    (*pool)->threads = (pthread_t*)calloc(max_thread_count, sizeof(pthread_t));
    if ((*pool)->threads == NULL) {
        free(*pool);
        return TPOOL_ERR_UNEXPECTED_ERROR;
    }

    (*pool)->task_queue = (struct thread_task**)calloc(TPOOL_MAX_TASKS, sizeof(struct thread_task*));
    if ((*pool)->task_queue == NULL) {
        free((*pool)->threads);
        free(*pool);
        return TPOOL_ERR_UNEXPECTED_ERROR;
    }

    if (pthread_mutex_init(&(*pool)->pool_lock, NULL) != 0) {
        free((*pool)->task_queue);
        free((*pool)->threads);
        free(*pool);
        return TPOOL_ERR_UNEXPECTED_ERROR;
    }

    if (pthread_cond_init(&(*pool)->pool_cond, NULL) != 0) {
        pthread_mutex_destroy(&(*pool)->pool_lock);
        free((*pool)->task_queue);
        free((*pool)->threads);
        free(*pool);
        return TPOOL_ERR_UNEXPECTED_ERROR;
    }

    (*pool)->max_threads = max_thread_count;
    (*pool)->active_threads = 0;
    (*pool)->queue_capacity = TPOOL_MAX_TASKS;
    (*pool)->queue_size = 0;
    (*pool)->shutdown = false;
    (*pool)->count = 0;

    return 0;
}

int 
thread_pool_thread_count(const struct thread_pool *pool) 
{
    return pool->count;
}

int 
thread_pool_delete(struct thread_pool *pool) 
{
		if (pool == NULL)
				return TPOOL_ERR_INVALID_ARGUMENT;

		pthread_mutex_lock(&pool->pool_lock);

		if (pool->queue_size > 0 || pool->active_threads > 0) {
				pthread_mutex_unlock(&pool->pool_lock);
				return TPOOL_ERR_HAS_TASKS;
		}
		pool->shutdown = true;
		pthread_cond_broadcast(&pool->pool_cond);

		pthread_mutex_unlock(&pool->pool_lock);

		for (int i = 0; i < pool->max_threads; i++) {
				if (pool->threads[i] != 0)
						pthread_join(pool->threads[i], NULL);
		}

		int size = __atomic_load_n(&pool->queue_size, __ATOMIC_SEQ_CST);
		bool has_tasks = size > 0;

		if (has_tasks)
				return TPOOL_ERR_HAS_TASKS;

		free(pool->threads);
		free(pool->task_queue);
		pthread_mutex_destroy(&pool->pool_lock);
		pthread_cond_destroy(&pool->pool_cond);
		free(pool);

		return 0;
}

int 
thread_pool_push_task(struct thread_pool *pool, struct thread_task *task) 
{
		if (pool == NULL || task == NULL)
				return TPOOL_ERR_INVALID_ARGUMENT;

		pthread_mutex_lock(&pool->pool_lock);

		if (pool->queue_size >= pool->queue_capacity) {
				pthread_mutex_unlock(&pool->pool_lock);
				return TPOOL_ERR_TOO_MANY_TASKS;
		}

		if (task->status != TASK_NEW && task->status != TASK_FINISHED) {
				pthread_mutex_unlock(&pool->pool_lock);
				return TPOOL_ERR_TASK_IN_POOL;
		}

		task->status = TASK_PUSHED;
		task->is_in_pool = true;
		task->is_joined = false;
		pool->task_queue[pool->queue_size++] = task;

		if (pool->count < pool->max_threads && pool->active_threads == pool->count) {
				if (pthread_create(&pool->threads[pool->count], NULL, worker_thread, pool) == 0)
						pool->count++;
		}

		pthread_cond_signal(&pool->pool_cond);

		pthread_mutex_unlock(&pool->pool_lock);

		return 0;
}

int 
thread_task_new(struct thread_task **task, thread_task_f function, void *arg) 
{
    if (task == NULL)
        return TPOOL_ERR_INVALID_ARGUMENT;

    *task = (struct thread_task*)calloc(1, sizeof(struct thread_task));
    if (*task == NULL)
        return TPOOL_ERR_UNEXPECTED_ERROR;

    (*task)->function = function;
    (*task)->arg = arg;
    (*task)->status = TASK_NEW;
    (*task)->is_joined = false;
    (*task)->is_in_pool = false;

    if (pthread_mutex_init(&(*task)->task_mutex, NULL) != 0) {
      	free(*task);
      	return TPOOL_ERR_UNEXPECTED_ERROR;
    }

    if (pthread_cond_init(&(*task)->task_cond, NULL) != 0) {
        pthread_mutex_destroy(&(*task)->task_mutex);
        free(*task);
        return TPOOL_ERR_UNEXPECTED_ERROR;
    }

    return 0;
}

bool 
thread_task_is_finished(const struct thread_task *task) 
{
    if (task == NULL)
        return false;

		int status = __atomic_load_n(&task->status, __ATOMIC_SEQ_CST);
    return status == TASK_FINISHED;
}

bool 
thread_task_is_running(const struct thread_task *task) 
{
    if (task == NULL)
        return false;

		int status = __atomic_load_n(&task->status, __ATOMIC_SEQ_CST);
    return status == TASK_RUNNING;
}

int 
thread_task_join(struct thread_task *task, void **result) 
{
		if (task == NULL || result == NULL)
				return TPOOL_ERR_INVALID_ARGUMENT;

		pthread_mutex_lock(&task->task_mutex);

		if (task->status == TASK_NEW || task->status == TASK_DETACHED) {
				pthread_mutex_unlock(&task->task_mutex);
				return TPOOL_ERR_TASK_NOT_PUSHED;
		}

		if (task->is_joined) {
				pthread_mutex_unlock(&task->task_mutex);
				return TPOOL_ERR_TASK_NOT_PUSHED;
		}

		if (task->status != TASK_FINISHED)
				pthread_cond_wait(&task->task_cond, &task->task_mutex);

		*result = task->result;
		task->is_joined = true;

		pthread_mutex_unlock(&task->task_mutex);

		return 0;
}

int 
thread_task_delete(struct thread_task *task) 
{
		if (task == NULL)
				return TPOOL_ERR_INVALID_ARGUMENT;

		pthread_mutex_lock(&task->task_mutex);

		if (task->status == TASK_DETACHED) {
				pthread_mutex_unlock(&task->task_mutex);
				return 0;
		}

		if (task->is_in_pool || (task->status != TASK_FINISHED && task->status != TASK_NEW)) {
				pthread_mutex_unlock(&task->task_mutex);
				return TPOOL_ERR_TASK_IN_POOL;
		}

		pthread_mutex_unlock(&task->task_mutex);

		pthread_mutex_destroy(&task->task_mutex);
		pthread_cond_destroy(&task->task_cond);
		free(task);

		return 0;
}

int 
thread_task_detach(struct thread_task *task) 
{
		if (task == NULL)
				return TPOOL_ERR_INVALID_ARGUMENT;

		pthread_mutex_lock(&task->task_mutex);

		if (task->status == TASK_DETACHED) {
				pthread_mutex_unlock(&task->task_mutex);
				return 0;
		}

		if (task->status == TASK_NEW) {
				pthread_mutex_unlock(&task->task_mutex);
				return TPOOL_ERR_TASK_NOT_PUSHED;
		}

		if (task->status == TASK_FINISHED) {
				if (task->is_joined) {
						pthread_mutex_unlock(&task->task_mutex);
						return TPOOL_ERR_TASK_NOT_PUSHED;
				}
				task->status = TASK_DETACHED;

				pthread_mutex_unlock(&task->task_mutex);

				pthread_mutex_destroy(&task->task_mutex);
				pthread_cond_destroy(&task->task_cond);
				free(task);

				return 0;
		}

		task->status = TASK_DETACHED;

		pthread_mutex_unlock(&task->task_mutex);

		return 0;
}