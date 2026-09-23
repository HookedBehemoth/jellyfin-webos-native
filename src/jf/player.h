/*
 * Jellyfin playback: FFmpeg demuxes, LG's Starfish pipeline decodes and presents the
 * video on the TV's own plane, and the decoded audio goes to ALSA.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Read when playback starts. */
extern bool jf_player_audio;
extern bool jf_player_subtitles;
extern const char *jf_player_audio_device;

typedef enum { JF_IDLE, JF_LOADING, JF_PLAYING, JF_FAILED } jf_player_state;

bool jf_player_play(const char *stream_uri, const char *transcode_uri,
                    uint32_t width, uint32_t height, int start_position_ms);
void jf_player_pause(void);
void jf_player_resume(void);
void jf_player_stop(void);
void jf_player_deinit(void);

/* Jump `delta_seconds` from the displayed position. The segment loop performs the seek
 * once the feed threads have parked, so this only has to ask. */
void jf_player_seek(int delta_seconds);

/* Text subtitle tracks found in the container, rendered with libass onto the
 * graphics plane; see jf/subs.h. The list is complete before the state reaches
 * JF_PLAYING and empty at every other time. `current` is an index into it, or
 * -1 for off. */
int jf_player_subtitle_count(void);
const char *jf_player_subtitle_name(int track);
int jf_player_subtitle_current(void);
void jf_player_subtitle_select(int track);

/* Projected position on the current segment's timeline in milliseconds - the
 * line video, audio and subtitles are all placed on - or -1 with no clock yet.
 * Not the same thing as jf_player_position, which is where in the *item*
 * playback is. */
int jf_player_media_ms(void);

jf_player_state jf_player_state_get(void);
const char *jf_player_error(void);
/* Displayed media position in milliseconds. */
int jf_player_position(void);

/* False: the video is on the TV's own plane, not in our framebuffer, so the app punches
 * a transparent hole rather than drawing a background. */
bool jf_player_embedded(void);
bool jf_player_needs_frame(void);
void jf_player_render(uint32_t width, uint32_t height);
