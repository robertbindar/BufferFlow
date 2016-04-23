/* vim: set ts=8 sts=2 et sw=2: */

#include "buffer_scheduler.h"
#include <pthread.h>
#include <stdio.h>

static inline void mrucache_put(buffer_scheduler_t *bufsched, cls_buf_t *buf);
static inline void mrucache_get(buffer_scheduler_t *bufsched, cls_buf_t **buf);
static inline void mrucache_remove(buffer_scheduler_t *bufsched, cls_buf_t *buf);

static void *stretch_allocator(void *arg)
{
  buffer_scheduler_t *bufsched = (buffer_scheduler_t *) arg;

  while (1) {
    // Wait to be signaled
    pthread_mutex_lock(&bufsched->lock);
    while (!bufsched->resize_alloc && !bufsched->shrink_alloc) {
      pthread_cond_wait(&bufsched->cond_alloc, &bufsched->lock);
    }

    if (bufsched->shrink_alloc) {
      uint64_t count = bufsched->nr_free_buffers / 2;
      bufsched->nr_free_buffers -= count;

      bufsched->shrink_alloc = 0;
      pthread_mutex_unlock(&bufsched->lock);

      bufsched->capacity -= count;
      allocator_shrink(&bufsched->allocator, count);
    } else {
      bufsched->resize_alloc = 0;

      if (bufsched->nr_free_buffers + bufsched->nr_assigned_buffers >= bufsched->max_pool_size) {
        pthread_mutex_unlock(&bufsched->lock);

        fprintf(stderr, "call the swapper \n");

        // Call the swapper to freeup some memory
        for (uint64_t i = 0; i < bufsched->nr_swapout; ++i) {
          cls_buf_t *buf = NULL;

          mrucache_get(bufsched, &buf);

          swapper_swapout(&bufsched->swapper, buf);

          allocator_move_to_free(&bufsched->allocator, buf);

          pthread_mutex_lock(&bufsched->lock);
          bufsched->nr_free_buffers++;
          bufsched->nr_assigned_buffers--;
          pthread_mutex_unlock(&bufsched->lock);
        }

        continue;
      }

      uint64_t count = bufsched->max_pool_size - bufsched->nr_free_buffers - bufsched->nr_assigned_buffers;
      if (count > bufsched->capacity) {
        count = bufsched->capacity;
        bufsched->capacity = bufsched->capacity * 2;
      } else if (count > bufsched->max_free_buffers) {
        count = bufsched->max_free_buffers - 1;
        bufsched->capacity += count;
      } else {
        bufsched->capacity += count;
      }
      bufsched->nr_free_buffers += count;

      pthread_mutex_unlock(&bufsched->lock);

      allocator_new(&bufsched->allocator, count);
    }
  }

  return NULL;
}

error_code sched_init(buffer_scheduler_t *bufsched, uint64_t buffer_size,
                      uint64_t max_pool_size)
{
  pthread_mutex_init(&bufsched->lock, NULL);

  bufsched->buffer_size = buffer_size;
  bufsched->max_pool_size = max_pool_size;
  bufsched->nr_assigned_buffers = 0;

  // min_free_buffers has to be bigger than 0
  bufsched->min_free_buffers = 300;
  bufsched->max_free_buffers = max_pool_size / 4 + 1;

  bufsched->capacity = 50;
  bufsched->nr_free_buffers = bufsched->capacity;

  pthread_cond_init(&bufsched->cond_alloc, NULL);
  bufsched->resize_alloc = 0;
  bufsched->shrink_alloc = 0;

  allocator_init(&bufsched->allocator, buffer_size);

  allocator_new(&bufsched->allocator, bufsched->nr_free_buffers);

  // dispatch async allocators
  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  pthread_create(&tid, &attr, stretch_allocator, bufsched);

  pthread_attr_destroy(&attr);

  dllist_init(&bufsched->mrucache);
  bufsched->nr_swapout = 10;
  swapper_init(&bufsched->swapper, bufsched->buffer_size);

  return BUFFERING_SUCCESS;
}

error_code sched_destroy(buffer_scheduler_t *bufsched)
{
  pthread_mutex_destroy(&bufsched->lock);

  pthread_cond_destroy(&bufsched->cond_alloc);

  allocator_destroy(&bufsched->allocator);

  swapper_destroy(&bufsched->swapper);

  return BUFFERING_SUCCESS;
}

error_code sched_alloc(buffer_scheduler_t *bufsched, cls_buf_t **buffer, cls_buf_handle_t bh)
{
  allocator_get(&bufsched->allocator, buffer, bh); // stuck: TODO

  pthread_mutex_lock(&bufsched->lock);

  bufsched->nr_assigned_buffers++;
  bufsched->nr_free_buffers--;

  // If there are no more free buffers, allocate more. This means we could
  // insert some free buffers into the allocator or call the swapper to freeup
  // some memory if there's no available memory.
  if (bufsched->nr_free_buffers <= bufsched->min_free_buffers) {
    bufsched->resize_alloc = 1;
    pthread_cond_signal(&bufsched->cond_alloc);
  }

  pthread_mutex_unlock(&bufsched->lock);

  return BUFFERING_SUCCESS;
}

error_code sched_free(buffer_scheduler_t *bufsched, cls_buf_t *buffer)
{
  allocator_put(&bufsched->allocator, buffer);

  // Remove the buffer from the mru cache if it exists
  mrucache_remove(bufsched, buffer);

  pthread_mutex_lock(&bufsched->lock);

  bufsched->nr_assigned_buffers--;
  bufsched->nr_free_buffers++;

  // TODO: async swapin buffers
  // Release some memory, there are too many free buffers allocated
  if (bufsched->nr_free_buffers == bufsched->max_free_buffers) {
    bufsched->shrink_alloc = 1;
    pthread_cond_signal(&bufsched->cond_alloc);
  }

  pthread_mutex_unlock(&bufsched->lock);

  return BUFFERING_SUCCESS;
}

void sched_mark_updated(buffer_scheduler_t *bufsched, cls_buf_t *buf)
{
  mrucache_put(bufsched, buf);
}

void sched_swapin(buffer_scheduler_t *bufsched, cls_buf_t *buf)
{
  // Allow the swapin operation to drop the free_buffers count below the window limit
  uint8_t memory_full = 0;
  pthread_mutex_lock(&bufsched->lock);
  if (bufsched->nr_free_buffers == 0) {
    memory_full = 1;
  }
  pthread_mutex_unlock(&bufsched->lock);

  if (memory_full) {
    cls_buf_t *mru = NULL;
    mrucache_get(bufsched, &mru);

    swapper_swapout(&bufsched->swapper, mru);

    allocator_move(&bufsched->allocator, buf, mru);
  } else {
    pthread_mutex_lock(&bufsched->lock);
    bufsched->nr_free_buffers--;
    bufsched->nr_assigned_buffers++;
    pthread_mutex_unlock(&bufsched->lock);

    allocator_move_from_free(&bufsched->allocator, buf);
  }

  swapper_swapin(&bufsched->swapper, buf);
}

static inline void mrucache_put(buffer_scheduler_t *bufsched, cls_buf_t *buf)
{
  pthread_mutex_lock(&bufsched->lock);
  dllist_iah(&bufsched->mrucache, &buf->link_mru);
  pthread_mutex_unlock(&bufsched->lock);
}

static inline void mrucache_get(buffer_scheduler_t *bufsched, cls_buf_t **buf)
{
  pthread_mutex_lock(&bufsched->lock);
  dllist_link *tmp = dllist_rem_head(&bufsched->mrucache);
  *buf = DLLIST_ELEMENT(tmp, cls_buf_t, link_mru);
  pthread_mutex_unlock(&bufsched->lock);
}

static inline void mrucache_remove(buffer_scheduler_t *bufsched, cls_buf_t *buf)
{
  pthread_mutex_lock(&bufsched->lock);
  dllist_rem(&bufsched->mrucache, &buf->link_mru);
  pthread_mutex_unlock(&bufsched->lock);
}

