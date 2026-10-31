/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/** @file gl.h Stand-in for the desktop header the NDK lacks; the desktop-only names are only ever handled by src/video/opengl_es.cpp. */

#ifndef OS_ANDROID_GL_GL_H
#define OS_ANDROID_GL_GL_H

#include <GLES3/gl3.h>

typedef double GLdouble;
typedef double GLclampd;

#define GL_TEXTURE_1D 0x0DE0
#define GL_POLYGON_MODE 0x0B40
#define GL_LINE 0x1B01
#define GL_FILL 0x1B02
#define GL_STENCIL_INDEX 0x1901

/* Only ever declared, to name the types of function pointers; the real ones come from the procedure lookup. */
#ifdef __cplusplus
extern "C" {
#endif
void glTexImage1D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLint border, GLenum format, GLenum type, const void *pixels);
void glTexSubImage1D(GLenum target, GLint level, GLint x, GLsizei width, GLenum format, GLenum type, const void *pixels);
void glClearDepth(GLdouble depth);
void glPolygonMode(GLenum face, GLenum mode);
void glDrawBuffer(GLenum buffer);
#ifdef __cplusplus
}
#endif

#endif /* OS_ANDROID_GL_GL_H */
