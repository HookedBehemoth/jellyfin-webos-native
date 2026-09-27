#include "packet_queue.h"

#include <stdlib.h>
#include <string.h>

void jf_queue_init(jf_queue *q) {
  memset(q, 0, sizeof(*q));
  pthread_mutex_init(&q->mutex, NULL);
  pthread_cond_init(&q->room, NULL);
  pthread_cond_init(&q->filled, NULL);
}

void jf_queue_destroy(jf_queue *q) {
  jf_queue_reset(q);
  pthread_mutex_destroy(&q->mutex);
  pthread_cond_destroy(&q->room);
  pthread_cond_destroy(&q->filled);
}

void jf_queue_reset(jf_queue *q) {
  pthread_mutex_lock(&q->mutex);
  while (q->count > 0) {
    free(q->slots[q->head].bytes);
    q->head = (q->head + 1) % JF_QUEUE_SLOTS;
    q->count--;
  }
  q->head = q->tail = q->count = q->bytes = 0;
  q->closed = false;
  pthread_cond_broadcast(&q->room);
  pthread_cond_broadcast(&q->filled);
  pthread_mutex_unlock(&q->mutex);
}

bool jf_queue_push(jf_queue *q, const jf_chunk *chunk) {
  pthread_mutex_lock(&q->mutex);
  /* Permit one oversized packet into an empty queue, but bound read-ahead. */
  while (!q->closed && (q->count == JF_QUEUE_SLOTS ||
                        (q->bytes != 0 && q->bytes + (size_t)chunk->size >
                                              JF_QUEUE_CAPACITY_BYTES)))
    pthread_cond_wait(&q->room, &q->mutex);
  if (q->closed) {
    pthread_mutex_unlock(&q->mutex);
    free(chunk->bytes);
    return false;
  }
  q->slots[q->tail] = *chunk;
  q->tail = (q->tail + 1) % JF_QUEUE_SLOTS;
  q->count++;
  q->bytes += (size_t)chunk->size;
  pthread_cond_signal(&q->filled);
  pthread_mutex_unlock(&q->mutex);
  return true;
}

bool jf_queue_pop(jf_queue *q, jf_chunk *out) {
  pthread_mutex_lock(&q->mutex);
  while (q->count == 0 && !q->closed)
    pthread_cond_wait(&q->filled, &q->mutex);
  if (q->count == 0) {
    pthread_mutex_unlock(&q->mutex);
    return false;
  }
  *out = q->slots[q->head];
  q->head = (q->head + 1) % JF_QUEUE_SLOTS;
  q->count--;
  q->bytes -= (size_t)out->size;
  pthread_cond_signal(&q->room);
  pthread_mutex_unlock(&q->mutex);
  return true;
}

void jf_queue_close(jf_queue *q) {
  pthread_mutex_lock(&q->mutex);
  q->closed = true;
  pthread_cond_broadcast(&q->room);
  pthread_cond_broadcast(&q->filled);
  pthread_mutex_unlock(&q->mutex);
}
