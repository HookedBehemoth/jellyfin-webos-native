/*
 * Where playback actually is, taken from the pipeline rather than guessed at.
 *
 * The load payload sets `queryPosition`, and Starfish then reports the
 * presentation timestamp of each displayed frame as the numeric value of a
 * FRAMEREADY event. That is the whole clock: a pts paired with the host time
 * the event arrived, sampled at the moment of the flip.
 *
 * This replaces polling getCurrentPlaytime(). That call is frame-quantized and
 * sometimes slow, so reading it needed a sampling period, a slow-query
 * rejection, a half-interval bracket to guess when the frame had really
 * flipped, and a stability probe before the result could be trusted. An event
 * that arrives *at* the flip needs none of it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define JF_CLOCK_NONE INT64_MIN

int64_t jf_now_ns(void);

void jf_clock_reset(void);
/* From the pipeline's thread, on every FRAMEREADY. */
void jf_clock_sample(int64_t pts_ns, int64_t host_ns);
/* PLAYING/PAUSED transitions: a stopped clock holds its last value instead of
 * projecting past it. */
void jf_clock_set_running(bool running);
bool jf_clock_ready(void);

/* Projected media position now, or JF_CLOCK_NONE when there is no usable
 * anchor. */
int64_t jf_clock_pts(void);
/* Host time at which media timestamp `pts_ns` is presented, or JF_CLOCK_NONE.
 * This is the line audio is pulled onto. */
int64_t jf_clock_host_for(int64_t pts_ns);
