# One binary of plain asserts over the parts that have no device in them. No framework:
# these exist so a wrong branch fails a build, not to describe the code.
add_executable(jf-tests tests.c "${CMAKE_SOURCE_DIR}/src/jf/store.c")
target_link_libraries(jf-tests PRIVATE jf-portable)
if (TARGET jf-demux)
    target_link_libraries(jf-tests PRIVATE jf-demux)
    target_compile_definitions(jf-tests PRIVATE JF_HAVE_DEMUX=1)
endif ()
# libass needs a real font to rasterise with, so this only runs where one is installed.
if (EXISTS "${LIBASS_ROOT}/lib")
    target_sources(jf-tests PRIVATE "${CMAKE_SOURCE_DIR}/src/jf/subs.c")
    target_include_directories(jf-tests SYSTEM PRIVATE "${LIBASS_ROOT}/include")
    target_link_libraries(jf-tests PRIVATE "${LIBASS_ROOT}/lib/libass.so")
    target_compile_definitions(jf-tests PRIVATE JF_HAVE_SUBS=1)
else ()
    message(STATUS "No libass at ${LIBASS_ROOT} - skipping the subtitle test "
            "(tools/build-libass.sh --host)")
endif ()
add_test(NAME jf-tests COMMAND jf-tests)

# The app itself against a mock server (api_mock.c), on SDL's offscreen driver: focus and
# navigation, screen by screen. Wherever the app builds.
if (TARGET jf-app AND NOT TARGET_WEBOS)
    add_executable(jf-ui-tests ui_tests.c api_mock.c player_mock.c
            "${CMAKE_SOURCE_DIR}/src/jf/picsubs.c")
    target_link_libraries(jf-ui-tests PRIVATE jf-app)
    add_test(NAME jf-ui-tests COMMAND jf-ui-tests)
    add_test(NAME jf-ui-tests-animated COMMAND jf-ui-tests animated)
endif ()
