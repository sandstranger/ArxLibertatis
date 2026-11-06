/*
 * Copyright 2011-2022 Arx Libertatis Team (see the AUTHORS file)
 *
 * This file is part of Arx Libertatis.
 *
 * Arx Libertatis is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Arx Libertatis is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Arx Libertatis.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "graphics/opengl/GLVertexBuffer.h"

static GLArrayClientState glArrayClientState = GL_NoArray;
static const void * glArrayClientStateRef = nullptr;
#ifdef ANDROID
static const void * glArrayClientStateOffset = nullptr;
static GLuint glBoundIndexBuffer = GL_NONE;
#endif
static int glArrayClientStateTexCount = 1;
static GLuint glBoundBuffer = GL_NONE;

#ifdef ANDROID
void resetVertexArrayState() {
	glArrayClientState = GL_NoArray;
	glArrayClientStateRef = nullptr;
	glArrayClientStateOffset = nullptr;
	// Every vertex layout has UV0; mark UV1/UV2 as previously enabled so
	// the first single-UV layout disables their stale arrays after a resize.
	glArrayClientStateTexCount = 3;
	glBoundBuffer = GL_NONE;
	glBoundIndexBuffer = GL_NONE;
	glBindBuffer(GL_ARRAY_BUFFER, GL_NONE);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_NONE);
	for(GLuint attribute = 0; attribute < 5; ++attribute) {
		glDisableVertexAttribArray(attribute);
	}
}

void bindIndexBuffer(GLuint buffer) {
	if(buffer != glBoundIndexBuffer) {
		glBoundIndexBuffer = buffer;
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
	}
}

void unbindIndexBuffer(GLuint buffer) {
	if(buffer == glBoundIndexBuffer) {
		bindIndexBuffer(GL_NONE);
	}
}
#endif

std::vector<GLushort> glShortIndexBuffer;
std::vector<GLuint> glIntIndexBuffer;

void setVertexArrayTexCoord(int index, const void * coord, size_t stride) {
#ifdef ANDROID
	const GLuint attribute = GLuint(index + 2);
	glEnableVertexAttribArray(attribute);
	glVertexAttribPointer(attribute, 2, GL_FLOAT, GL_FALSE, GLsizei(stride), coord);
#else
	glClientActiveTexture(GL_TEXTURE0 + index);
	if(index > 0) glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(2, GL_FLOAT, stride, coord);
#endif
}

bool switchVertexArray(GLArrayClientState type, const void * ref, int texcount, const void * vertexOffset) {

#ifdef ANDROID
	// A VBO can be reused with different base offsets for indexed draws.
	// Skip redundant pointer setup only when both the buffer and its base
	// offset are unchanged.
	if(glArrayClientState == type && glArrayClientStateRef == ref
	   && glArrayClientStateOffset == vertexOffset) {
		return false;
	}
#else
	ARX_UNUSED(vertexOffset);
	if(glArrayClientState == type && glArrayClientStateRef == ref) {
		return false;
	}
#endif

	if(glArrayClientState != type) {
		arx_assert(texcount >= 1);
#ifdef ANDROID
		for(int i = texcount; i < glArrayClientStateTexCount; ++i) {
			glDisableVertexAttribArray(GLuint(i + 2));
		}
#else
		for(int i = texcount; i < glArrayClientStateTexCount; i++) {
			glClientActiveTexture(GL_TEXTURE0 + i);
			glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		}
#endif
		glArrayClientStateTexCount = texcount;
	}

	glArrayClientState = type;
	glArrayClientStateRef = ref;
#ifdef ANDROID
	glArrayClientStateOffset = vertexOffset;
#endif

	return true;
}

void clearVertexArray(const void * ref) {
	if(glArrayClientStateRef == ref) {
		glArrayClientStateRef = nullptr;
	}
}

void bindBuffer(GLuint buffer) {
	if(buffer != glBoundBuffer) {
		glBoundBuffer = buffer;
		glBindBuffer(GL_ARRAY_BUFFER, glBoundBuffer);
	}
}

void unbindBuffer(GLuint buffer) {
	if(buffer == glBoundBuffer) {
		bindBuffer(GL_NONE);
	}
}
