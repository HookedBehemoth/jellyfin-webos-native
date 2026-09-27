/* Blocking packet handoff between the demuxer and the feeder threads. */
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint8_t *bytes; /* malloc'd; the consumer frees it */
  int size;
  int64_t pts;
} jf_chunk;

#define JF_QUEUE_SLOTS 512
#define JF_QUEUE_CAPACITY_BYTES (8u << 20)

typedef struct {
  jf_chunk slots[JF_QUEUE_SLOTS];
  size_t head, tail, count, bytes;
  bool closed;
  pthread_mutex_t mutex;
  pthread_cond_t room;   /* a slot or byte budget freed up */
  pthread_cond_t filled; /* a chunk arrived, or the queue closed */
} jf_queue;

void jf_queue_init(jf_queue *q);
void jf_queue_destroy(jf_queue *q);

/* Drops anything buffered and reopens the queue. Only once both the producer
 * and the consumer have joined. */
void jf_queue_reset(jf_queue *q);

/* Takes ownership of chunk->bytes even when a shutdown interrupts a blocked
 * producer, so a caller never has to guess who frees a rejected packet. */
bool jf_queue_push(jf_queue *q, const jf_chunk *chunk);

/* False once the queue is closed *and* drained. */
bool jf_queue_pop(jf_queue *q, jf_chunk *out);

/* Wakes blocked producers and consumers. Buffered packets can still drain. */
void jf_queue_close(jf_queue *q);
