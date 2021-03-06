/* vim: set ts=8 sts=2 et sw=2: */

#include "buffer_scheduler.h"
#include "buffering_types.h"

error_code sched_init(buffer_scheduler_t *bufsched, uint64_t buffer_size,
                      uint64_t max_pool_size)
{
  HANDLE_ERR(pthread_mutex_init(&bufsched->lock, NULL), BUFSCHEDULER_LOCK_ERR);

  bufsched->buffer_size = buffer_size;
  bufsched->max_pool_size = max_pool_size;
  bufsched->nr_assigned_buffers = 0;

  allocator_init(&bufsched->allocator, buffer_size);

  bufsched->nr_free_buffers = max_pool_size;

  return BUFFERING_SUCCESS;
}

error_code sched_destroy(buffer_scheduler_t *bufsched)
{
  HANDLE_ERR(pthread_mutex_destroy(&bufsched->lock), BUFSCHEDULER_LOCK_ERR);

  allocator_destroy(&bufsched->allocator);

  return BUFFERING_SUCCESS;
}

static int init = 0;
error_code sched_alloc(buffer_scheduler_t *bufsched, cls_buf_t **buffer, cls_buf_handle_t bh)
{
  HANDLE_ERR(pthread_mutex_lock(&bufsched->lock), BUFSCHEDULER_LOCK_ERR);
  if (init == 0) {
    allocator_new(&bufsched->allocator, bufsched->max_pool_size);
    init = 1;
  }
  HANDLE_ERR(pthread_mutex_unlock(&bufsched->lock), BUFSCHEDULER_LOCK_ERR);

  allocator_get(&bufsched->allocator, buffer, bh);

  return BUFFERING_SUCCESS;
}

error_code sched_free(buffer_scheduler_t *bufsched, cls_buf_t *buffer)
{
  allocator_put(&bufsched->allocator, buffer);

  return BUFFERING_SUCCESS;
}

