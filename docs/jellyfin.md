# Jellyfin client

`src/app/main.c` is a real client for a Jellyfin server: UDP discovery,
sign-in (password or Quick Connect), a stored token, home rows, a virtual
library grid, and the path down to a single episode. It reuses `uidemo`'s
renderer unchanged — one instanced batch, rasterised glyphs, the same remote
and pointer handling.

Verified against **Jellyfin 10.11.8**, and on the TV at 1920x1080.

## Shape

```
src/app/main.c            screens, navigation, poster cache
src/jf/api.c              endpoints, JSON, credentials, the worker pool
src/jf/image.c            PNG decode through the TV's libpng
src/jf/store.c            where files go on disk, and the artwork cache
```

Requests never touch the render thread. `api.Fetcher` owns a fixed pool of 32
task slots and four worker threads. `submit` reserves a slot; `start` publishes
its filled-in inputs through a `std.Io.Queue`. Workers fetch and decode artwork
(including disk-cache hits), then post a coalesced, empty SDL event. The UI
drains completed tasks, uploads decoded pixels to GL, copies screen data and
hands slots back. A task's results live in that task's arena, so "copy what you need" is
the rule that keeps a draw command from holding a pointer into memory a worker
is about to reuse.

The main loop waits in SDL when idle. Input, window exposure/resizing and
changed results request frames; Quick Connect polling and playback-control
expiry provide the only normal UI deadlines. Debug scripts and captures
explicitly request their own frames. A minimized window continues processing
results without drawing. Desktop mpv signals new video frames through its render
callback; those frames reuse the retained UI command list for compositing.
Starfish presents video on a separate plane, so TV playback does not schedule
graphics frames once the controls are hidden.

Starfish initialization, demuxing, audio decoding and feeding run on background
threads. Bounded `std.Io.Queue` packet queues keep both the packet-count and
8 MiB read-ahead limits without polling. Stop and seek close the queues and wake
the pacing event before threads are joined. Thread priorities remain at their
defaults.

## Subtitles

Text subtitle tracks are rendered with libass (`src/jf/subs.c`) onto the
graphics plane. Video belongs to the TV's own plane, so nothing can be
composited into the picture; the overlay is drawn over the transparent hole
instead, at the window's size rather than the video's.

Every text format FFmpeg knows — ASS/SSA, SRT, WebVTT, mov_text — decodes to ASS
dialogue lines, which is exactly what `ass_process_chunk` takes, so
`jf_demux_subtitle_decode` has one path and the build needs only those few
decoders. Bitmap tracks (PGS, VobSub) have no ASS form and are not offered.

**The timing is the pipeline's.** Events are fed on the same rebased segment
timeline the video feed and the ALSA writer are paced against, and the frame
asked for is `jf_clock_pts()` — the projected presentation timestamp, not the
wall clock. Subtitles therefore track the picture and the sound through pauses,
buffering and seeks, and a seek flushes the track rather than re-deriving an
offset.

The demux thread feeds; the render thread composes. One lock covers the library,
the renderer and the track, which is a few short calls a minute against a few a
second. libass returns one 8-bit coverage bitmap per colour run; those are
blended into a single RGBA image bounded by their union — a strip near the
bottom, not a frame — so an overlay costs one upload and one draw call. It is
re-uploaded only when `ass_render_frame` reports a change.

The playback screen otherwise schedules no frames once the controls hide, so
with a track selected the main loop ticks at 33 ms to ask whether the overlay
moved. libass can say when the next event *starts* but not when the current one
*ends*, so there is no single deadline to wait on.

The first text track is selected when playback starts, and the Subtitles button
cycles through the rest and back to off. The reader thread owns the demuxer, so
a selection made on the render thread is a request it picks up between packets.
`JF_NOSUBS=1` keeps the reader off that path entirely.

Subtitle packets are not decoded before the segment origin is known. It is set
by the first video or audio packet, and a line rebased against a zero origin
lands at its absolute container time - half an hour out, on a resume.

Two bugs that cost time and are easy to reintroduce:

- A draw command holds a **slice**, not a string. Passing a card *by value* to a
  draw function makes its text point into that function's stack frame, which is
  gone by the time the renderer runs. Cards are passed by pointer for that
  reason, and the pointer must be to storage that outlives the frame.
- `fetcher.pending()` counts tasks that are **finished but not yet consumed**,
  not just in-flight ones. "Nothing in flight" is not "the screen is up to
  date": there is a frame between a worker finishing and the UI draining it.
- The subtitle canvas deliberately outlives the track. The UI uploads
  `jf_subs_image.rgba` after the lock is gone, so freeing it from the demux
  thread on a track change pulls the buffer out from under a texture upload.

## Navigation is a stack, not a rule

Back pops a stack of entries (`stack_entry` in `app/main.c`). The rule-based version
— "from details, go back to the grid if a grid is loaded" — is wrong in the
ordinary case: reaching a show from a *home row* and pressing Back returned to
whichever library grid happened to still be loaded, so
`home → shows → korra → back` landed in `shows`.

An entry stores enough to **re-enter** a screen, not a snapshot of it.
Re-entering refetches, which is also what keeps a details screen current.

Back belongs to webOS only at the server picker and signed-in home. Other
screens, and an open text editor, claim it through the shared Wayland layer.
Home rows and episode lists have persistent wheel scroll positions; remote
navigation reveals the focused row without overriding wheel scrolling each
frame. Returning home restores its scroll position.

Show and season pages share the series' first backdrop as a full-screen
background. Season cards and the season's left-hand poster use the season's
own Primary image, falling back to the show poster if absent. Series metadata
shows its year span (or an open span for continuing shows), without type or
season-count labels. Episode rows use their own landscape Primary image and
show episode number/title, humanized duration, and `★ rating` when available.
The star is yellow. Blue poster badges count unfinished seasons on shows and
unwatched episodes on seasons; watched episodes show white check marks in blue
badges. Zero counts, counts still loading, and unwatched episode badges are hidden.
The series count is fetched from season watch states, since Jellyfin's
series-level `UnplayedItemCount` counts episodes, not seasons. Opening a show
or season focuses its first unfinished child (first child if all are watched),
while Back restores the previous selection. Overview `<br>` tags become real
line breaks and unknown movie years are omitted.
Scroll content reaches the screen edges; selection padding is not a clip.
Grid Up/Down preserve the selected item when no item exists in that column in
the adjacent row. The texture cache distinguishes image type and requested size;
backdrops also have a separate disk-cache key.

## Storage

Two roots, chosen without any environment, because SAM provides none:

| | |
|---|---|
| installed | the app's own directory, `<appdir>/conf/` and `<appdir>/cache/` |
| development | `/tmp/jellyfin-native/{conf,cache}` |
| override | `$JELLYFIN_STORE` |

An installed app runs with its own directory as the working directory, so
`/proc/self/cwd` answers both "where do I write" and "am I installed" -- the
same trick `ndlplay` uses to find its app id. The marker is the path containing
`/usr/palm/applications/`, since a developer-mode install lives under
`/media/developer/apps` and a retail one under `/media/cryptofs/apps`.

Writing into the app's own directory is what the native apps on this TV do:

```
com.limelight.webos/conf/{moonlight.ini,hosts.ini,key/key.pem}
com.limelight.webos/cache/<uuid>_<id>          (box art)
org.mariotaku.ihsplay/.cache/fontconfig/...
```

### The permission that makes it work

**An installed app cannot write to its own directory unless the package ships
it world-writable.** The app runs as a jail uid (6350 here) that owns none of
its files, which are installed owned by uid 1000. Both reference apps ship
`777`; a package built with a default `mkdir` gets `755` and every write fails
silently.

`WRITABLE_DIRS` in `cmake/WebOSPackage.cmake` therefore ships `conf/` and
`cache/` at `777`. The
proof that this is the real mechanism is the ownership of what gets written:

```
drwxrwxrwx 1000:1000  cache/
-rw-r--r-- 6350:5000  cache/<item>-<tag>-240x360.img     <- written by the jail uid
```

`store.init` also probes writability and falls back to `/tmp` with a log line
rather than failing every write silently, which is what an older package
installed before this change would otherwise do.

Because SAM gives no terminal either, stderr is redirected to
`<root>/conf/jellyfin.log` (`/tmp/jellyfin.log` if even that is not writable).
Without it an installed app is undebuggable.

## Credentials

Five lines -- server, token, user id, user name, password -- written `0600`.

**The password is stored, not just the token.** Jellyfin invalidates a device's
previous token whenever that device signs in again, so a token-only store goes
stale on its own and strands the user at a sign-in screen with a remote in
their hand. On a 401 the client re-authenticates in the background, saves the
new token and reloads the screen that failed; the user sees a status line, not
a login form. Two consecutive failures stop the retry and sign out properly.

That does mean a plaintext password on disk in a world-writable directory. The
file mode is what protects it, and root on this TV can read it regardless.
Quick Connect stores no password and simply signs out when its token dies.

Only a **401/403** triggers any of this. An earlier version signed out on any
non-200, so a restarting server cost you a password typed on a remote.

A file written by a root development run over SSH is `0600 root`, which the
jailed app cannot read. That is a development artefact, not a bug: the app
writes it as its own uid in normal use.

## Artwork cache

Keyed `<item>-<tag>-<width>x<height>.img` under `cache/`, holding the encoded
PNG as received.

The tag is Jellyfin's own image tag, which is **a hash of the image content**,
so a changed image is a different filename and a stale one can never be served.
That is what makes the cache need no revalidation request at all: a hit costs
no network. Episodes carry `SeriesPrimaryImageTag`, which is exactly the tag
for the series poster the episode falls back to.

What the server actually offers, measured:

| | |
|---|---|
| `ETag` on images | **absent**; `If-None-Match` is ignored and returns 200 with the full body |
| `If-Modified-Since` | **honoured**, returns 304 with 0 bytes |
| `tag=` in the URL | accepted but *not* validated -- a wrong tag still serves the image |

So `tag=` is purely a cache-busting URL component, which is how Jellyfin's own
clients use it, and this client sends it for the same reason: it makes the URL
change when the image does. An image with no tag is simply not cached;
`If-Modified-Since` is the documented fallback if that ever needs to change.

Writes go through a temporary plus rename, because four workers share the
directory and a half-written file must never be readable as a whole one. The
cache is swept back under 48 MB at startup, oldest first -- a disposable cache
does not justify an index to keep consistent across four writer threads.

In memory, on top of that, sits an LRU of 48 GL textures keyed by item **and**
tag, so re-tagged artwork is not served from the old texture for the rest of a
session.

## Artwork: why `libpng`, and why not the JPEG

Both libraries are on the TV:

```
/usr/lib/libjpeg.so.62   -> libjpeg.so.62.3.0
/usr/lib/libpng16.so.16  -> libpng16.so.16.39.0
/usr/lib/libwebp.so.7    /usr/lib/liblxjpeg.so.2    /lib/libz.so.1
```

The client links `libpng16` and asks Jellyfin for `format=Png`. **JPEG was rejected on ABI grounds, not
preference:** the TV ships `libjpeg.so.62` and a current development machine
ships `libjpeg.so.8`. Those are different ABIs behind the same name, and
libjpeg's entry points take a `struct jpeg_decompress_struct` whose layout *is*
the ABI — so a `dlopen`'d JPEG path needs one transcribed struct per version
and is silently wrong on whichever machine it was not written for.
`libpng16.so.16` is on both (1.6.39 on the TV, 1.6.58 here).

Within libpng, this uses the **simplified API** (`png_image_begin_read_from_memory`
/ `png_image_finish_read`), which is why `src/jf/image.c` is about forty lines:
errors come back as a zero return and a message in the struct, where the classic
`png_create_read_struct` path signals them by longjmp'ing out of an error
callback — and a callback that returns instead makes libpng call `abort()`.

### `fillWidth`/`fillHeight` is a hint, not a contract

The same request comes back at different sizes depending on the source art:

```
fillWidth=200&fillHeight=300  ->  200x300, 204x300, 212x300, 200x301, 534x300
```

That killed the first design, which packed posters into one atlas of fixed
tiles: an image wider than its tile overwrites its neighbours. **Posters get one
texture each.** The cost is a draw call per distinct texture on screen, which is
what the batcher already does for a binding change; the alternative is cropping
every image to a tile and re-uploading over tiles a scrolling grid may still be
drawing from. The cache is 48 textures, evicted least-recently-drawn, and
`coverUv` crops the long axis so a 534x300 library banner and a 200x300 poster
look right in the same card.

## Playback: Starfish for video, ALSA for audio

The details and episode screens resolve

```
{server}/Videos/{id}/stream?static=true&api_key={token}
```

FFmpeg demuxes it in-process. Video access units go to LG's Starfish pipeline
(`libplayerAPIs`) as a raw elementary stream and are decoded and presented on the
TV's own video plane, which this process never touches. Audio is decoded to
48 kHz interleaved stereo S16 and written to ALSA.

```
src/jf/demux.c        FFmpeg: containers in, Annex-B video and PCM audio out
src/jf/player.c       the three threads, the segment loop, seek and pause
src/jf/smp.h          the C API of the pipeline
src/jf/smp_shim.cpp   the only C++: try/catch around StarfishMediaAPIs
src/jf/smp_payload.c  the JSON the pipeline actually consumes
src/jf/smp_segment.c  the libpf entry points StarfishMediaAPIs does not forward
src/jf/clock.c        where playback is, from the pipeline's own events
src/jf/audio_alsa.c   the PCM sink, and the whole of A/V sync
```

Three threads move one segment of playback: a reader that demuxes ahead and
decodes audio into two bounded queues, a video thread that feeds Starfish, and
an audio thread that writes to ALSA. A seek closes the queues, parks all three,
re-anchors the pipeline and starts a new segment without unloading it.

### Audio does not go through the pipeline

Starfish will take PCM, and this client used to hand it PCM. Nearly everything
that path needed was compensation for a sink that would not say where it was:

- the sound had to be cut into 1024-sample access units;
- each one needed a timestamp from a sample cursor maintained here, because the
  sink plays what it is given back to back — so a gap between two chunks is not
  heard as a gap, it makes everything after it play early against the picture,
  permanently;
- every hole therefore had to be filled with silence rather than closed up;
- the sink only built at a short list of sample rates, and an unlisted rate was
  taken as "bypass" rather than refused — 48 kHz PCM fed into a sink that came
  up at 44.1 kHz plays about 9% slow;
- `Play` had to wait on a PCM preroll before the first video frame existed.

ALSA answers the question that machinery existed to work around:
`snd_pcm_delay` reports exactly how many frames are still queued, so the
presentation time of the next sample written is *known*. What is left is one
rule, in `jf_audio_placement`:

> Video is the master clock. For each chunk, compare the host time it is due
> against the host time the queue will reach — pad silence if it would arrive
> early, drop frames from its head if it is already late, and write it
> unchanged when the error is under 20 ms.

The same correction serves drift, a seek and an underrun, because after a flush
or an `EPIPE` the queue is simply empty and "when will the queue reach this
point" answers *now*. The device is `default`, overridable with `JF_ALSA_DEV`.

The load payload therefore declares `needAudio:false` and
`useCurrentTimeWithSystemClock:true`: with no audio track in the pipeline,
nothing else establishes a running clock for the video sink.

### Where playback is: `queryPosition`, not `getCurrentPlaytime`

`option.queryPosition` makes Starfish report the presentation timestamp of each
displayed frame as the numeric value of a `FRAMEREADY` event. That is the whole
clock — a pts paired with the host time the event arrived, sampled at the moment
of the flip.

The alternative, polling `getCurrentPlaytime()`, is frame-quantized and
sometimes slow, and reading it usefully needed a sampling period, a slow-query
rejection, a half-interval bracket to guess when the frame had *really* flipped,
and a stability probe before the result could be trusted. An event that arrives
at the flip needs none of that; `src/jf/clock.c` is forty lines.

### The traps that cost time

- **`Load` returning true only means the request was accepted.** Feeding and
  `Play` belong after the asynchronous `LOADCOMPLETED` event.
- **The load payload describes the timeline but does not activate it.** libpf
  does nothing until `CustomPipeline` receives a segment event, and
  `StarfishMediaAPIs` does not forward `sendSegmentEvent`. `smp_segment.c`
  reaches it by mangled name through `libpf-1.0`, following the public `player`
  member of the instance. `setTimeToDecode`, the public wrapper, rejects the
  `LOADED` state.
- **`flush()` with no argument sends no `FLUSH_START`/`FLUSH_STOP` pair** and
  leaves the sink on the pre-seek segment. `flush(const char *)` parses exactly
  two keys, `audioFlush` and `offset`, and those reach the pipeline.
- **A `BufferFull` answer from `Feed` keeps nothing**, so the same buffer has to
  be offered again.
- **A live transcode has no byte ranges.** `av_seek_frame` reports success on
  one anyway and positions FFmpeg at byte zero, so seeking a transcode means
  asking Jellyfin for a new stream at `StartTimeTicks` instead.
- **A negative pts is read by libpf as an enormous unsigned one.** The reader
  sets the segment's time origin, because it sees packets in container order and
  two feed threads racing for it would send the loser negative.

## What the library contains, and what the TV decodes

Sampled from 600 of 3026 items, movies and episodes:

| | |
|---|---|
| containers | `mkv` 98%, `mp4`, `mpeg` |
| video | `hevc` 339, `h264` 154, `av1` 103, `mpeg2video` 4 |
| audio | `aac` 292, `eac3` 249, `flac` 168, `opus` 141, `ac3` 103, `dts` 32, `truehd` 14, `mp2` 4 |
| channels | 2ch 621, 6ch 361, 8ch 15 |

Audio codecs stopped mattering once decoding moved into FFmpeg: everything above
decodes in software and is downmixed to stereo by `swresample` before it reaches
ALSA. Video still has to suit the hardware decoder — full table in
[codecs.md](codecs.md) — so a source above 8 bits per sample, or H.264 above
High profile, is swapped for the server's transcode before anything is read.

## Testing without a remote in your hand

There is no way to click through this app headlessly, so it replays one:

```sh
set -a; . ./.env; set +a
UI_SCRIPT=oddo UI_CAPTURE=/tmp/home.ppm ./build/src/jellyfin
```

`UI_SCRIPT` letters are `u`/`d`/`l`/`r` for the arrows, `o` for OK, `b` for
Back, `s` for the subtitle key, `[`/`]` to scroll up/down, and `.` to wait a beat. Back uses the native
LG keycode on the TV. A press is held until nothing is outstanding in the
fetcher, so a script does not race a request. `UI_CAPTURE` saves the screen it
ends on — this application's own OpenGL backbuffer, the same path as `uidemo`'s
F12 — and exits.

The same works on the TV, which is how the grid above was confirmed on device:

```sh
ssh $T "cd /tmp && XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-0 \
  JELLYFIN_USER=... JELLYFIN_PASSWORD=... UI_SCRIPT=oddoddrro \
  UI_CAPTURE=/tmp/jf.ppm /tmp/jellyfin"
```

`JELLYFIN_ADDRESS`, `JELLYFIN_USER` and `JELLYFIN_PASSWORD` prefill the sign-in
fields when no token is stored; they are ignored once one is.

To exercise the *installed* path, run the installed binary from its own
directory -- that is what makes `/proc/self/cwd` say "installed":

```sh
ssh $T "cd /media/developer/apps/usr/palm/applications/dev.hookedbehemoth.jellyfin \
  && XDG_RUNTIME_DIR=/tmp/xdg WAYLAND_DISPLAY=wayland-0 ./jellyfin"
```

With no tty its output goes to `conf/jellyfin.log`, not the terminal.

The endpoint layer has its own live test, skipped unless the environment names
a server:

```sh
cmake --preset host && ctest --preset host
```

`ctest --preset host` also runs the Back ownership/navigation, artwork metadata,
home scrolling and shared virtual-list regression tests on the host.

## Credentials

Four lines — server, token, user id, user name — written `0600` to
`$JELLYFIN_STORE`, else `$HOME/.jellyfin-native`. The **password is never
stored**; the access token is. The device id is derived from the hostname so the
server's device list does not grow an entry per launch and Quick Connect
approvals stick.

Only a **401/403** clears them. An earlier version signed out on any non-200,
which meant a restarting server or a dropped Wi-Fi association cost you a
password typed back in on a TV remote.
