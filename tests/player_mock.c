/*
 * A player that always starts, for the UI tests: the playback screen, its
 * chrome and the subtitle picker are reachable without a pipeline. Nothing
 * moves - the position is wherever the last seek put it - and there is no
 * picture or subtitle to draw.
 */
#include "jf/player.h"

bool jf_player_audio = true;
jf_decoders jf_player_decoders;
bool jf_player_subtitles = true;
const char *jf_player_audio_device = "default";

static jf_player_state state = JF_IDLE;
static int position_ms;
static int subtitle = -1;
static int subtitle_count;
static jf_player_track tracks[16];
static int audio;
/* The last one the closing credits, as a show's own. */
static const int chapters[] = {0, 10 * 60000, 30 * 60000, 40 * 60000};

#define DURATION_MS (45 * 60000)

bool jf_player_play(const char *stream_uri, const char *transcode_uri,
                    uint32_t width, uint32_t height, int start_position_ms) {
  (void)stream_uri, (void)transcode_uri, (void)width, (void)height;
  state = JF_PLAYING;
  position_ms = start_position_ms;
  subtitle = -1;
  subtitle_count = 0;
  audio = 0;
  return true;
}

void jf_player_pause(void) {}
void jf_player_resume(void) {}
void jf_player_stop(void) { state = JF_IDLE; }
/* The stream running out by itself, as a stop does without the UI asking. */
void player_mock_end(void) { state = JF_IDLE; }
void jf_player_deinit(void) {}
void jf_player_seek_to(int ms) {
  position_ms = ms < 0 ? 0 : ms > DURATION_MS ? DURATION_MS : ms;
}
void jf_player_seek(int delta_seconds) {
  jf_player_seek_to(position_ms + delta_seconds * 1000);
}
int jf_player_duration(void) { return DURATION_MS; }
int jf_player_chapters(const int **starts_ms) {
  *starts_ms = chapters;
  return (int)(sizeof(chapters) / sizeof(chapters[0]));
}

int jf_player_audio_count(void) { return 2; }
const char *jf_player_audio_name(int track) {
  return track == 0 ? "English" : "Commentary";
}
int jf_player_audio_stream(int track) { return track; }
int jf_player_audio_current(void) { return audio; }
void jf_player_audio_select(int track) { audio = track; }

void jf_player_subtitle_tracks(const jf_player_track *list, int count,
                               int selected) {
  subtitle_count = count < 16 ? count : 16;
  for (int i = 0; i < subtitle_count; i++)
    tracks[i] = list[i];
  subtitle = selected;
}
int jf_player_subtitle_count(void) { return subtitle_count; }
const char *jf_player_subtitle_name(int track) { return tracks[track].name; }
int jf_player_subtitle_stream(int track) {
  return track < 0 ? -1 : tracks[track].index;
}
int jf_player_subtitle_current(void) { return subtitle; }
void jf_player_subtitle_select(int track) { subtitle = track; }
void jf_player_subtitle_preview(bool on) { (void)on; }
int jf_player_subtitle_ms(void) { return -1; }

jf_player_state jf_player_state_get(void) { return state; }
const char *jf_player_error(void) { return ""; }
int jf_player_position(void) { return position_ms; }
bool jf_player_embedded(void) { return false; }
bool jf_player_needs_frame(void) { return false; }
void jf_player_render(uint32_t width, uint32_t height) {
  (void)width, (void)height;
}
