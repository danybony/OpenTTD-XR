/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/gpl-2.0>.
 */

/**
 * @file opengl_es.cpp Compatibility layer that runs the desktop OpenGL backend on an OpenGL ES 3.0 context.
 *
 * OpenGL ES 3.0 lacks a few things the backend and the 3D renderer use. Rather than
 * branching at every call site, the procedure lookup hands out the functions below in
 * place of the missing or incompatible ones:
 *  - 1D textures become 2D textures of height 1.
 *  - BGRA uploads become RGBA uploads of the same bytes, with red and blue swizzled when
 *    sampling. Textures that are rendered to must therefore be created as GL_RGBA.
 *  - GLSL 1.50 sources are rewritten to GLSL ES 3.00.
 *  - Diagnostics without an ES equivalent (wireframe, GPU timers) do nothing.
 */

#include "../stdafx.h"

#if defined(__APPLE__)
#	include <OpenGL/gl3.h>
#else
#	include <GL/gl.h>
#endif
#include "../3rdparty/opengl/glext.h"

#include "opengl_es.h"

#include <regex>

#include "../safeguards.h"

/* glext.h only declares function pointer types for what came after OpenGL 1.1. */
using PFNGLBINDTEXTUREPROC = void (APIENTRY *)(GLenum, GLuint);
using PFNGLTEXPARAMETERIPROC = void (APIENTRY *)(GLenum, GLenum, GLint);
using PFNGLTEXIMAGE2DPROC = void (APIENTRY *)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
using PFNGLTEXSUBIMAGE2DPROC = void (APIENTRY *)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
using PFNGLREADPIXELSPROC = void (APIENTRY *)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
using PFNGLGETINTEGERVPROC = void (APIENTRY *)(GLenum, GLint *);
using PFNGLDRAWARRAYSPROC = void (APIENTRY *)(GLenum, GLint, GLsizei);

static GetOGLProcAddressProc _real_get_proc = nullptr;

/** Get a function pointer of the real context on first use. */
#define REAL(type, name) static const auto real = reinterpret_cast<type>(_real_get_proc(name))

static GLenum ToESTarget(GLenum target)
{
	return target == GL_TEXTURE_1D ? GL_TEXTURE_2D : target;
}

/** Make sampling the bound texture see the BGRA bytes uploaded as RGBA in the right order. */
static void SwizzleBoundTexture(GLenum target)
{
	REAL(PFNGLTEXPARAMETERIPROC, "glTexParameteri");
	real(target, GL_TEXTURE_SWIZZLE_R, GL_BLUE);
	real(target, GL_TEXTURE_SWIZZLE_B, GL_RED);
}

static void APIENTRY ESBindTexture(GLenum target, GLuint texture)
{
	REAL(PFNGLBINDTEXTUREPROC, "glBindTexture");
	real(ToESTarget(target), texture);
}

static void APIENTRY ESTexParameteri(GLenum target, GLenum pname, GLint param)
{
	REAL(PFNGLTEXPARAMETERIPROC, "glTexParameteri");
	real(ToESTarget(target), pname, param);
}

static void APIENTRY ESTexImage2D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)
{
	REAL(PFNGLTEXIMAGE2DPROC, "glTexImage2D");
	target = ToESTarget(target);
	if (format == GL_BGRA) {
		real(target, level, GL_RGBA8, width, height, border, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		SwizzleBoundTexture(target);
	} else {
		real(target, level, internal_format, width, height, border, format, type, pixels);
	}
}

static void APIENTRY ESTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, const void *pixels)
{
	REAL(PFNGLTEXSUBIMAGE2DPROC, "glTexSubImage2D");
	if (format == GL_BGRA) {
		format = GL_RGBA;
		type = GL_UNSIGNED_BYTE;
	}
	real(ToESTarget(target), level, x, y, width, height, format, type, pixels);
}

static void APIENTRY ESTexImage1D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLint border, GLenum format, GLenum type, const void *pixels)
{
	ESTexImage2D(target, level, internal_format, width, 1, border, format, type, pixels);
}

static void APIENTRY ESTexSubImage1D(GLenum target, GLint level, GLint x, GLsizei width, GLenum format, GLenum type, const void *pixels)
{
	ESTexSubImage2D(target, level, x, 0, width, 1, format, type, pixels);
}

static void APIENTRY ESTexImage3D(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void *pixels)
{
	REAL(PFNGLTEXIMAGE3DPROC, "glTexImage3D");
	if (format == GL_BGRA) {
		real(target, level, GL_RGBA8, width, height, depth, border, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
		SwizzleBoundTexture(target);
	} else {
		real(target, level, internal_format, width, height, depth, border, format, type, pixels);
	}
}

static void *APIENTRY ESMapBuffer(GLenum target, GLenum access)
{
	REAL(PFNGLMAPBUFFERRANGEPROC, "glMapBufferRange");
	static const auto get_size = reinterpret_cast<PFNGLGETBUFFERPARAMETERIVPROC>(_real_get_proc("glGetBufferParameteriv"));
	GLint size = 0;
	get_size(target, GL_BUFFER_SIZE, &size);
	GLbitfield bits = 0;
	if (access != GL_WRITE_ONLY) bits |= GL_MAP_READ_BIT;
	if (access != GL_READ_ONLY) bits |= GL_MAP_WRITE_BIT;
	return real(target, 0, size, bits);
}

static void APIENTRY ESReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels)
{
	REAL(PFNGLREADPIXELSPROC, "glReadPixels");
	if (format != GL_BGRA) {
		real(x, y, width, height, format, type, pixels);
		return;
	}
	real(x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	GLint row_length = 0;
	static const auto get_integer = reinterpret_cast<PFNGLGETINTEGERVPROC>(_real_get_proc("glGetIntegerv"));
	get_integer(GL_PACK_ROW_LENGTH, &row_length);
	const GLsizei pitch = row_length != 0 ? row_length : width;
	auto *row = static_cast<uint8_t *>(pixels);
	for (GLsizei j = 0; j < height; j++, row += pitch * 4) {
		for (GLsizei i = 0; i < width; i++) std::swap(row[i * 4], row[i * 4 + 2]);
	}
}

static void APIENTRY ESGetIntegerv(GLenum pname, GLint *data)
{
	REAL(PFNGLGETINTEGERVPROC, "glGetIntegerv");
	if (pname == GL_POLYGON_MODE) {
		data[0] = data[1] = GL_FILL;
	} else {
		real(pname, data);
	}
}

static void APIENTRY ESDrawBuffer(GLenum buffer)
{
	REAL(PFNGLDRAWBUFFERSPROC, "glDrawBuffers");
	real(1, &buffer);
}

static void APIENTRY ESClearDepth(GLdouble depth)
{
	REAL(PFNGLCLEARDEPTHFPROC, "glClearDepthf");
	real(static_cast<GLfloat>(depth));
}

static void APIENTRY ESMultiDrawArrays(GLenum mode, const GLint *first, const GLsizei *count, GLsizei draws)
{
	REAL(PFNGLDRAWARRAYSPROC, "glDrawArrays");
	for (GLsizei i = 0; i < draws; i++) real(mode, first[i], count[i]);
}

static void APIENTRY ESPolygonMode(GLenum, GLenum) {}
static void APIENTRY ESBindFragDataLocation(GLuint, GLuint, const GLchar *) {}
static void APIENTRY ESBeginQuery(GLenum, GLuint) {}
static void APIENTRY ESEndQuery(GLenum) {}
static void APIENTRY ESGetQueryObjectiv(GLuint, GLenum, GLint *params) { *params = 0; }
static void APIENTRY ESGetQueryObjectui64v(GLuint, GLenum, GLuint64 *params) { *params = 0; }

/** Rewrite a GLSL 1.50 shader to GLSL ES 3.00; everything else the shaders use is common to both. */
static std::string TranslateShader(GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
	std::string source;
	for (GLsizei i = 0; i < count; i++) {
		if (lengths != nullptr && lengths[i] >= 0) {
			source.append(strings[i], lengths[i]);
		} else {
			source.append(strings[i]);
		}
	}

	static const std::regex version{R"(#version 150[^\n]*\n?)"};
	static const std::regex sampler{R"(\bsampler1D\b)"};
	static const std::regex lookup{R"(\btexture\((palette|pal), *(\w+)\))"};
	source = std::regex_replace(source, version, "");
	source = std::regex_replace(source, sampler, "sampler2D");
	source = std::regex_replace(source, lookup, "texture($1, vec2($2, 0.5))");

	return "#version 300 es\n"
		"precision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp usampler2D;\n"
		"precision highp sampler2DArray;\nprecision highp sampler2DShadow;\n" + source;
}

static void APIENTRY ESShaderSource(GLuint shader, GLsizei count, const GLchar *const *strings, const GLint *lengths)
{
	REAL(PFNGLSHADERSOURCEPROC, "glShaderSource");
	const std::string source = TranslateShader(count, strings, lengths);
	const GLchar *text = source.c_str();
	real(shader, 1, &text, nullptr);
}

/** Procedure lookup for an OpenGL ES context. */
static OGLProc GetESProcAddress(const char *proc)
{
	static const std::pair<std::string_view, OGLProc> replacements[] = {
#define ES(name, function) {name, reinterpret_cast<OGLProc>(&function)}
		ES("glBindTexture", ESBindTexture),
		ES("glTexParameteri", ESTexParameteri),
		ES("glTexImage1D", ESTexImage1D),
		ES("glTexSubImage1D", ESTexSubImage1D),
		ES("glTexImage2D", ESTexImage2D),
		ES("glTexSubImage2D", ESTexSubImage2D),
		ES("glTexImage3D", ESTexImage3D),
		ES("glMapBuffer", ESMapBuffer),
		ES("glReadPixels", ESReadPixels),
		ES("glGetIntegerv", ESGetIntegerv),
		ES("glDrawBuffer", ESDrawBuffer),
		ES("glClearDepth", ESClearDepth),
		ES("glMultiDrawArrays", ESMultiDrawArrays),
		ES("glPolygonMode", ESPolygonMode),
		ES("glBindFragDataLocation", ESBindFragDataLocation),
		ES("glBeginQuery", ESBeginQuery),
		ES("glEndQuery", ESEndQuery),
		ES("glGetQueryObjectiv", ESGetQueryObjectiv),
		ES("glGetQueryObjectui64v", ESGetQueryObjectui64v),
		ES("glShaderSource", ESShaderSource),
#undef ES
	};

	for (const auto &[name, function] : replacements) {
		if (name == proc) return function;
	}
	return _real_get_proc(proc);
}

/**
 * Wrap the platform's procedure lookup for use with an OpenGL ES 3.0 context.
 * @param get_proc The platform's lookup.
 * @return The lookup to use instead.
 */
GetOGLProcAddressProc WrapOGLProcAddressForES(GetOGLProcAddressProc get_proc)
{
	_real_get_proc = get_proc;
	return &GetESProcAddress;
}
