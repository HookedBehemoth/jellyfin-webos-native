/*
 * What the two rendering probes have in common: shader compilation, the bottom-right
 * frame-time overlay, and the ASCII framebuffer dump that lets either one be checked over
 * ssh with no screen.
 *
 * They exist to measure the TV's GPU, so the measuring itself is the shared part; what
 * differs is only the scene each one draws.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../platform/gl.h"

/* Panics with the info log rather than returning: a probe with a broken shader has
 * nothing left to measure. */
unsigned probe_program(const unsigned char *vertex, const unsigned char *fragment);

#define PROBE_OVERLAY_COLS 22
#define PROBE_OVERLAY_ROWS 8
#define PROBE_OVERLAY_ZOOM 2 /* on-screen magnification of the 1x rasterisation */

typedef struct {
    unsigned texture;
    unsigned program;
    unsigned uniform_buffer;
    uint8_t *pixels;
    int width, height;
} probe_overlay;

/* `text_vs`/`text_fs` are the compiled text shader; the overlay sits in the bottom-right
 * corner of a `width` x `height` framebuffer. */
bool probe_overlay_init(probe_overlay *overlay, const unsigned char *text_vs,
                        const unsigned char *text_fs, int width, int height);
void probe_overlay_clear(probe_overlay *overlay);
void probe_overlay_line(probe_overlay *overlay, int row, const char *text);
/* Uploads and draws. The caller's vertex array must already have the unit quad bound. */
void probe_overlay_draw(probe_overlay *overlay);

/* How GPU time gets measured: the timer query when the driver really implements it,
 * otherwise a glFinish stopwatch. Mali-G52 r46p0 advertises the extension and never
 * returns a result, so the fallback is not theoretical. */
typedef enum { PROBE_GPU_QUERY, PROBE_GPU_FINISH } probe_gpu_mode;

typedef struct {
    probe_gpu_mode mode;
    unsigned queries[2];
    uint64_t frames;
    double cpu_ms, gpu_ms, frame_ms;
    uint64_t last_frame_ns;
    uint64_t cpu_start_ns;
} probe_timer;

void probe_timer_init(probe_timer *timer);
/* Call at the top of a frame, before any drawing. */
void probe_timer_begin(probe_timer *timer);
/* Call after the last draw of the frame, before the swap. */
void probe_timer_end(probe_timer *timer);
const char *probe_gpu_mode_name(const probe_timer *timer);

uint64_t probe_now_ns(void);

/* Read the frame back and print it as coarse ASCII, so a render can be checked without
 * looking at a screen. */
void probe_dump_frame(void);
