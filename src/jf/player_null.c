/*
 * The playback backend for builds with no player.
 *
 * Video on the TV belongs to LG's Starfish pipeline (player.c), which exists nowhere else.
 * The desktop backend is to be libmpv, rendering into this process's own GL context behind
 * this same interface; until it is written, this stands in so that everything above
 * playback - discovery, sign-in, the home rows, the library grid, artwork, navigation, and
 * the UI_SCRIPT/UI_CAPTURE loop - builds and runs on a desktop.
 *
 * It refuses to play and says why, which is what jf_player_play's false return already
 * means to its one caller. The stream URL goes to the log on the way out: it is the thing
 * worth having here, because it can be pasted straight into a player by hand.
 */
#include "player.h"

#include <stdio.h>
#include <string.h>

bool jf_player_audio = true;
bool jf_player_subtitles = true;
const char *jf_player_audio_device = "default";

static jf_player_state state = JF_IDLE;
static const char *error_text = "";

bool jf_player_play(const char *stream_uri, const char *transcode_uri, uint32_t width,
                    uint32_t height, int start_position_ms)
{
    (void)width;
    (void)height;
    (void)start_position_ms;
    fprintf(stderr, "no player in this build\n  stream:    %s\n  transcode: %s\n", stream_uri,
            transcode_uri);
    error_text = "this build has no player; see README.md";
    state = JF_IDLE;
    return false;
}

void jf_player_pause(void) {}
void jf_player_resume(void) {}
void jf_player_stop(void) { state = JF_IDLE; }
void jf_player_deinit(void) {}
void jf_player_seek(int delta_seconds) { (void)delta_seconds; }

jf_player_state jf_player_state_get(void) { return state; }
const char *jf_player_error(void) { return error_text; }
int jf_player_position(void) { return 0; }

/* Both false: nothing is presenting, so nothing asks for a frame and nothing needs a hole
 * punched through the UI. */
bool jf_player_embedded(void) { return false; }
bool jf_player_needs_frame(void) { return false; }
void jf_player_render(uint32_t width, uint32_t height) { (void)width; (void)height; }

/* No pipeline here, so no container and no tracks to offer. */
int jf_player_subtitle_count(void) { return 0; }
const char *jf_player_subtitle_name(int track) {
  (void)track;
  return "";
}
int jf_player_subtitle_current(void) { return -1; }
void jf_player_subtitle_select(int track) { (void)track; }
int jf_player_media_ms(void) { return -1; }
