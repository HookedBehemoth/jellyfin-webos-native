# The Jellyfin client, and the two rendering probes that share its platform layer.
#
# Three tiers, by what each needs from the TV:
#   jf-portable       nothing                 - always built; what the host tests link
#   xmb / gltri / ui  SDL2, EGL, GLES         - wherever those exist, TV or desktop
#   the player        libplayerAPIs, libpf    - the TV only
#
# So `jellyfin` itself builds on a desktop, with the null backend in jf/player_null.c in
# place of Starfish: everything above playback runs, and pressing Play says why it will
# not. The desktop player is to be libmpv; see README.md.

find_package(PkgConfig REQUIRED)

if (WIN32)
    add_library(jf-os STATIC platform/os_windows.c)
    target_link_libraries(jf-os PUBLIC ws2_32)
else ()
    add_library(jf-os STATIC platform/os_posix.c)
endif ()

# The backend-agnostic pieces: the Starfish payloads, the A/V sync arithmetic, the clock,
# the packer and the layout geometry. The tests cover exactly this set.
add_library(jf-portable STATIC
        jf/smp_payload.c
        jf/arena.c
        jf/blurhash.c
        jf/cfg.c
        jf/packet_queue.c
        jf/clock.c
        ui/skyline.c
        ui/loom.c
        platform/luna_json.c)
target_include_directories(jf-portable PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(jf-portable PUBLIC jf-os m pthread)

# demux.c needs nothing but FFmpeg, so the host tests link it wherever
# `tools/build-ffmpeg.sh --host` has put one. On the TV it is part of `jellyfin` itself,
# further down. No FFmpeg here just means those tests are skipped.
if (NOT TARGET_WEBOS)
    if (EXISTS "${FFMPEG_ROOT}/lib")
        add_library(jf-demux STATIC jf/demux.c)
        target_include_directories(jf-demux PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
        target_include_directories(jf-demux SYSTEM PUBLIC "${FFMPEG_ROOT}/include")
        foreach (_lib avformat avcodec swresample avutil)
            # Absolute, so CMake records the rpath itself: a distro FFmpeg of the same
            # soname in /usr/lib must not win the lookup.
            target_link_libraries(jf-demux PUBLIC "${FFMPEG_ROOT}/lib/lib${_lib}.so")
        endforeach ()
    else ()
        message(STATUS "No FFmpeg at ${FFMPEG_ROOT} - skipping the demux tests "
                "(tools/build-ffmpeg.sh --host)")
    endif ()
endif ()

# ------------------------------------------------------------ platform layer

if (TARGET_WEBOS)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    pkg_check_modules(GLIB REQUIRED glib-2.0)
    # The NDK's sysroot carries libEGL and libGLESv2 but no pkg-config files for them.
    set(GL_LIBRARIES GLESv2 EGL)
    set(PLATFORM_EXTRA ${GLIB_LIBRARIES} helpers)
    set(HAVE_PLATFORM ON)
else ()
    # A desktop without these still builds jf-portable and its tests; it just gets no
    # windowed programs.
    pkg_check_modules(SDL2 sdl2)
    if (WIN32)
        # GLES comes from ANGLE, whose mingw-w64 package installs the headers and import
        # libraries into the toolchain prefix but ships no pkg-config files.
        set(EGL_FOUND TRUE)
        set(GLESV2_FOUND TRUE)
        set(GL_LIBRARIES GLESv2 EGL)
    else ()
        pkg_check_modules(EGL egl)
        pkg_check_modules(GLESV2 glesv2)
        set(GL_LIBRARIES ${GLESV2_LIBRARIES} ${EGL_LIBRARIES})
    endif ()
    set(PLATFORM_EXTRA "")
    if (SDL2_FOUND AND EGL_FOUND AND GLESV2_FOUND)
        set(HAVE_PLATFORM ON)
    else ()
        set(HAVE_PLATFORM OFF)
        message(STATUS "No SDL2/EGL/GLES here - skipping xmb and gltri")
    endif ()
endif ()

if (HAVE_PLATFORM)
    # Window, GL context, input and the webOS lifecycle. Off the TV the webOS-only halves
    # compile out and SDL picks an ordinary desktop backend, so the probes run either way.
    add_library(jf-platform STATIC platform/window.c platform/luna.c platform/luna_json.c)
    target_include_directories(jf-platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(jf-platform SYSTEM PUBLIC ${SDL2_INCLUDE_DIRS}
            ${GLIB_INCLUDE_DIRS} ${GLESV2_INCLUDE_DIRS} ${EGL_INCLUDE_DIRS})
    target_link_directories(jf-platform PUBLIC ${SDL2_LIBRARY_DIRS})
    target_link_libraries(jf-platform PUBLIC jf-os ${SDL2_LIBRARIES} ${GL_LIBRARIES}
            ${PLATFORM_EXTRA} pthread)
    if (TARGET_WEBOS)
        target_compile_definitions(jf-platform PUBLIC JF_WEBOS=1)
    endif ()


    # The instanced UI batch and its glyph atlas.
    pkg_check_modules(FREETYPE REQUIRED freetype2)
    add_library(jf-ui STATIC ui/loom.c ui/skyline.c ui/glyph_atlas.c ui/renderer.c)
    target_include_directories(jf-ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_include_directories(jf-ui SYSTEM PUBLIC ${FREETYPE_INCLUDE_DIRS})
    target_link_directories(jf-ui PUBLIC ${FREETYPE_LIBRARY_DIRS})
    target_link_libraries(jf-ui PUBLIC jf-platform ${FREETYPE_LIBRARIES} m)
    foreach (_entry vsUi:ui_vs:vertex fsFill:ui_fill:fragment fsRound:ui_round:fragment
            fsBorder:ui_border:fragment fsGlyph:ui_glyph:fragment fsImage:ui_image:fragment
            fsImagePremultiplied:ui_image_premultiplied:fragment)
        string(REPLACE ":" ";" _parts "${_entry}")
        list(GET _parts 0 _fn)
        list(GET _parts 1 _symbol)
        list(GET _parts 2 _stage)
        webos_add_shader(jf-ui SYMBOL ${_symbol}
                SRC "${CMAKE_CURRENT_SOURCE_DIR}/shaders/ui.slang" ENTRY ${_fn} STAGE ${_stage})
    endforeach ()

    # What the two probes have in common: shader compilation, the frame-time overlay and
    # the ASCII framebuffer dump.
    add_library(jf-probe STATIC apps/probe.c ui/text.c)
    target_include_directories(jf-probe PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(jf-probe PUBLIC jf-platform m)
    webos_embed(jf-probe SYMBOL font8x16 FILE "${CMAKE_SOURCE_DIR}/assets/font8x16.bin")
    webos_add_shader(jf-probe SYMBOL text_vs SRC "${CMAKE_CURRENT_SOURCE_DIR}/shaders/text.slang"
            ENTRY vsText STAGE vertex)
    webos_add_shader(jf-probe SYMBOL text_fs SRC "${CMAKE_CURRENT_SOURCE_DIR}/shaders/text.slang"
            ENTRY fsText STAGE fragment)

    add_executable(xmb apps/xmb.c)
    target_link_libraries(xmb PRIVATE jf-probe)
    webos_add_shader(xmb SYMBOL xmb_vs SRC "${CMAKE_CURRENT_SOURCE_DIR}/shaders/xmb.slang"
            ENTRY vsMain STAGE vertex)
    webos_add_shader(xmb SYMBOL xmb_fs SRC "${CMAKE_CURRENT_SOURCE_DIR}/shaders/xmb.slang"
            ENTRY fsMain STAGE fragment)

    add_executable(gltri apps/gltri.c)
    target_link_libraries(gltri PRIVATE jf-probe)
    webos_add_shader(gltri SYMBOL tri_vs SRC "${CMAKE_CURRENT_SOURCE_DIR}/shaders/tri.slang"
            ENTRY vsMain STAGE vertex)
    webos_add_shader(gltri SYMBOL tri_fs SRC "${CMAKE_CURRENT_SOURCE_DIR}/shaders/tri.slang"
            ENTRY fsMain STAGE fragment)

    webos_add_ipk(TARGET xmb APPID dev.hookedbehemoth.xmb TITLE "XMB" VERSION 0.0.1
            COLOR "#1f2a44" ICON "${CMAKE_SOURCE_DIR}/assets/icon.png" WEBOS_VERSIONS ">=5")
    webos_add_ipk(TARGET gltri APPID dev.hookedbehemoth.gltri TITLE "GL Triangles"
            VERSION 0.0.1 COLOR "#1f6feb" ICON "${CMAKE_SOURCE_DIR}/assets/icon.png"
            WEBOS_VERSIONS ">=5")
endif ()

# ------------------------------------------------------------------ jellyfin
#
# Everything above playback is portable. The player is not: video on the TV belongs to
# libplayerAPIs and libpf, which exist nowhere else, so a desktop build gets the null
# backend instead and refuses to play. See README.md for what replaces it.

if (NOT HAVE_PLATFORM)
    return()
endif ()

pkg_check_modules(LIBCURL libcurl)
pkg_check_modules(LIBPNG libpng16)
if (TARGET_WEBOS)
    # Static: the NDK ships libjson-c.a, so nothing has to be bundled or assumed present
    # on the TV for four endpoints' worth of parsing.
    find_library(JSONC_LIBRARY NAMES libjson-c.a REQUIRED)
    find_path(JSONC_INCLUDE_DIR NAMES json.h PATH_SUFFIXES json-c REQUIRED)
    set(JSONC_LIBRARIES "${JSONC_LIBRARY}")
    set(JSONC_INCLUDE_DIRS "${JSONC_INCLUDE_DIR}")
    set(JSONC_FOUND ON)
else ()
    pkg_check_modules(JSONC json-c)
endif ()

if (NOT (LIBCURL_FOUND AND LIBPNG_FOUND AND JSONC_FOUND))
    message(STATUS "No libcurl/libpng/json-c here - skipping jellyfin")
    return()
endif ()

add_executable(jellyfin
        app/main.c
        jf/api.c
        jf/arena.c
        jf/blurhash.c
        jf/cfg.c
        jf/image.c
        jf/store.c
        jf/subs.c)

# The licences of what the package distributes - its own shared objects and the one
# static library - embedded rather than staged: an installed app runs as a jail uid, and a
# licence that fails to open is not an acceptable outcome. The TV's own libraries are not
# here; see src/app/licenses.h. Symbol names match it.
foreach (_license agpl_3_0|${CMAKE_SOURCE_DIR}/LICENSE.txt
        lgpl_3_0|${CMAKE_SOURCE_DIR}/assets/licenses/lgpl-3.0.txt
        lgpl_2_1|${CMAKE_SOURCE_DIR}/assets/licenses/lgpl-2.1.txt
        apache_2_0|${CMAKE_SOURCE_DIR}/assets/licenses/apache-2.0.txt
        libass|${CMAKE_SOURCE_DIR}/assets/licenses/libass.txt
        harfbuzz|${CMAKE_SOURCE_DIR}/assets/licenses/harfbuzz.txt
        mit|${CMAKE_SOURCE_DIR}/assets/licenses/mit.txt)
    string(REPLACE "|" ";" _parts "${_license}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _path)
    webos_embed(jellyfin SYMBOL license_${_name} FILE "${_path}")
endforeach ()

if (WIN32)
    # MSYS2 packages libass, so there is nothing to build out of tree.
    pkg_check_modules(LIBASS REQUIRED libass)
    target_include_directories(jellyfin SYSTEM PRIVATE ${LIBASS_INCLUDE_DIRS})
    target_link_directories(jellyfin PRIVATE ${LIBASS_LIBRARY_DIRS})
    target_link_libraries(jellyfin PRIVATE ${LIBASS_LIBRARIES})
else ()
    if (NOT EXISTS "${LIBASS_ROOT}/lib")
        message(FATAL_ERROR "No libass at ${LIBASS_ROOT} - run tools/build-libass.sh")
    endif ()
    target_include_directories(jellyfin SYSTEM PRIVATE "${LIBASS_ROOT}/include")
    target_link_libraries(jellyfin PRIVATE "${LIBASS_ROOT}/lib/libass.so")
endif ()
target_include_directories(jellyfin SYSTEM PRIVATE
        ${LIBCURL_INCLUDE_DIRS} ${LIBPNG_INCLUDE_DIRS} ${JSONC_INCLUDE_DIRS})
target_link_directories(jellyfin PRIVATE ${LIBCURL_LIBRARY_DIRS} ${LIBPNG_LIBRARY_DIRS}
        ${JSONC_LIBRARY_DIRS})
if (NOT WIN32)
    set(PLATFORM_SYSTEM_LIBS dl)
endif ()
target_link_libraries(jellyfin PRIVATE jf-ui jf-probe
        ${LIBCURL_LIBRARIES} ${LIBPNG_LIBRARIES} ${JSONC_LIBRARIES}
        ${PLATFORM_SYSTEM_LIBS} m pthread)

if (NOT TARGET_WEBOS)
    # The real player against a faked pipeline (jf/smp_host.c): audio, subtitles
    # and seeking on a desktop, with a clock where the picture would be. Without
    # ALSA or a host FFmpeg, the null backend.
    pkg_check_modules(ALSA alsa)
    if (NOT (ALSA_FOUND AND TARGET jf-demux))
        target_sources(jellyfin PRIVATE jf/player_null.c)
        return()
    endif ()
    target_sources(jellyfin PRIVATE
            jf/player.c
            jf/picsubs.c
            jf/audio_alsa.c
            jf/clock.c
            jf/packet_queue.c
            jf/smp_payload.c
            jf/smp_host.c)
    target_include_directories(jellyfin SYSTEM PRIVATE ${ALSA_INCLUDE_DIRS})
    target_link_libraries(jellyfin PRIVATE jf-demux ${ALSA_LIBRARIES})
    # The picture too, when the host FFmpeg was built with swscale for it.
    if (EXISTS "${FFMPEG_ROOT}/lib/libswscale.so")
        target_compile_definitions(jellyfin PRIVATE JF_HOST_VIDEO=1)
        target_link_libraries(jellyfin PRIVATE "${FFMPEG_ROOT}/lib/libswscale.so")
    endif ()
    return()
endif ()

pkg_check_modules(ALSA REQUIRED alsa)
pkg_check_modules(LIBPLAYERAPIS REQUIRED libplayerAPIs)

# The one C++ translation unit in the program. Everything about the StarfishMediaAPIs ABI
# lives here and nowhere else; see jf/smp_shim.cpp.
add_library(jf-smp-shim STATIC jf/smp_shim.cpp)
target_include_directories(jf-smp-shim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_include_directories(jf-smp-shim SYSTEM PUBLIC ${LIBPLAYERAPIS_INCLUDE_DIRS})
target_link_directories(jf-smp-shim PUBLIC ${LIBPLAYERAPIS_LIBRARY_DIRS})
# stdc++ explicitly: this is a C program that happens to contain one C++ translation unit,
# so the C driver links it and will not add the runtime by itself.
target_link_libraries(jf-smp-shim PUBLIC ${LIBPLAYERAPIS_LIBRARIES} stdc++)

target_sources(jellyfin PRIVATE
        jf/player.c
        jf/picsubs.c
        jf/demux.c
        jf/audio_alsa.c
        jf/clock.c
        jf/packet_queue.c
        jf/smp_payload.c
        jf/smp_segment.c)
target_include_directories(jellyfin SYSTEM PRIVATE ${ALSA_INCLUDE_DIRS}
        "${FFMPEG_ROOT}/include")
target_link_directories(jellyfin PRIVATE "${FFMPEG_ROOT}/lib")
target_link_options(jellyfin PRIVATE "-Wl,-rpath-link,${FFMPEG_ROOT}/lib"
        "-Wl,-rpath-link,${LIBASS_ROOT}/lib")
target_link_libraries(jellyfin PRIVATE jf-smp-shim ${ALSA_LIBRARIES}
        avformat avcodec swresample avutil)
# The bundled FFmpeg lives beside the binary, which is what $ORIGIN/lib resolves to.
set_target_properties(jellyfin PROPERTIES
        INSTALL_RPATH "$ORIGIN/lib"
        BUILD_WITH_INSTALL_RPATH TRUE)

set(FFMPEG_BUNDLE "")
foreach (_soname IN LISTS FFMPEG_SONAMES)
    list(APPEND FFMPEG_BUNDLE "${FFMPEG_ROOT}/lib/${_soname}")
endforeach ()
foreach (_soname IN LISTS LIBASS_SONAMES)
    list(APPEND FFMPEG_BUNDLE "${LIBASS_ROOT}/lib/${_soname}")
endforeach ()

webos_add_ipk(
        TARGET jellyfin
        APPID dev.hookedbehemoth.jellyfin
        TITLE "Jellyfin"
        VERSION 0.0.1
        COLOR "#00a4dc"
        ICON "${CMAKE_SOURCE_DIR}/assets/icon.png"
        WEBOS_VERSIONS ">=5"
        BUNDLE_LIBS ${FFMPEG_BUNDLE}
        WRITABLE_DIRS conf cache)
