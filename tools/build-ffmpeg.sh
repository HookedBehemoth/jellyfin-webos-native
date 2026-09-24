#!/usr/bin/env bash
# Builds the small FFmpeg the Jellyfin client links: container demux, audio decode to
# PCM, and the two Annex B bitstream filters. Nothing else.
#
#   tools/build-ffmpeg.sh          # cross build, installs into the NDK sysroot
#   tools/build-ffmpeg.sh --host   # native build into build/ffmpeg-host, for the tests
#
# The TV's own decoder takes the video, and the server transcodes anything it will not,
# so this build has no video decoders, no encoders, no muxers, no filters and no
# scaler. The subtitle decoders are the exception to "no decoders": every text format
# FFmpeg knows decodes to ASS dialogue lines, which is the one thing libass takes, so
# those few are what SRT, WebVTT and mov_text support costs. Bitmap subtitles - PGS,
# VobSub, DVB - decode to paletted pictures that jf/picsubs.c places over the video.
# Seeking reopens the stream, so there is no HLS and no segment handling: file,
# http and https are the whole protocol list. The video parsers stay because
# jf_demux_video_unsupported reads bit depth and profile, which containers do not carry
# and only the parser fills in.
#
# zlib is the TV's own. A TLS library is not - webos-ipk-verify says no firmware exports
# one - so it ships in the same ipk, and that makes its size ours to pay. mbedTLS is the
# TLS client and nothing else, where OpenSSL is a crypto toolkit that happens to contain
# one: 0.6 MB against 4.7 MB for the same https. Everything here is C: the shipped .so
# files must not pull in libstdc++.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${FFMPEG_VERSION:-9.0.1}"
SHA256="${FFMPEG_SHA256:-cf38e0e28c7e5605942c4a77755349b0145804a397af37eb1fb4c77cb237f635}"
URL="${FFMPEG_URL:-https://ffmpeg.org/releases/ffmpeg-$VERSION.tar.xz}"
ARCHIVE="$ROOT/build/downloads/ffmpeg-$VERSION.tar.xz"
SRC_DIR="$ROOT/build/ffmpeg-src"
# mbedTLS 4 is a different API that FFmpeg does not take yet; 3.6 is the LTS line.
SSL_VERSION="${MBEDTLS_VERSION:-3.6.7}"
SSL_SHA256="${MBEDTLS_SHA256:-a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6}"
SSL_URL="${MBEDTLS_URL:-https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$SSL_VERSION/mbedtls-$SSL_VERSION.tar.bz2}"
SSL_ARCHIVE="$ROOT/build/downloads/mbedtls-$SSL_VERSION.tar.bz2"
SSL_SRC_DIR="$ROOT/build/mbedtls-src"

fetch() { # url sha256 destination
    echo "$2  $3" | sha256sum -c --status 2>/dev/null && return 0
    curl -fL --retry 3 -o "$3" "$1"
    echo "$2  $3" | sha256sum -c
}

HOST_BUILD=0
[ "${1:-}" = "--host" ] && HOST_BUILD=1
VIDEO_ARGS=()

if [ "$HOST_BUILD" = 1 ]; then
    PREFIX="$ROOT/build/ffmpeg-host"
    DESTDIR=""
    BUILD_DIR="$ROOT/build/ffmpeg-build-host"
    READELF=readelf
    SSL_CMAKE_ARGS=()
    # The desktop has no Starfish, so jf/smp_host.c decodes the picture itself:
    # VAAPI where the GPU has it, dav1d or FFmpeg's own decoders otherwise, and
    # swscale to turn the result into RGBA. None of this goes to the TV.
    TARGET_ARGS=(
        --extra-cflags="-I$PREFIX/include"
        --extra-ldflags="-L$PREFIX/lib -Wl,-rpath,$PREFIX/lib"
    )
    VIDEO_ARGS=(
        --enable-swscale --enable-vaapi --enable-libdav1d
        --enable-decoder=h264,hevc,vp9,av1,libdav1d
        --enable-hwaccel=h264_vaapi,hevc_vaapi,vp9_vaapi,av1_vaapi
    )
    export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
else
    SDK="${WEBOS_SDK:-/opt/arm-webos-linux-gnueabi_sdk-buildroot}"
    [ -d "$SDK" ] || { echo "error: no webOS NDK at $SDK (set WEBOS_SDK)" >&2; exit 1; }
    SYSROOT="$SDK/arm-webos-linux-gnueabi/sysroot"
    CROSS="$SDK/bin/arm-webos-linux-gnueabi-"
    PREFIX=/usr/local/ffmpeg
    DESTDIR="$SYSROOT"
    BUILD_DIR="$ROOT/build/ffmpeg-build"
    READELF="${CROSS}readelf"
    # Same as target + thumb for size
    TARGET_ARGS=(
        --enable-cross-compile --target-os=linux --arch=arm --cpu=cortex-a55
        --cross-prefix="$CROSS" --pkg-config="$SDK/bin/pkg-config" --enable-lto
        --enable-thumb
        --extra-cflags="--sysroot=$SYSROOT -mthumb -mfpu=neon-vfpv4 -mfloat-abi=softfp"
        --extra-cflags="-I$SYSROOT$PREFIX/include"
        --extra-ldflags="--sysroot=$SYSROOT -L$SYSROOT$PREFIX/lib"
        --extra-ldflags="-Wl,-rpath-link,$SYSROOT$PREFIX/lib"
    )
    # Prefer our packages over sysroot
    export PKG_CONFIG_LIBDIR="$SYSROOT$PREFIX/lib/pkgconfig:$SYSROOT/usr/lib/pkgconfig:$SYSROOT/usr/share/pkgconfig"
    export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
    SSL_CMAKE_ARGS=(
        -DCMAKE_TOOLCHAIN_FILE="$SDK/share/buildroot/toolchainfile.cmake"
        -DCMAKE_C_FLAGS="-mthumb -mcpu=cortex-a55 -mfpu=neon-vfpv4 -mfloat-abi=softfp"
    )
fi

mkdir -p "$ROOT/build/downloads"
fetch "$URL" "$SHA256" "$ARCHIVE"

# Always rebuild
rm -rf "${DESTDIR}${PREFIX}" "$SRC_DIR" "$BUILD_DIR"
mkdir -p "$SRC_DIR" "$BUILD_DIR"
tar -xf "$ARCHIVE" -C "$SRC_DIR" --strip-components=1

# ------------------------------------------------------------------------ mbedTLS
fetch "$SSL_URL" "$SSL_SHA256" "$SSL_ARCHIVE"
rm -rf "$SSL_SRC_DIR"
mkdir -p "$SSL_SRC_DIR"
tar -xf "$SSL_ARCHIVE" -C "$SSL_SRC_DIR" --strip-components=1
# Disable ARMv8 AES extension
python3 "$SSL_SRC_DIR/scripts/config.py" unset MBEDTLS_AESCE_C
cmake -S "$SSL_SRC_DIR" -B "$SSL_SRC_DIR/build" -G Ninja \
    "${SSL_CMAKE_ARGS[@]}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$PREFIX" \
    -DCMAKE_INSTALL_RPATH='$ORIGIN' \
    -DUSE_SHARED_MBEDTLS_LIBRARY=ON -DUSE_STATIC_MBEDTLS_LIBRARY=OFF \
    -DENABLE_PROGRAMS=OFF -DENABLE_TESTING=OFF -DMBEDTLS_FATAL_WARNINGS=OFF
cmake --build "$SSL_SRC_DIR/build"

DESTDIR="$DESTDIR" cmake --install "$SSL_SRC_DIR/build" --strip

cd "$BUILD_DIR"
"$SRC_DIR/configure" \
    "${TARGET_ARGS[@]}" \
    --prefix="$PREFIX" \
    --enable-shared --disable-static --enable-pic \
    --enable-version3 \
    --disable-autodetect --enable-zlib --enable-mbedtls \
    --disable-programs --disable-doc \
    --disable-avdevice --disable-avfilter --disable-swscale \
    --disable-encoders --disable-muxers --disable-devices --disable-filters \
    --disable-hwaccels --disable-decoders \
    --disable-demuxers --disable-protocols \
    --enable-demuxer=matroska,mov,mpegts,mp3,flac,ogg,wav,aac,ac3 \
    --enable-decoder=aac,aac_latm,ac3,eac3,mp3,flac,opus,vorbis,alac,dca,truehd,mlp \
    --enable-decoder=pcm_s16le,pcm_s16be,pcm_s24le,pcm_s32le,pcm_f32le \
    --enable-decoder=ass,ssa,subrip,srt,webvtt,movtext,text \
    --enable-decoder=pgssub,dvdsub,dvbsub \
    --enable-protocol=file,http,https,tcp,tls \
    --disable-bsfs --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,extract_extradata \
    --disable-parsers \
    --enable-parser=h264,hevc,vp9,av1 \
    --enable-parser=aac,aac_latm,ac3,mpegaudio,flac,opus,vorbis,dca,mlp \
    "${VIDEO_ARGS[@]}"

make -j"$(nproc)"
make install ${DESTDIR:+DESTDIR="$DESTDIR"}

INSTALLED="${DESTDIR}${PREFIX}/lib"
for want in HTTPS_PROTOCOL MATROSKA_DEMUXER MOV_DEMUXER MPEGTS_DEMUXER AAC_DECODER \
            AC3_DECODER H264_MP4TOANNEXB_BSF HEVC_MP4TOANNEXB_BSF EXTRACT_EXTRADATA_BSF \
            H264_PARSER HEVC_PARSER ASS_DECODER SUBRIP_DECODER MOVTEXT_DECODER WEBVTT_DECODER \
            PGSSUB_DECODER DVDSUB_DECODER; do
    grep -q "^#define CONFIG_$want 1$" config_components.h || {
        echo "error: CONFIG_$want is off" >&2; exit 1; }
done
UNWANTED="HLS_DEMUXER AVFILTER"
[ "$HOST_BUILD" = 1 ] || UNWANTED="H264_DECODER $UNWANTED"
for unwanted in $UNWANTED; do
    if grep -qs "^#define CONFIG_$unwanted 1$" config_components.h config.h; then
        echo "error: CONFIG_$unwanted is on" >&2; exit 1
    fi
done
if "$READELF" -d "$INSTALLED"/libavcodec.so.* 2>/dev/null | grep -q "libstdc++"; then
    echo "error: the build links libstdc++" >&2; exit 1
fi
"$READELF" -d "$INSTALLED"/libavformat.so.* | grep -q "libmbedtls.so" || {
    echo "error: libavformat does not use the bundled mbedTLS" >&2; exit 1; }
for lib in libmbedtls libmbedx509 libmbedcrypto; do
    ls "$INSTALLED/$lib.so".* >/dev/null 2>&1 || {
        echo "error: $lib is missing" >&2; exit 1; }
done
if "$READELF" -d "$INSTALLED"/libavformat.so.* | grep -qE "libssl|libcrypto\."; then
    echo "error: libavformat picked up an OpenSSL instead" >&2; exit 1
fi

printf '\nInstalled FFmpeg %s into %s\n' "$VERSION" "$INSTALLED"
find "$INSTALLED" -maxdepth 1 -type f -name "*.so.*" -printf "%9s  %f\n" | sort -k2 |
    awk '{ total += $1; print } END { printf "%9d  total\n", total }'
