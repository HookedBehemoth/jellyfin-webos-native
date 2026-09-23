/*
 * OpenGL ES for the applications.
 *
 * The Zig build dlopen'd libGLESv2 and fetched every entry point by name so it needed no
 * sysroot. With the NDK in hand there is nothing to arrange: the headers and the library
 * are both there, so this is a plain include plus the drawable size everything above the
 * platform layer works in.
 */
#pragma once

#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include <stdint.h>

extern uint32_t gl_width;
extern uint32_t gl_height;
