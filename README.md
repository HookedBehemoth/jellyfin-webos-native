# webos-native

Native C applications for LG webOS TVs. The project contains three programs:

- `jellyfin` — a native Jellyfin client.
- `xmb` — a full-screen shader and a frame-time overlay.
- `gltri` — 3000 instanced triangles and the same overlay.

All three use the same SDL2 platform layer for webOS windowing, input and OpenGL
ES context creation. Jellyfin links a deliberately small target FFmpeg build for
container demuxing and audio decode, and packages those shared libraries with
the app; everything else it needs is already on the TV.

One translation unit is C++: `src/jf/smp_shim.cpp`, a try/catch façade over
`StarfishMediaAPIs`. Nothing else in the program touches C++.

## Requirements

- The [openlgtv buildroot NDK](https://github.com/openlgtv/buildroot-nc4) at
  `/opt/arm-webos-linux-gnueabi_sdk-buildroot`, or anywhere with `WEBOS_SDK`
  pointing at it
- CMake 3.21+ and Ninja
- `ares-package` / `ares-install` from the webOS CLI, for packaging and
  installing
- `slangc` to compile the embedded OpenGL ES shaders; `glslangValidator` is
  optional extra validation
- FFmpeg, from `tools/build-ffmpeg.sh` — a build with no video decoders, no
  encoders, no muxers, no filters and only file/http/https, which is all the
  client needs when the server transcodes. It builds mbedTLS alongside it, since
  no firmware exports a TLS library and https has to come from somewhere; both
  ship inside the ipk. Required for the TV; on a desktop
  `tools/build-ffmpeg.sh --host` is optional and only the demux tests want it.
  Both install where `FFMPEG_ROOT` defaults to; override it with
  `-DFFMPEG_ROOT=/path`.
- libass, from `tools/build-libass.sh` — the subtitle renderer, with the FriBidi
  and HarfBuzz it will not build without. FreeType is the TV's own; fontconfig is
  deliberately left out, so ASS styles resolve to the same face the UI uses.
  All three ship inside the ipk. Required for both targets;
  `tools/build-libass.sh --host` also enables the subtitle test. They install
  where `LIBASS_ROOT` defaults to; override it with `-DLIBASS_ROOT=/path`.

## Build

```sh
cmake --preset webos          # WEBOS_SDK=/path/to/ndk to move the NDK
cmake --build build
```

Every program gets three targets:

```sh
cmake --build build --target jellyfin-ipk      # -> build/dist/*.ipk
cmake --build build --target jellyfin-install  # ares-install; honours ARES_DEVICE
cmake --build build --target jellyfin-verify   # webosbrew-ipk-verify
```

`-verify` reports, per firmware release, any symbol the ipk needs that the TV
does not export — which is the objective answer to the glibc- and
libstdc++-version questions.

## Running locally

Everything except the player builds and runs on a desktop:

```sh
cmake --preset host
cmake --build build-host
./build-host/src/jellyfin    # Escape or Back closes it
./build-host/src/xmb
./build-host/src/gltri
```

`xmb` and `gltri` each read their own frame back as coarse ASCII, plus a count
of the pure white pixels the overlay is the only source of — a headless check
that the scene, the text program and the blend all work:

```sh
XMB_DUMP=1 ./build-host/src/xmb
GLTRI_DUMP=1 ./build-host/src/gltri
```

### Playback is the one thing that does not run here

Video on the TV belongs to `libplayerAPIs` and `libpf`, which exist nowhere
else, so a desktop build gets `jf/player_null.c` instead: discovery, sign-in,
the home rows, the library grid, artwork, navigation and the
`UI_SCRIPT`/`UI_CAPTURE` loop all work, and pressing Play refuses and logs the
stream URL — which can be pasted straight into a player by hand.

The desktop backend is to be **libmpv**, as the Zig version had, rendering into
this process's own GL context behind the same `jf_player_*` interface Starfish
sits behind. **Audio stays ALSA there too**: the sink in `jf/audio_alsa.c` and
the sync rule in `jf/audio_sync.h` are not per-platform, and having one audio
path on both is worth more than letting mpv own its own.

Open before writing it: whether mpv demuxes the stream itself (and is simply
pointed at ALSA) or is used as a video decoder alone, with our existing demuxer
and ALSA sink keeping the audio — the second keeps one audio path but means two
readers of the same URL.

### Driving it without a remote

`UI_SCRIPT` replays remote presses a few frames apart and `UI_CAPTURE` saves the
screen it ends on as a PPM. Letters are the four arrows, `o` for OK, `b` for
Back, `.` waits one more beat:

```sh
set -a; . ./.env; set +a
UI_SCRIPT=oddo UI_CAPTURE=/tmp/home.ppm ./build-host/src/jellyfin
```

`JELLYFIN_ADDRESS` / `JELLYFIN_USER` / `JELLYFIN_PASSWORD` prefill the sign-in
fields, which is what lets a script get past them.

## Test

The backend-agnostic code — the Starfish payloads, the A/V sync arithmetic, the
clock projection, the atlas packer and the virtual-list geometry — runs here
too:

```sh
ctest --preset host
```

The host preset enables UBSan and builds C and C++ with `-Wall -Wextra -Werror`.
Enable the versioned pre-commit hook once per checkout to check staged C/C++ changes with
`clang-format`:

```sh
git config core.hooksPath .githooks
```

The hook prints the focused formatting diff and stops the commit. Apply it with
`git clang-format --staged`, review the result, then stage it again.

A machine with no SDL2/EGL/GLES still configures; it just builds the tests and
skips the two probes.

## Windows

There is a third preset for debugging the UI on Windows. It builds the same
desktop target — the player is still `jf/player_null.c`, so it does not play
anything — with MSYS2's mingw-w64 toolchain:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,SDL2,freetype,curl,libpng,json-c,libass,angleproject}
cmake --preset windows
cmake --build build-windows
```

`slangc` has to be on `PATH` for the shader step, and so does `sh`, which MSYS2
provides. GLES comes from ANGLE rather than a system library, which is why the
context asked for there is ES 3.1: ANGLE returns exactly the version requested,
and the UI shaders need 3.10 for their storage buffer blocks. Running the
resulting `.exe` needs `C:\msys64\ucrt64\bin` on `PATH` for the DLLs.

## Settings

`conf/preferences.ini` holds every setting. The app writes the whole file, with
its defaults, the first time a setting changes in the UI:

```ini
[ui]
animations=true
; frame times and instance counts in the corner, redrawing every frame
overlay=false
; a .ttf to draw the UI with; empty picks a system face
font=

[playback]
; false plays video only
audio=true
; false keeps the reader off the subtitle path entirely
subtitles=true
audio_device=default

[log]
; every key SDL reports
keys=false
; every SDL window event with the sizes reported at it
window=false
; every Luna lifecycle payload
luna=false
```

Host debugging only, from the environment:

| variable | effect |
|---|---|
| `UI_SCRIPT` / `UI_CAPTURE` | replay remote presses, then save a PPM |
| `JELLYFIN_ADDRESS` / `JELLYFIN_USER` / `JELLYFIN_PASSWORD` | prefill sign-in when nothing is stored |
| `JELLYFIN_STORE` | where `conf/` and `cache/` live |
| `XMB_DUMP`, `GLTRI_DUMP` | the xmb and gltri probes |

An installed app has no terminal, so it redirects stdout and stderr to
`conf/jellyfin.log`.
