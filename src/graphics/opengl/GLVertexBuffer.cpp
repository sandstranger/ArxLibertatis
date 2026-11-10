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
//! Enabled vertex attribute arrays, one bit per GLESVertexAttribute
static u32 glEnabledVertexAttributes = 0;
#else
static int glArrayClientStateTexCount = 1;
#endif
static GLuint glBoundBuffer = GL_NONE;

#ifdef ANDROID
void resetVertexArrayState() {
	glArrayClientState = GL_NoArray;
	glArrayClientStateRef = nullptr;
	glArrayClientStateOffset = nullptr;
	glBoundBuffer = GL_NONE;
	glBoundIndexBuffer = GL_NONE;
	glBindBuffer(GL_ARRAY_BUFFER, GL_NONE);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, GL_NONE);
	// A recreated context or the MSAA copy may have left any array enabled
	for(GLuint attribute = 0; attribute < GLESAttributeCount; ++attribute) {
		glDisableVertexAttribArray(attribute);
	}
	glEnabledVertexAttributes = 0;
}

//! Enable exactly the vertex attribute arrays in mask, only changing what differs
static void setEnabledVertexAttributes(u32 mask) {
	u32 changed = mask ^ glEnabledVertexAttributes;
	for(GLuint attribute = 0; changed != 0; ++attribute, changed >>= 1) {
		if(changed & 1) {
			if(mask & (u32(1) << attribute)) {
				glEnableVertexAttribArray(attribute);
			} else {
				glDisableVertexAttribArray(attribute);
			}
		}
	}
	glEnabledVertexAttributes = mask;
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
	// The array is enabled by switchVertexArray()
	glVertexAttribPointer(GLuint(GLESAttributeTexCoord0 + index), 2, GL_FLOAT, GL_FALSE, GLsizei(stride), coord);
#else
	glClientActiveTexture(GL_TEXTURE0 + index);
	if(index > 0) glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glTexCoordPointer(2, GL_FLOAT, stride, coord);
#endif
}

bool switchVertexArray(GLArrayClientState type, const void * ref, int texcount, const void * vertexOffset,
                       u32 extraAttributes) {

#ifdef ANDROID
	// A VBO can be reused with different base offsets for indexed draws.
	// Skip redundant pointer setup only when both the buffer and its base
	// offset are unchanged.
	if(glArrayClientState == type && glArrayClientStateRef == ref
	   && glArrayClientStateOffset == vertexOffset) {
		return false;
	}

	// Arrays the layout does not use are disabled: an enabled array keeps pointing into
	// its buffer, which may be deleted or orphaned.
	arx_assert(texcount >= 1 && texcount <= 3);
	u32 mask = glesAttributeBit(GLESAttributePosition) | glesAttributeBit(GLESAttributeColor);
	for(int i = 0; i < texcount; i++) {
		mask |= glesAttributeBit(GLESVertexAttribute(GLESAttributeTexCoord0 + i));
	}
	setEnabledVertexAttributes(mask | extraAttributes);
#else
	ARX_UNUSED(vertexOffset);
	ARX_UNUSED(extraAttributes);
	if(glArrayClientState == type && glArrayClientStateRef == ref) {
		return false;
	}

	if(glArrayClientState != type) {
		arx_assert(texcount >= 1);
		for(int i = texcount; i < glArrayClientStateTexCount; i++) {
			glClientActiveTexture(GL_TEXTURE0 + i);
			glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		}
		glArrayClientStateTexCount = texcount;
	}
#endif

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
