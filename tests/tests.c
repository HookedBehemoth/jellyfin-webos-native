/* Host tests for the parts of the app that do not need a TV.
 *
 * Run with `cmake --preset host && ctest --preset host`.
 */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "jf/audio_sync.h"
#include "jf/blurhash.h"
#include "jf/cfg.h"
#include "jf/clock.h"
#include "jf/smp_payload.h"
#include "jf/store.h"
#include "platform/luna.h"
#include "ui/loom.h"
#include "ui/skyline.h"
#ifdef JF_HAVE_DEMUX
#include "jf/demux.h"
#endif
#ifdef JF_HAVE_SUBS
#include "jf/subs.h"
#endif

#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static int failures;

/* The load payload is the contract with libpf; a missing key is a silent behaviour change
 * on the TV rather than an error, so the keys that matter are named here. */
static void test_load_payload(void)
{
    char payload[4096];
    const smp_video_params video = {"dev.hookedbehemoth.jellyfin", "_Window_Id_66", "H265",
                                    1920, 1080, 24000, 1001};
    CHECK(smp_payload_load(payload, sizeof(payload), &video));
    static const char *const expected[] = {
        "\"mediaTransportType\":\"BUFFERSTREAM\"",
        "\"seekMode\":\"keep-rate\"",
        "\"queryPosition\":true",
        "\"format\":\"RAW\"",
        "\"streamQualityInfo\":true",
        "\"streamQualityInfoNonFlushable\":true",
        "\"streamQualityInfoCorruptedFrame\":true",
        "\"videoFpsValue\":24000,\"videoFpsScale\":1001",
        "\"pauseAtDecodeTime\":true",
        "\"seperatedPTS\":true",
        "\"srcBufferLevelVideo\":{\"minimum\":1048576,\"maximum\":8388608}",
        "\"qBufferLevelAudio\":0",
        "\"srcBufferLevelAudio\":{\"minimum\":1048576,\"maximum\":2097152}",
        "\"windowId\":\"_Window_Id_66\"",
        /* Audio left the pipeline entirely: it goes to ALSA, and the video sink has to
         * run off the system clock because nothing else establishes one. */
        "\"needAudio\":false",
        "\"audioSync\":false",
        "\"useCurrentTimeWithSystemClock\":true",
    };
    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++)
        if (strstr(payload, expected[i]) == NULL) {
            fprintf(stderr, "FAIL missing %s\nin %s\n", expected[i], payload);
            failures++;
        }
    CHECK(strstr(payload, "pcmInfo") == NULL);
    CHECK(strstr(payload, "\"audio\"") == NULL);

    /* A frame rate of zero leaves the pair out rather than sending zeroes, which the
     * pipeline would read as "believe maxFrameRate instead". */
    const smp_video_params no_fps = {"app", "_Window_Id_1", "H264", 1280, 720, 0, 0};
    CHECK(smp_payload_load(payload, sizeof(payload), &no_fps));
    CHECK(strstr(payload, "videoFpsValue") == NULL);

    /* A payload that does not fit must be refused, not truncated: half a JSON document is
     * not a JSON document. */
    char tiny[32];
    CHECK(!smp_payload_load(tiny, sizeof(tiny), &video));
}

static void test_feed_and_control_payloads(void)
{
    char payload[160];
    const char buffer[] = "es";
    CHECK(smp_payload_feed(payload, sizeof(payload), buffer, sizeof(buffer), 1500000000, 1));
    CHECK(strstr(payload, "\"pts\":1500000000") != NULL);
    CHECK(strstr(payload, "\"esData\":1") != NULL);

    /* flush's two keys are the only ones StarfishMediaAPIs::flush parses, and audioFlush
     * is false because there is no audio appsrc any more. */
    CHECK(smp_payload_flush(payload, sizeof(payload), 0));
    CHECK(strcmp(payload, "{\"audioFlush\":false,\"offset\":0}") == 0);
    CHECK(smp_payload_play_rate(payload, sizeof(payload), 1000));
    CHECK(strcmp(payload, "{\"audioOutput\":false,\"playRate\":1.000}") == 0);
}

/* The whole of A/V sync now lives in this one function, so it is the one that has to be
 * right: audio due later than the queue's tail is padded, audio already overdue is
 * dropped, and a wild timestamp is capped rather than stalling the track. */
static void test_audio_placement(void)
{
    const int rate = 48000;
    const int64_t ms = 1000000;

    /* Within the threshold: write it as it is. */
    CHECK(jf_audio_placement(1000 * ms, 1000 * ms, rate) == 0);
    CHECK(jf_audio_placement(1000 * ms, 1010 * ms, rate) == 0);
    CHECK(jf_audio_placement(1010 * ms, 1000 * ms, rate) == 0);

    /* Due 100 ms after the queue runs dry: 100 ms of silence, in frames. */
    CHECK(jf_audio_placement(1100 * ms, 1000 * ms, rate) == rate / 10);
    /* Already 100 ms late: drop that many frames from the head. */
    CHECK(jf_audio_placement(1000 * ms, 1100 * ms, rate) == -(rate / 10));
    /* A ten-second gap is capped at the maximum pad; the chunks after it converge. */
    CHECK(jf_audio_placement(11000 * ms, 1000 * ms, rate) ==
          JF_AUDIO_MAX_PAD_NS * rate / 1000000000LL);
}

/* The clock is a pts and the host time it arrived, projected forward - and deliberately
 * unusable when it has gone stale or playback has stopped. */
static void test_clock(void)
{
    const int64_t ms = 1000000;
    jf_clock_reset();
    CHECK(jf_clock_pts() == JF_CLOCK_NONE);
    CHECK(jf_clock_host_for(0) == JF_CLOCK_NONE);

    const int64_t host = jf_now_ns();
    jf_clock_sample(5000 * ms, host);
    jf_clock_set_running(true);
    /* host_for is the line audio is pulled onto: one second later in the media is one
     * second later on the host clock. */
    CHECK(jf_clock_host_for(6000 * ms) == host + 1000 * ms);
    CHECK(jf_clock_host_for(4000 * ms) == host - 1000 * ms);

    /* A stray sample that rewinds further than the tolerance is ignored. */
    jf_clock_sample(1000 * ms, host);
    CHECK(jf_clock_host_for(5000 * ms) == host);
    /* Forward progress is accepted. */
    jf_clock_sample(5040 * ms, host + 40 * ms);
    CHECK(jf_clock_host_for(5040 * ms) == host + 40 * ms);

    /* A stopped pipeline holds its last position instead of projecting past it. */
    jf_clock_set_running(false);
    CHECK(jf_clock_pts() == 5040 * ms);
    jf_clock_reset();
}

static void test_luna_event(void)
{
    char event[32];
    CHECK(jf_luna_json_string("{\"event\":\"close\",\"reason\":\"memoryReclaim\"}", "event",
                              event, sizeof(event)) &&
          strcmp(event, "close") == 0);
    CHECK(jf_luna_json_string("{\"returnValue\":true, \"event\": \"relaunch\"}", "event", event,
                              sizeof(event)) &&
          strcmp(event, "relaunch") == 0);
    /* A registration ack carries no event, and a non-string value is not one. */
    CHECK(!jf_luna_json_string("{\"returnValue\":true}", "event", event, sizeof(event)));
    CHECK(!jf_luna_json_string("{\"event\":42}", "event", event, sizeof(event)));
    /* The key must not be matched inside some other value. */
    CHECK(!jf_luna_json_string("{\"reason\":\"event\"}", "event", event, sizeof(event)));
}

static void test_virtual_list(void)
{
    const loom_rect viewport = {0, 0, 400, 200};
    const loom_virtual_list list = loom_virtual_list_init(viewport, 10000, 40, 1234);
    CHECK(list.first > 0);
    CHECK(list.last - list.first <= 6);
    CHECK(loom_virtual_list_item(&list, list.first).y <= 0);
    /* Revealing an item already on screen must not move the view. */
    CHECK(loom_virtual_list_reveal(&list, list.first + 1, 0) == list.scroll);
}

/* Glyphs are packed with an implicit one-pixel gutter; two regions that touch would bleed
 * into each other under the linear filter the atlas is sampled with. */
static void test_skyline(void)
{
    skyline packer;
    CHECK(skyline_init(&packer, 512));
    skyline_region placed[256];
    size_t count = 0;
    unsigned seed = 42;
    while (count < 256) {
        seed = seed * 1103515245u + 12345u;
        const uint16_t w = (uint16_t)(3 + (seed >> 16) % 58);
        seed = seed * 1103515245u + 12345u;
        const uint16_t h = (uint16_t)(3 + (seed >> 16) % 58);
        skyline_region region;
        if (!skyline_alloc(&packer, w, h, &region))
            break;
        CHECK(region.x >= 1 && region.y >= 1);
        CHECK(region.x + region.w <= packer.size - 1);
        CHECK(region.y + region.h <= packer.size - 1);
        for (size_t i = 0; i < count; i++) {
            const skyline_region other = placed[i];
            const bool overlaps = region.x < other.x + other.w + 1 &&
                                  other.x < region.x + region.w + 1 &&
                                  region.y < other.y + other.h + 1 &&
                                  other.y < region.y + region.h + 1;
            CHECK(!overlaps);
        }
        placed[count++] = region;
    }
    CHECK(count > 100);
    CHECK(!skyline_enlarge(&packer, 123)); /* not a power of two */
    CHECK(!skyline_enlarge(&packer, 256)); /* smaller than it already is */
    CHECK(skyline_enlarge(&packer, 1024) && packer.size == 1024);
    skyline_destroy(&packer);
}

#ifdef JF_HAVE_DEMUX
/* Only what runs without media: the bundled FFmpeg links, and a URL that opens
 * nothing reports failure instead of handing back a half-built context. */
static void test_demux_open_failure(void) {
  CHECK(jf_demux_open("/nonexistent/stream.mkv") == NULL);
  CHECK(jf_demux_open("https://127.0.0.1:1/stream.mkv") == NULL);
  jf_demux_close(NULL);
}
#endif

#ifdef JF_HAVE_SUBS
/* The subtitle path end to end, minus the TV: a script header, one event fed on
 * the segment timeline, and the composited overlay appearing and disappearing
 * with it. This is where an off-by-one in the timing or a wrong premultiply
 * shows up as nothing on screen, which the app itself cannot tell apart from
 * "no subtitles here". */
/* Rendering is on background threads, so a frame arrives a little later. */
static void frame_until(int64_t ms, jf_subs_image *image, bool drawn) {
  for (int tries = 0; tries < 400; tries++) {
    jf_subs_frame(ms, image);
    if ((image->w > 0) == drawn)
      return;
    usleep(5000);
  }
}

static void test_subtitles(void) {
  static const char header[] =
      "[Script Info]\nScriptType: v4.00+\nPlayResX: 1920\nPlayResY: 1080\n\n"
      "[V4+ Styles]\n"
      "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
      "OutlineColour, "
      "BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, ScaleY, "
      "Spacing, Angle, "
      "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, "
      "Encoding\n"
      "Style: "
      "Default,Sans,48,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,0,0,0,0,"
      "100,100,0,0,1,2,0,2,10,10,40,1\n\n"
      "[Events]\n"
      "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
      "Effect, Text\n";
  /* The ASS event format FFmpeg's decoders emit: read order first, then the
   * fields. */
  static const char line[] = "0,0,Default,,0,0,0,,Hello";

  jf_subs_geometry(1920, 1080, 1920, 1080, 0);
  CHECK(jf_subs_open(1, 0, header, (int)sizeof(header) - 1));
  CHECK(jf_subs_ready());
  jf_subs_feed(0, line, (int)sizeof(line) - 1, 1000, 2000);
  /* Read again after a seek back, with the read order a flushed decoder
   * restarts from: still one line. */
  jf_subs_feed(0, "7,0,Default,,0,0,0,,Hello", 25, 1000, 2000);
  /* Another stream's line is not this one's. */
  jf_subs_feed(1, "0,0,Default,,0,0,0,,Stale", 25, 5000, 2000);

  jf_subs_image image;
  jf_subs_frame(500, &image);
  CHECK(image.w == 0); /* before the event */

  frame_until(1500, &image, true);
  CHECK(image.w > 0 && image.h > 0 && image.rgba != NULL);
  CHECK(image.x >= 0 && image.y >= 0);
  CHECK(image.x + image.w <= 1920 && image.y + image.h <= 1080);
  /* Something was actually drawn, and it is not an opaque block: a glyph covers
   * some of its own bounding box and none of the rest. */
  int opaque = 0, clear = 0;
  for (int i = 0; i < image.w * image.h; i++) {
    if (image.rgba[i * 4 + 3] > 200)
      opaque++;
    if (image.rgba[i * 4 + 3] == 0)
      clear++;
  }
  CHECK(opaque > 0 && clear > 0);
  const int drawn_w = image.w;

  frame_until(4000, &image, false);
  CHECK(image.w == 0); /* after it */
  frame_until(5500, &image, false);
  CHECK(image.w == 0); /* the other stream's line never arrived */
  /* Holding still is not a change, even though another thread rendered it. */
  usleep(100000);
  CHECK(!jf_subs_frame(5500, &image));

  /* A seek back lands in the middle of the line, which is still there. */
  frame_until(2500, &image, true);
  CHECK(image.w == drawn_w);

  /* The server's copy, as it converts an SRT: its own short event format, and
   * a line that was never fed. It replaces what the demuxer fed. */
  static const char script[] =
      "[Script Info]\nScriptType: v4.00+\n\n[V4+ Styles]\n"
      "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
      "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, "
      "ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
      "MarginL, MarginR, MarginV, Encoding\n"
      "Style: Default,Sans,48,&H00FFFFFF,&H00FFFFFF,&H00000000,&H00000000,"
      "0,0,0,0,100,100,0,0,1,2,0,2,10,10,40,1\n\n"
      "[Events]\nFormat: Layer, Start, End, Style, Text\n"
      "Dialogue: 0,0:00:08.00,0:00:10.00,Default,Later\n"
      "Dialogue: 0,0:00:01.00,0:00:03.00,Default,Hello there\n"
      /* SRT markup as the server leaves it: empty tags, which draw nothing
       * once they are ASS, and a soft break that is a real one. */
      "Dialogue: 0,0:00:12.00,0:00:14.00,Default,<i></i>Hello <FONT "
      "color=\"#ff0000\"></font>there\n"
      "Dialogue: 0,0:00:16.00,0:00:18.00,Default,Hello\\nthere\n"
      "Dialogue: 0,0:00:20.00,0:00:22.00,Default,Hello\n";
  char *copy = malloc(sizeof(script));
  memcpy(copy, script, sizeof(script));
  CHECK(jf_subs_open_file(1, copy, sizeof(script) - 1));
  frame_until(9000, &image, true);
  CHECK(image.w > 0);
  frame_until(2000, &image, true);
  for (int tries = 0; tries < 400 && image.w == drawn_w; tries++) {
    usleep(5000);
    jf_subs_frame(2000, &image);
  }
  CHECK(image.w > drawn_w); /* "Hello there", not the fed "Hello" */
  const int there_w = image.w, there_h = image.h;
  frame_until(11000, &image, false);
  frame_until(13000, &image, true);
  CHECK(image.w == there_w); /* no tags drawn as text */
  frame_until(15000, &image, false);
  frame_until(17000, &image, true);
  CHECK(image.h > there_h * 3 / 2); /* two lines */
  frame_until(19000, &image, false);
  frame_until(21000, &image, true);
  CHECK(image.w == drawn_w); /* "Hello" alone, as fed */
  /* An older selection's file is not used. */
  copy = malloc(sizeof(script));
  memcpy(copy, script, sizeof(script));
  CHECK(!jf_subs_open_file(0, copy, sizeof(script) - 1));

  /* A 2.4:1 picture letterboxed into the frame keeps its subtitles on the
   * picture: above the bottom bar of (1080 - 800) / 2. */
  jf_subs_geometry(1920, 1080, 1920, 800, 0);
  /* Switching back to the stream finds the line it was fed before, with
   * nothing fed since. */
  CHECK(jf_subs_open(2, 0, header, (int)sizeof(header) - 1));
  frame_until(1500, &image, true);
  CHECK(image.w > 0 && image.y + image.h <= 940);
  /* Kept above the player's controls: the dialogue line rises, a sign pinned
   * with \pos stays on the picture. */
  const int low_y = image.y;
  jf_subs_keep_above(700);
  for (int tries = 0; tries < 400 && image.y == low_y; tries++) {
    usleep(5000);
    jf_subs_frame(1500, &image);
  }
  CHECK(image.w > 0 && image.y + image.h <= 700);
  static const char sign[] = "0,0,Default,,0,0,0,,{\\pos(960,900)}Sign";
  jf_subs_feed(0, sign, (int)sizeof(sign) - 1, 30000, 2000);
  frame_until(29000, &image, false);
  frame_until(31000, &image, true);
  /* 900 of 1080 on the 800-row picture at 140: its bottom near 806. */
  CHECK(image.w > 0 && image.y + image.h > 780);
  jf_subs_keep_above(INT_MAX);
  /* A switch mid-item draws nothing from the item's start in the meantime -
   * a sign at 0:00 flashed up until the real frame was rendered. */
  static const char opening[] = "0,0,Default,,0,0,0,,Opening";
  jf_subs_feed(0, opening, (int)sizeof(opening) - 1, 0, 2000);
  CHECK(jf_subs_open(4, 0, header, (int)sizeof(header) - 1));
  usleep(100000); /* long enough for a worker to have rendered frame 0 */
  bool flashed = false;
  for (int tries = 0; tries < 60; tries++) {
    jf_subs_frame(20000, &image);
    flashed |= image.w > 0;
    usleep(5000);
  }
  CHECK(!flashed);
  jf_subs_close(5);
  CHECK(!jf_subs_ready());
  jf_subs_release();
}
#endif

static void test_cfg_round_trip(void) {
  jf_arena arena = {0};
  cfg_writer writer;
  cfg_writer_init(&writer, &arena);
  cfg_comment(&writer, "written by the tests");
  cfg_write_text(&writer, "top", "before any section");
  cfg_section(&writer, "ui");
  cfg_write_bool(&writer, "animations", false);
  cfg_write_int(&writer, "scale", -42);
  cfg_write_float(&writer, "gamma", 2.2f);
  cfg_comment(&writer, "a comment between values");
  cfg_section(&writer, "server");
  cfg_write_text(&writer, "url", "http://host:8096/?a=b");
  cfg_write_text(&writer, "empty", "");
  CHECK(writer.ok);
  CHECK(strcmp(writer.text, "# written by the tests\n"
                            "top=before any section\n"
                            "\n[ui]\n"
                            "animations=false\n"
                            "scale=-42\n"
                            "gamma=2.20000005\n"
                            "# a comment between values\n"
                            "\n[server]\n"
                            "url=http://host:8096/?a=b\n"
                            "empty=\n") == 0);

  const char *path = "cfg-test.ini";
  CHECK(cfg_flush(&writer, path));
  cfg_reader reader;
  CHECK(cfg_open(&reader, &arena, path));
  remove(path);

  CHECK(cfg_next(&reader) == CFG_VALUE);
  CHECK(strcmp(reader.section, "") == 0 && strcmp(reader.key, "top") == 0);
  CHECK(strcmp(cfg_text(&reader), "before any section") == 0);
  CHECK(cfg_next(&reader) == CFG_SECTION && strcmp(reader.section, "ui") == 0);
  CHECK(cfg_next(&reader) == CFG_VALUE &&
        strcmp(reader.key, "animations") == 0);
  CHECK(cfg_bool(&reader, true) == false);
  CHECK(cfg_next(&reader) == CFG_VALUE && strcmp(reader.key, "scale") == 0);
  CHECK(cfg_int(&reader, 0) == -42);
  CHECK(cfg_next(&reader) == CFG_VALUE && strcmp(reader.key, "gamma") == 0);
  CHECK(cfg_float(&reader, 0) == 2.2f);
  CHECK(cfg_int(&reader, 7) == 7);
  CHECK(cfg_next(&reader) == CFG_SECTION &&
        strcmp(reader.section, "server") == 0);
  CHECK(cfg_next(&reader) == CFG_VALUE && strcmp(reader.key, "url") == 0);
  CHECK(strcmp(cfg_text(&reader), "http://host:8096/?a=b") == 0);
  CHECK(cfg_next(&reader) == CFG_VALUE && strcmp(reader.key, "empty") == 0);
  CHECK(strcmp(cfg_text(&reader), "") == 0);
  CHECK(cfg_int(&reader, 3) == 3 && cfg_bool(&reader, true) == true);
  CHECK(cfg_next(&reader) == CFG_END);
  CHECK(cfg_next(&reader) == CFG_END);

  CHECK(cfg_open(&reader, &arena, "cfg-test-missing.ini"));
  CHECK(cfg_next(&reader) == CFG_END);
  jf_arena_destroy(&arena);
}

static void test_cfg_reader_tolerance(void) {
  char text[] = "\r\n  ; comment\r\n# another\r\n[ ui ]\r\n"
                "  animations =  On \r\nno equals sign\r\n[broken\r\n"
                "flag=maybe\r\ncount = 12x\r\n\r\nlast=1";
  cfg_reader reader;
  cfg_init(&reader, text);
  CHECK(cfg_next(&reader) == CFG_SECTION && strcmp(reader.section, "ui") == 0);
  CHECK(cfg_next(&reader) == CFG_VALUE &&
        strcmp(reader.key, "animations") == 0);
  CHECK(strcmp(cfg_text(&reader), "On") == 0 &&
        cfg_bool(&reader, false) == true);
  CHECK(cfg_next(&reader) == CFG_VALUE && strcmp(reader.key, "flag") == 0);
  /* "[broken" did not start a section */
  CHECK(strcmp(reader.section, "ui") == 0);
  CHECK(cfg_bool(&reader, true) == true && cfg_bool(&reader, false) == false);
  CHECK(cfg_next(&reader) == CFG_VALUE && strcmp(reader.key, "count") == 0);
  CHECK(cfg_int(&reader, -1) == -1);
  CHECK(cfg_next(&reader) == CFG_VALUE && strcmp(reader.key, "last") == 0);
  CHECK(cfg_int(&reader, 0) == 1 && cfg_bool(&reader, false) == true);
  CHECK(cfg_next(&reader) == CFG_END);
}

static void test_credentials(void) {
  setenv("JELLYFIN_STORE", "store-test", 1);
  jf_store_init();
  const jf_credentials saved = {"http://host:8096", "0123abcd", "user-id",
                                "Some User", "p=ss word;#x"};
  jf_store_save(&saved);
  jf_credentials loaded;
  CHECK(jf_store_load(&loaded));
  CHECK(strcmp(loaded.server, saved.server) == 0);
  CHECK(strcmp(loaded.token, saved.token) == 0);
  CHECK(strcmp(loaded.user_id, saved.user_id) == 0);
  CHECK(strcmp(loaded.user_name, saved.user_name) == 0);
  CHECK(strcmp(loaded.password, saved.password) == 0);
  jf_store_forget();
  CHECK(!jf_store_load(&loaded));
  rmdir("store-test/conf");
  rmdir("store-test/cache");
  rmdir("store-test");
}

static void test_blurhash(void) {
  /* 1x1 components with a pure red DC term: every pixel is that colour. */
  uint8_t rgb[4 * 3 * 3];
  CHECK(jf_blurhash_decode("00TI:j", 4, 3, rgb));
  for (size_t i = 0; i < 12; i++)
    CHECK(rgb[i * 3] == 255 && rgb[i * 3 + 1] == 0 && rgb[i * 3 + 2] == 0);
  /* The reference hash: 4x3 components, a varied image. */
  CHECK(jf_blurhash_decode("LEHV6nWB2yk8pyo0adR*.7kCMdnj", 4, 3, rgb));
  CHECK(memcmp(rgb, rgb + 3, 3) != 0 || memcmp(rgb, rgb + 33, 3) != 0);
  memset(rgb, 7, sizeof(rgb));
  CHECK(!jf_blurhash_decode("LEHV6nWB2yk8pyo0adR*.7kCMdn", 4, 3, rgb));
  CHECK(!jf_blurhash_decode("00TI:\"", 4, 3, rgb));
  CHECK(!jf_blurhash_decode("", 4, 3, rgb));
  CHECK(rgb[0] == 7);
}

int main(void)
{
  test_cfg_round_trip();
  test_blurhash();
  test_cfg_reader_tolerance();
  test_credentials();
  test_load_payload();
  test_feed_and_control_payloads();
  test_audio_placement();
  test_clock();
  test_luna_event();
  test_virtual_list();
  test_skyline();
#ifdef JF_HAVE_DEMUX
    test_demux_open_failure();
#endif
#ifdef JF_HAVE_SUBS
    test_subtitles();
#endif
    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
