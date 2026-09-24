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
void jf_player_seek_to(int position_ms);

/* The item's length in milliseconds, 0 until known, and its chapters' start
 * times, both from the container. */
int jf_player_duration(void);
int jf_player_chapters(const int **starts_ms);

/* The item's audio streams, by the container's names. Selecting one re-seeks
 * to the current position with it. */
int jf_player_audio_count(void);
const char *jf_player_audio_name(int track);
int jf_player_audio_current(void);
void jf_player_audio_select(int track);

/* The item's subtitle tracks as the server lists them, including files beside
 * the video. `index` is the server's stream index; `url` is the server's copy
 * converted to ASS, empty for a bitmap track, which only the container can
 * supply. `stream` is the container's own index for the track, -1 when it is
 * not in there; the player fills it in, since the server numbers files beside
 * the video first and so shifts every index after them. */
typedef struct {
  int index;
  bool external;
  bool text;
  char name[96];
  char url[512];
  int stream;
} jf_player_track;

/* Hands over the list, from the playback info that arrives shortly after
 * jf_player_play, and selects `selected` in it (-1: off). Everything below is
 * empty before then. */
void jf_player_subtitle_tracks(const jf_player_track *tracks, int count,
                               int selected);
int jf_player_subtitle_count(void);
const char *jf_player_subtitle_name(int track);
/* The server's stream index of a track, for reporting the selection back. */
int jf_player_subtitle_stream(int track);
int jf_player_subtitle_current(void);
void jf_player_subtitle_select(int track);
/* While on, every text track in the container is decoded rather than only the
 * selected one, so switching between them shows the line already running once
 * the reader has passed it. For the time a picker is open. */
void jf_player_subtitle_preview(bool on);

/* Where playback is on the item's own timeline in milliseconds - what subtitle
 * times are in - projected from the pipeline's clock, or -1 with no clock yet.
 * Not jf_player_position, which is the coarse displayed position. */
int jf_player_subtitle_ms(void);

jf_player_state jf_player_state_get(void);
const char *jf_player_error(void);
/* Displayed media position in milliseconds. */
int jf_player_position(void);

/* False: the video is on the TV's own plane, not in our framebuffer, so the app punches
 * a transparent hole rather than drawing a background. */
bool jf_player_embedded(void);
bool jf_player_needs_frame(void);
void jf_player_render(uint32_t width, uint32_t height);
