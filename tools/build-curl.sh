#!/usr/bin/env bash
# Builds the libcurl the Jellyfin client links on the TV: http, https, ws and wss.
#
#   tools/build-curl.sh            # cross build, installs into the NDK sysroot
#
# The firmware has a libcurl, but 7.82, from before WebSockets - and those are how the
# server says what changed (its /socket), so the home rows follow along without being
# asked. Desktop builds take the system's curl, which has them; there is no --host.
#
# Static, and hidden in the binary (see src/CMakeLists.txt): the TV's media pipeline,
# libplayerAPIs, loads the firmware's libcurl.so.4 itself, and a libcurl.so.4 of ours
# beside the app would be handed to it in its place. TLS is the mbedTLS that
# tools/build-ffmpeg.sh already bundles, so run that first; certificates are the
# firmware's bundle.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="${CURL_VERSION:-8.23.0}"
# Checked against curl-$VERSION.tar.xz.asc, Daniel Stenberg's key 27EDEAF22F3ABCEB50DB9A125CC908FDB71E12C2.
SHA256="${CURL_SHA256:-8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5}"
URL="${CURL_URL:-https://curl.se/download/curl-$VERSION.tar.xz}"
ARCHIVE="$ROOT/build/downloads/curl-$VERSION.tar.xz"
SRC_DIR="$ROOT/build/curl-src"
BUILD_DIR="$ROOT/build/curl-build"

fetch() { # url sha256 destination
    echo "$2  $3" | sha256sum -c --status 2>/dev/null && return 0
    curl -fL --retry 3 -o "$3" "$1"
    echo "$2  $3" | sha256sum -c
}

SDK="${WEBOS_SDK:-/opt/arm-webos-linux-gnueabi_sdk-buildroot}"
[ -d "$SDK" ] || { echo "error: no webOS NDK at $SDK (set WEBOS_SDK)" >&2; exit 1; }
SYSROOT="$SDK/arm-webos-linux-gnueabi/sysroot"
CROSS="$SDK/bin/arm-webos-linux-gnueabi-"
PREFIX=/usr/local/curl
MBEDTLS="$SYSROOT/usr/local/ffmpeg"
[ -f "$MBEDTLS/include/mbedtls/ssl.h" ] || {
    echo "error: no mbedTLS at $MBEDTLS - run tools/build-ffmpeg.sh first" >&2; exit 1; }

mkdir -p "$ROOT/build/downloads"
fetch "$URL" "$SHA256" "$ARCHIVE"

# Always rebuild
rm -rf "$SYSROOT$PREFIX" "$SRC_DIR" "$BUILD_DIR"
mkdir -p "$SRC_DIR" "$BUILD_DIR"
tar -xf "$ARCHIVE" -C "$SRC_DIR" --strip-components=1

cd "$BUILD_DIR"
# Same ABI the app is compiled with: base soft-float ABI, real FPU. See device.md.
"$SRC_DIR/configure" \
    --host=arm-webos-linux-gnueabi \
    CC="${CROSS}gcc" AR="${CROSS}ar" RANLIB="${CROSS}ranlib" \
    CFLAGS="--sysroot=$SYSROOT -Os -mthumb -mcpu=cortex-a55 -mfpu=neon-vfpv4 -mfloat-abi=softfp" \
    LDFLAGS="--sysroot=$SYSROOT -L$MBEDTLS/lib" \
    PKG_CONFIG=false \
    --prefix="$PREFIX" \
    --disable-shared --enable-static --with-pic \
    --with-mbedtls="$MBEDTLS" --with-zlib="$SYSROOT/usr" \
    --with-ca-bundle=/etc/ssl/certs/ca-certificates.crt --without-ca-path \
    --enable-websockets --enable-threaded-resolver \
    --disable-ftp --disable-file --disable-ipfs --disable-ldap --disable-ldaps \
    --disable-rtsp --disable-dict --disable-telnet --disable-tftp --disable-pop3 \
    --disable-imap --disable-smtp --disable-gopher --disable-mqtt --disable-smb \
    --disable-ntlm --disable-kerberos-auth --disable-negotiate-auth --disable-aws \
    --disable-manual --disable-docs --disable-libcurl-option \
    --without-libpsl --without-brotli --without-zstd --without-nghttp2 \
    --without-libidn2 --without-libssh2 --without-libgsasl

# The library and its headers; the curl tool is not wanted.
make -j"$(nproc)" -C lib
make -C lib install DESTDIR="$SYSROOT"
make -C include install DESTDIR="$SYSROOT"
make install-pkgconfigDATA DESTDIR="$SYSROOT"

INSTALLED="$SYSROOT$PREFIX/lib/libcurl.a"
for want in USE_MBEDTLS HAVE_LIBZ; do
    grep -q "^#define $want 1$" lib/curl_config.h || {
        echo "error: $want is off" >&2; exit 1; }
done
if grep -q "^#define CURL_DISABLE_WEBSOCKETS 1$" lib/curl_config.h; then
    echo "error: WebSockets are off" >&2; exit 1
fi
grep -q '^#define CURL_CA_BUNDLE "/etc/ssl/certs/ca-certificates.crt"$' lib/curl_config.h || {
    echo "error: the CA bundle is not the firmware's" >&2; exit 1; }

printf '\nInstalled libcurl %s into %s (%s bytes)\n' "$VERSION" "$INSTALLED" \
    "$(stat -c %s "$INSTALLED")"
