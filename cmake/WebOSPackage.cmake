# webos_add_ipk(TARGET <exe-target>
#               APPID <com.example.app>
#               TITLE <"Human Readable">
#               [VERSION <1.0.0>]
#               [ICON <path/to/icon.png>]   defaults to assets/icons/<target minus media->.png
#               [COLOR <#rrggbb>]           written to both appinfo iconColor and bgColor.
#                                           iconColor fills behind the icon - without it the
#                                           launcher default shows through the icon's
#                                           transparent corners - and bgColor is the tile
#                                           background. One value for both, matching the
#                                           icon, so the tile reads as a single block.
#               [WEBOS_VERSIONS <">=5">]    webOS releases this variant targets
#               [BUNDLE_LIBS <lib> ...]     shared objects to ship in lib/
#               [MEDIA <file> ...]          data files staged next to the binary
#               [WRITABLE_DIRS <name> ...]  directories created mode 777 inside the app.
#                                           An installed app runs as a jail uid, not as the
#                                           uid owning the installed files, so this is the
#                                           only way it can write beside its own binary.
#               )
#
# Adds three targets:
#   <exe-target>-ipk      stage + ares-package  ->  ${CMAKE_BINARY_DIR}/dist/<appid>_<ver>_<arch>.ipk
#   <exe-target>-install  ares-install of the above (honours $ENV{ARES_DEVICE})
#   <exe-target>-verify   webosbrew-ipk-verify against firmware symbol dumps
#
# Deliberately built out of `cmake -E` commands rather than CPack: for a samples repo the
# packaging step should be as readable as the sample itself.

set(WEBOS_PACKAGE_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

function(webos_add_ipk)
    cmake_parse_arguments(IPK "" "TARGET;APPID;TITLE;VERSION;ICON;COLOR;WEBOS_VERSIONS"
            "BUNDLE_LIBS;MEDIA;WRITABLE_DIRS;FILES" ${ARGN})

    if (NOT IPK_TARGET OR NOT IPK_APPID OR NOT IPK_TITLE)
        message(FATAL_ERROR "webos_add_ipk: TARGET, APPID and TITLE are required")
    endif ()
    if (NOT TARGET_WEBOS)
        return()
    endif ()
    if (NOT IPK_VERSION)
        set(IPK_VERSION "1.0.0")
    endif ()
    # Icons are white artwork on transparency, named after the target; the colour behind
    # them comes from COLOR below, which webOS paints itself.
    if (NOT IPK_ICON)
        string(REGEX REPLACE "^media-" "" _icon_name "${IPK_TARGET}")
        set(IPK_ICON "${CMAKE_SOURCE_DIR}/assets/icons/${_icon_name}.png")
    endif ()
    if (NOT EXISTS "${IPK_ICON}")
        message(FATAL_ERROR "No icon at ${IPK_ICON} - run assets/make-icon.py")
    endif ()
    if (NOT IPK_COLOR)
        set(IPK_COLOR "#1f6feb")
    endif ()

    set(_stage "${CMAKE_CURRENT_BINARY_DIR}/staging-${IPK_TARGET}")
    set(_dist "${CMAKE_BINARY_DIR}/dist")
    set(IPK_MAIN "$<TARGET_FILE_NAME:${IPK_TARGET}>")

    # appinfo.json is generated so the app id and title live in exactly one place: the
    # sample's CMakeLists, next to the compile definitions that select the webOS variant.
    configure_file("${WEBOS_PACKAGE_MODULE_DIR}/appinfo.json.in"
            "${CMAKE_CURRENT_BINARY_DIR}/${IPK_TARGET}-appinfo.json.gen" @ONLY)
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${IPK_TARGET}-appinfo.json"
            INPUT "${CMAKE_CURRENT_BINARY_DIR}/${IPK_TARGET}-appinfo.json.gen")

    set(_stage_cmds
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_stage}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_stage}" "${_stage}/lib" "${_dist}"
            COMMAND "${CMAKE_COMMAND}" -E copy
            "${CMAKE_CURRENT_BINARY_DIR}/${IPK_TARGET}-appinfo.json" "${_stage}/appinfo.json"
            COMMAND "${CMAKE_COMMAND}" -E copy "${IPK_ICON}" "${_stage}/icon.png"
            COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${IPK_TARGET}>" "${_stage}/")

    # `cmake -E copy` dereferences symlinks, which matters: ipk archives cannot carry them,
    # so every bundled lib has to be staged as a real file under its SONAME.
    foreach (_lib IN LISTS IPK_BUNDLE_LIBS)
        get_filename_component(_lib_name "${_lib}" NAME)
        list(APPEND _stage_cmds
                COMMAND "${CMAKE_COMMAND}" -E copy "${_lib}" "${_stage}/lib/${_lib_name}")
    endforeach ()

    if (IPK_MEDIA)
        list(APPEND _stage_cmds
                COMMAND "${CMAKE_COMMAND}" "-DMEDIA_FILES=${IPK_MEDIA}"
                -P "${WEBOS_PACKAGE_MODULE_DIR}/CheckSampleMedia.cmake")
        foreach (_media IN LISTS IPK_MEDIA)
            list(APPEND _stage_cmds COMMAND "${CMAKE_COMMAND}" -E copy "${_media}" "${_stage}/")
        endforeach ()
    endif ()

    # Anything else the app carries, next to it under its own name, its mode
    # kept: a script stays executable.
    foreach (_file IN LISTS IPK_FILES)
        list(APPEND _stage_cmds COMMAND "${CMAKE_COMMAND}" -E copy "${_file}" "${_stage}/")
    endforeach ()

    foreach (_dir IN LISTS IPK_WRITABLE_DIRS)
        list(APPEND _stage_cmds
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${_stage}/${_dir}"
                COMMAND chmod 777 "${_stage}/${_dir}")
    endforeach ()
    if (IPK_WRITABLE_DIRS)
        list(APPEND _stage_cmds COMMAND chmod 777 "${_stage}")
    endif ()

    set(_ipk "${_dist}/${IPK_APPID}_${IPK_VERSION}_${TARGET_WEBOS_ARCH}.ipk")

    add_custom_target(${IPK_TARGET}-ipk
            ${_stage_cmds}
            COMMAND ares-package "${_stage}" -o "${_dist}" --force-arch "${TARGET_WEBOS_ARCH}"
            DEPENDS ${IPK_TARGET}
            COMMENT "Packaging ${IPK_APPID}"
            VERBATIM)

    add_custom_target(${IPK_TARGET}-install
            COMMAND ares-install "${_ipk}"
            DEPENDS ${IPK_TARGET}-ipk
            COMMENT "Installing ${IPK_APPID}"
            VERBATIM)

    # webosbrew-ipk-verify reports, per firmware release, any symbol the ipk needs that the
    # TV does not export. This is the objective answer to the _GLIBCXX_USE_CXX11_ABI and
    # glibc-version questions, so it gets a first-class target rather than a README note.
    #
    # WEBOS_VERSIONS scopes it to the releases the variant is *for*. Without it the check
    # also reports the generations this build deliberately does not target - which is
    # informative to read once, but is not a build failure.
    set(_verify_args -S -d)
    if (IPK_WEBOS_VERSIONS)
        list(APPEND _verify_args -r "${IPK_WEBOS_VERSIONS}")
    endif ()
    add_custom_target(${IPK_TARGET}-verify
            COMMAND webosbrew-ipk-verify ${_verify_args} "${_ipk}"
            DEPENDS ${IPK_TARGET}-ipk
            COMMENT "Verifying ${IPK_APPID} against firmware symbol dumps"
            VERBATIM)
endfunction()
