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

#ifndef ARX_GRAPHICS_OPENGL_GLVERTEXBUFFER_H
#define ARX_GRAPHICS_OPENGL_GLVERTEXBUFFER_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>
#include <limits>

#include "graphics/VertexBuffer.h"
#include "graphics/Vertex.h"
#include "graphics/Math.h"
#include "graphics/opengl/OpenGLRenderer.h"
#include "graphics/opengl/OpenGLUtil.h"

#include "io/log/Logger.h"

template <class Vertex>
static void setVertexArray(OpenGLRenderer * renderer, const Vertex * vertices, const void * ref);

// cached vertex array definitions
enum GLArrayClientState {
	GL_NoArray,
	GL_TexturedVertex,
	GL_SMY_VERTEX,
	GL_SMY_VERTEX3
};

#ifdef ANDROID
static inline const void * vertexAttributeOffset(const void * base, size_t offset) {
	return reinterpret_cast<const void *>(reinterpret_cast<std::uintptr_t>(base) + offset);
}
#endif

void bindBuffer(GLuint buffer);
void unbindBuffer(GLuint buffer);
#ifdef ANDROID
void bindIndexBuffer(GLuint buffer);
void unbindIndexBuffer(GLuint buffer);
void resetVertexArrayState();
#endif
void setVertexArrayTexCoord(int index, const void * coord, size_t stride);
bool switchVertexArray(GLArrayClientState type, const void * ref, int texcount, const void * vertexOffset);
void clearVertexArray(const void * ref);

template <>
inline void setVertexArray(OpenGLRenderer * renderer, const TexturedVertex * vertices, const void * ref) {
	ARX_UNUSED(vertices);

	if(!switchVertexArray(GL_TexturedVertex, ref, 1, vertices)) {
		return;
	}

#ifdef ANDROID
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(*vertices),
	                      vertexAttributeOffset(vertices, offsetof(TexturedVertex, p)));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(*vertices),
	                      vertexAttributeOffset(vertices, offsetof(TexturedVertex, color)));
	setVertexArrayTexCoord(0, vertexAttributeOffset(vertices, offsetof(TexturedVertex, uv)), sizeof(*vertices));
#else
	glVertexPointer(4, GL_FLOAT, sizeof(*vertices), &vertices->p);
	if(renderer->hasVertexFogCoordinate()) {
		glEnableClientState(GL_FOG_COORDINATE_ARRAY);
		glFogCoordPointer(GL_FLOAT, sizeof(*vertices), &vertices->w);
	}
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(*vertices), &vertices->color);
	setVertexArrayTexCoord(0, &vertices->uv, sizeof(*vertices));
#endif
}

template <>
inline void setVertexArray(OpenGLRenderer * renderer, const SMY_VERTEX * vertices, const void * ref) {
	if(!switchVertexArray(GL_SMY_VERTEX, ref, 1, vertices)) {
		return;
	}

#ifdef ANDROID
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(*vertices),
	                      vertexAttributeOffset(vertices, offsetof(SMY_VERTEX, p)));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(*vertices),
	                      vertexAttributeOffset(vertices, offsetof(SMY_VERTEX, color)));
	setVertexArrayTexCoord(0, vertexAttributeOffset(vertices, offsetof(SMY_VERTEX, uv)), sizeof(*vertices));
#else
	glVertexPointer(3, GL_FLOAT, sizeof(*vertices), &vertices->p.x);
	if(renderer->hasVertexFogCoordinate()) glDisableClientState(GL_FOG_COORDINATE_ARRAY);
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(*vertices), &vertices->color);
	setVertexArrayTexCoord(0, &vertices->uv, sizeof(*vertices));
#endif
}

template <>
inline void setVertexArray(OpenGLRenderer * renderer, const SMY_VERTEX3 * vertices, const void * ref) {
	if(!switchVertexArray(GL_SMY_VERTEX3, ref, 3, vertices)) {
		return;
	}

#ifdef ANDROID
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(*vertices),
	                      vertexAttributeOffset(vertices, offsetof(SMY_VERTEX3, p)));
	glEnableVertexAttribArray(1);
	glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(*vertices),
	                      vertexAttributeOffset(vertices, offsetof(SMY_VERTEX3, color)));
	setVertexArrayTexCoord(0, vertexAttributeOffset(vertices, offsetof(SMY_VERTEX3, uv)), sizeof(*vertices));
	setVertexArrayTexCoord(1, vertexAttributeOffset(vertices, offsetof(SMY_VERTEX3, uv) + sizeof(Vec2f)), sizeof(*vertices));
	setVertexArrayTexCoord(2, vertexAttributeOffset(vertices, offsetof(SMY_VERTEX3, uv) + 2 * sizeof(Vec2f)), sizeof(*vertices));
#else
	glVertexPointer(3, GL_FLOAT, sizeof(*vertices), &vertices->p.x);
	if(renderer->hasVertexFogCoordinate()) glDisableClientState(GL_FOG_COORDINATE_ARRAY);
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(*vertices), &vertices->color);
	setVertexArrayTexCoord(0, &vertices->uv[0], sizeof(*vertices));
	setVertexArrayTexCoord(1, &vertices->uv[1], sizeof(*vertices));
	setVertexArrayTexCoord(2, &vertices->uv[2], sizeof(*vertices));
#endif
}

extern const GLenum arxToGlPrimitiveType[];

extern std::vector<GLushort> glShortIndexBuffer;
extern std::vector<GLuint> glIntIndexBuffer;

template <class Vertex>
class BaseGLVertexBuffer : public VertexBuffer<Vertex> {
	
	typedef VertexBuffer<Vertex> Base;
	
public:
	
	using Base::capacity;
	
	BaseGLVertexBuffer(const BaseGLVertexBuffer &) = delete;
	BaseGLVertexBuffer & operator=(const BaseGLVertexBuffer &) = delete;
	
	BaseGLVertexBuffer(OpenGLRenderer * renderer, size_t size, Renderer::BufferUsage usage)
		: Base(size)
		, m_renderer(renderer)
		, m_buffer(GL_NONE)
		, m_offset(0)
		, m_usage(usage)
	{
		glGenBuffers(1, &m_buffer);
		arx_assert(m_buffer != GL_NONE);
	}
	
	void draw(Renderer::Primitive primitive, size_t count, size_t offset) const override {
		
		arx_assert(offset < capacity());
		arx_assert(offset + count <= capacity());
		
		m_renderer->beforeDraw<Vertex>();
		
		bindBuffer(m_buffer);
		
		setVertexArray<Vertex>(m_renderer, nullptr, this);
		
		glDrawArrays(arxToGlPrimitiveType[primitive], m_offset + offset, count);
		
	}
	
	void drawIndexed(Renderer::Primitive primitive, size_t count, size_t offset,
	                 const unsigned short * indices, size_t nbindices) const override {
		
		arx_assert(offset < capacity());
		arx_assert(offset + count <= capacity());
		arx_assert(indices != nullptr);
		
		offset += m_offset;
		
		m_renderer->beforeDraw<Vertex>();
		
		bindBuffer(m_buffer);
		
#ifdef ANDROID
		const void * indexData = m_renderer->streamIndices(indices, nbindices);
		if(m_renderer->hasDrawElementsBaseVertex()) {
			// Keep the attribute pointers at the start of the buffer and let the
			// OES/EXT base-vertex draw apply the offset.
			setVertexArray<Vertex>(m_renderer, nullptr, this);
			m_renderer->drawIndexedElements(primitive, indexData, nbindices, offset, count);
		} else {
			// No base-vertex draws: move the attribute pointers to the offset instead.
			const Vertex * baseVertices = reinterpret_cast<const Vertex *>(offset * sizeof(Vertex));
			setVertexArray<Vertex>(m_renderer, baseVertices, this);
			m_renderer->drawIndexedElements(primitive, indexData, nbindices, 0, count);
		}
#else
		setVertexArray<Vertex>(m_renderer, nullptr, this);
		GLenum mode = arxToGlPrimitiveType[primitive];
		GLenum type = GL_UNSIGNED_SHORT;
		const void * data = indices;
		
		if(m_renderer->hasDrawElementsBaseVertex()) {
			
			if(m_renderer->hasDrawRangeElements()) {
				#if !ARX_HAVE_GLEW
				glDrawRangeElementsBaseVertex(mode, 0, count - 1, nbindices, type, data, offset);
				#else
				glDrawRangeElementsBaseVertex(mode, 0, count - 1, nbindices, type, const_cast<void *>(data), offset);
				#endif
			} else {
				#if !ARX_HAVE_GLEW
				glDrawElementsBaseVertex(mode, nbindices, type, data, offset);
				#else
				glDrawElementsBaseVertex(mode, nbindices, type, const_cast<void *>(data), offset);
				#endif
			}
			
		} else {
			
			if(offset != 0) {
				if(offset + count - 1 <= std::numeric_limits<GLushort>::max()) {
					glShortIndexBuffer.resize(std::max(glShortIndexBuffer.size(), nbindices));
					for(size_t i = 0; i < nbindices; i++) {
						glShortIndexBuffer[i] = GLushort(indices[i] + offset);
					}
					type = GL_UNSIGNED_SHORT;
					data = glShortIndexBuffer.data();
				} else {
					glIntIndexBuffer.resize(std::max(glIntIndexBuffer.size(), nbindices));
					for(size_t i = 0; i < nbindices; i++) {
						glIntIndexBuffer[i] = GLuint(indices[i] + offset);
					}
					type = GL_UNSIGNED_INT;
					data = glIntIndexBuffer.data();
				}
			}
			
			if(m_renderer->hasDrawRangeElements()) {
				glDrawRangeElements(mode, offset, offset + count - 1, nbindices, type, data);
			} else {
				glDrawElements(mode, nbindices, type, data);
			}
			
		}
		
#endif
		
	}
	
	~BaseGLVertexBuffer() override {
		clearVertexArray(this);
		unbindBuffer(m_buffer);
		glDeleteBuffers(1, &m_buffer);
	}
	
protected:
	
	OpenGLRenderer * m_renderer;
	GLuint m_buffer;
	size_t m_offset;
	Renderer::BufferUsage m_usage;
	
};

template <class Vertex>
class GLVertexBuffer : public BaseGLVertexBuffer<Vertex> {
	
	typedef BaseGLVertexBuffer<Vertex> Base;
	
public:
	
	using Base::capacity;
	
	GLVertexBuffer(const GLVertexBuffer &) = delete;
	GLVertexBuffer & operator=(const GLVertexBuffer &) = delete;
	
	GLVertexBuffer(OpenGLRenderer * renderer, size_t size, Renderer::BufferUsage usage)
		: Base(renderer, size, usage)
		, m_initialized(false)
	{ }

    void setData(const Vertex * vertices, size_t count, size_t offset, BufferFlags flags) override {

        arx_assert(offset < capacity());
        arx_assert(offset + count <= capacity());

        bindBuffer(m_buffer);

        if(offset == 0 && count == capacity()) {
            initialize(vertices);
            return;
        }

        if(!m_initialized || (flags & DiscardBuffer)) {
            // Orphan only when the caller explicitly discards the old contents.
            initialize();
        }

        if(count != 0) {
            const size_t obytes = offset * sizeof(Vertex);
            const size_t nbytes = count * sizeof(Vertex);
            glBufferSubData(GL_ARRAY_BUFFER, obytes, nbytes, vertices);
        }
    }
	
protected:
	
	void initialize(const Vertex * data = nullptr) {
		arx_assert(!m_initialized || m_usage != Renderer::Static);
		arx_assert(data || m_usage != Renderer::Static);
#if GL_ARB_buffer_storage && !defined(ANDROID)
		if(m_usage == Renderer::Static && m_renderer->hasBufferStorage()) {
			glBufferStorage(GL_ARRAY_BUFFER, capacity() * sizeof(Vertex), data, 0);
		}
		else
		#endif
		{
			GLenum usage = m_usage == Renderer::Static ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW;
			if(m_usage == Renderer::Stream && m_renderer->hasBufferUsageStream()) {
				usage = GL_STREAM_DRAW;
			}
			glBufferData(GL_ARRAY_BUFFER, capacity() * sizeof(Vertex), data, usage);
		}
		m_initialized = true;
	}
	
	using Base::m_renderer;
	using Base::m_buffer;
	using Base::m_usage;
	bool m_initialized;
	
};

template <class Vertex>
class GLShadowVertexBuffer final : public GLVertexBuffer<Vertex> {
	
	typedef GLVertexBuffer<Vertex> Base;
	
public:
	
	using Base::capacity;
	
	GLShadowVertexBuffer(const GLShadowVertexBuffer &) = delete;
	GLShadowVertexBuffer & operator=(const GLShadowVertexBuffer &) = delete;
	
	GLShadowVertexBuffer(OpenGLRenderer * renderer, size_t size, Renderer::BufferUsage usage)
		: Base(renderer, size, usage)
		, m_lockedFlags(0)
		, m_lockedOffset(0)
		, m_lockedCount(0)
	{ }
	
	void setData(const Vertex * vertices, size_t count, size_t offset, BufferFlags flags) override {
	
#ifdef ANDROID
		if(drawsFromClientMemory()) {
			m_shadow.resize(capacity());
			if(count != 0) {
				std::memcpy(m_shadow.data() + offset, vertices, count * sizeof(Vertex));
			}
			return;
		}
#endif
		
		if(m_usage == Renderer::Dynamic) {
			m_shadow.resize(capacity());
			std::memcpy(m_shadow.data() + offset, vertices, count * sizeof(Vertex));
		}
		
		Base::setData(vertices, count, offset, flags);
	}
	
	Vertex * lock(BufferFlags flags, size_t offset, size_t count) override {
		
		m_shadow.resize(capacity());
		
		arx_assert(offset < capacity());
		
		m_lockedFlags = flags;
		m_lockedOffset = offset;
		m_lockedCount = std::min(count, capacity() - offset);
		
		return m_shadow.data() + offset;
	}
	
	void unlock() override {
#ifdef ANDROID
		if(drawsFromClientMemory()) {
			return;
		}
#endif
		Base::setData(m_shadow.data() + m_lockedOffset, m_lockedCount, m_lockedOffset, m_lockedFlags);
	}
	
#ifdef ANDROID
	
	/*
	 * Without buffer mapping (OpenGL ES 2.0 drivers lacking GL_EXT_map_buffer_range)
	 * streamed vertices are drawn straight from client memory. The driver copies them
	 * into fresh memory for every draw, whereas glBufferSubData() on a buffer that
	 * queued draws still read would make it copy the whole buffer or wait for the GPU.
	 */
	 
	void draw(Renderer::Primitive primitive, size_t count, size_t offset) const override {
		
		if(!drawsFromClientMemory()) {
			Base::draw(primitive, count, offset);
			return;
		}
		
		arx_assert(offset < capacity());
		arx_assert(offset + count <= capacity());
		
		m_renderer->template beforeDraw<Vertex>();
		m_shadow.resize(capacity());
		bindBuffer(GL_NONE);
		setVertexArray<Vertex>(m_renderer, m_shadow.data(), this);
		glDrawArrays(arxToGlPrimitiveType[primitive], GLint(offset), GLsizei(count));
		
	}
	
	void drawIndexed(Renderer::Primitive primitive, size_t count, size_t offset,
	                 const unsigned short * indices, size_t nbindices) const override {
	                 
		if(!drawsFromClientMemory()) {
			Base::drawIndexed(primitive, count, offset, indices, nbindices);
			return;
		}
		
		arx_assert(offset < capacity());
		arx_assert(offset + count <= capacity());
		arx_assert(indices != nullptr);
		
		m_renderer->template beforeDraw<Vertex>();
		m_shadow.resize(capacity());
		bindBuffer(GL_NONE);
		setVertexArray<Vertex>(m_renderer, m_shadow.data() + offset, this);
		const void * indexData = m_renderer->streamIndices(indices, nbindices);
		m_renderer->drawIndexedElements(primitive, indexData, nbindices, 0, count);
		
	}
	
#endif

protected:

#ifdef ANDROID
	bool drawsFromClientMemory() const { return m_usage == Renderer::Stream; }
	using Base::m_renderer;
#endif
	
	using Base::m_usage;
	using Base::m_initialized;
	
	mutable std::vector<Vertex> m_shadow;
	BufferFlags m_lockedFlags;
	size_t m_lockedOffset;
	size_t m_lockedCount;
	
};

template <class Vertex>
class GLMapVertexBuffer : public GLVertexBuffer<Vertex> {
	
	typedef GLVertexBuffer<Vertex> Base;
	
public:
	
	using Base::capacity;
	
	GLMapVertexBuffer(const GLMapVertexBuffer &) = delete;
	GLMapVertexBuffer & operator=(const GLMapVertexBuffer &) = delete;
	
	GLMapVertexBuffer(OpenGLRenderer * renderer, size_t size, Renderer::BufferUsage usage)
		: Base(renderer, size, usage)
	{ }
	
	Vertex * lock(BufferFlags flags, size_t offset, size_t count) override {
		ARX_UNUSED(count);
		
		arx_assert(offset < capacity());
		
		bindBuffer(m_buffer);
		
		if(!m_initialized || (flags & DiscardBuffer)) {
			// Orphan only on first use or when the caller discards the old contents.
			initialize();
		}
		
#ifdef ANDROID
		void * buf = glMapBufferOES(GL_ARRAY_BUFFER, GL_WRITE_ONLY_OES);
#else
		void * buf = glMapBuffer(GL_ARRAY_BUFFER, GL_WRITE_ONLY);
#endif
		if(!buf) {
			LogError << "Could not map vertex buffer!";
			return nullptr;
		}
		
		return reinterpret_cast<Vertex *>(buf) + offset;
	}
	
	void unlock() override {
		
		bindBuffer(m_buffer);
		
#ifdef ANDROID
		GLboolean ret = glUnmapBufferOES(GL_ARRAY_BUFFER);
#else
		GLboolean ret = glUnmapBuffer(GL_ARRAY_BUFFER);
#endif
		if(ret == GL_FALSE) {
			LogWarning << "Vertex buffer invalidated";
		}
		
	}
	
protected:
	
	using Base::initialize;
	
	using Base::m_buffer;
	using Base::m_initialized;
	
};

#ifdef ANDROID

/*!
 * Vertex buffer for OpenGL ES 3.0 or GL_EXT_map_buffer_range (native drivers and ANGLE).
 *
 * A range that draws already queued in this or the previous frames may still read
 * is never rewritten with glBufferSubData() or a synchronized map: ANGLE's Vulkan
 * backend has to end the current render pass for such an update (or copy the whole
 * buffer first), which is extremely slow on tiled GPUs, especially Mali.
 *  - DiscardBuffer (and the first use) orphans the storage with glBufferData(nullptr),
 *    so the driver hands out fresh memory and keeps the old one alive for queued draws.
 *  - NoOverwrite maps with GL_MAP_UNSYNCHRONIZED_BIT, matching the desktop renderer:
 *    the caller promises not to touch data that queued draws still use.
 */
template <class Vertex>
class GLESMapRangeVertexBuffer final : public GLVertexBuffer<Vertex> {
	
	typedef GLVertexBuffer<Vertex> Base;
	
public:
	
	using Base::capacity;
	
	GLESMapRangeVertexBuffer(const GLESMapRangeVertexBuffer &) = delete;
	GLESMapRangeVertexBuffer & operator=(const GLESMapRangeVertexBuffer &) = delete;
	
	GLESMapRangeVertexBuffer(OpenGLRenderer * renderer, size_t size, Renderer::BufferUsage usage)
		: Base(renderer, size, usage)
		, m_mapped(false)
	{ }
	
	void setData(const Vertex * vertices, size_t count, size_t offset, BufferFlags flags) override {
		
		arx_assert(offset < capacity());
		arx_assert(offset + count <= capacity());
		arx_assert(!m_mapped);
		
		bindBuffer(m_buffer);
		
		if(offset == 0 && count == capacity()) {
			// Respecifying the whole buffer with data never waits for the GPU.
			initialize(vertices);
			return;
		}
		
		if(count == 0) {
			if(!m_initialized || (flags & DiscardBuffer)) {
				initialize();
			}
			return;
		}
		
		// Only the written range is replaced, the rest of the buffer is kept.
		Vertex * dst = map(flags, offset, count);
		if(!dst) {
			glBufferSubData(GL_ARRAY_BUFFER, GLintptr(offset * sizeof(Vertex)),
			                GLsizeiptr(count * sizeof(Vertex)), vertices);
			return;
		}
		std::memcpy(dst, vertices, count * sizeof(Vertex));
		unmap();
		
	}
	
	Vertex * lock(BufferFlags flags, size_t offset, size_t count) override {
		
		arx_assert(offset < capacity());
		arx_assert(!m_mapped);
		
		bindBuffer(m_buffer);
		
		count = std::min(std::max(count, size_t(1)), capacity() - offset);
		
		return map(flags, offset, count);
	}
	
	void unlock() override {
		
		if(!m_mapped) {
			return;
		}
		
		bindBuffer(m_buffer);
		
		unmap();
		
	}
	
	~GLESMapRangeVertexBuffer() override {
		if(m_mapped) {
			bindBuffer(m_buffer);
			unmap();
		}
	}
	
private:
	
	Vertex * map(BufferFlags flags, size_t offset, size_t count) {
		
		// No GL_MAP_INVALIDATE_* flags: some Mali drivers create a resource ghost
		// (allocate and copy the buffer) for them. Discarding is done by orphaning
		// with glBufferData() instead.
		GLbitfield access = GL_MAP_WRITE_BIT;
		
		if(!m_initialized || (flags & DiscardBuffer)) {
			// The fresh storage is not used by any draw yet, so no synchronization is needed.
			initialize();
			access |= GL_MAP_UNSYNCHRONIZED_BIT;
		} else if(flags & NoOverwrite) {
			access |= GL_MAP_UNSYNCHRONIZED_BIT;
		}
		
		void * buf = m_renderer->mapBufferRange(GL_ARRAY_BUFFER, offset * sizeof(Vertex),
		                                        count * sizeof(Vertex), access);
		if(!buf) {
			LogError << "Could not map vertex buffer!";
			return nullptr;
		}
		
		m_mapped = true;
		
		return reinterpret_cast<Vertex *>(buf);
	}
	
	void unmap() {
		m_mapped = false;
		if(!m_renderer->unmapBuffer(GL_ARRAY_BUFFER)) {
			LogWarning << "Vertex buffer invalidated";
		}
	}
	
	using Base::initialize;
	
	using Base::m_renderer;
	using Base::m_buffer;
	using Base::m_initialized;
	
	bool m_mapped;
	
};

#endif // ANDROID

#ifndef ANDROID
template <class Vertex>
class GLMapRangeVertexBuffer final : public GLMapVertexBuffer<Vertex> {
	
	typedef GLMapVertexBuffer<Vertex> Base;
	
public:
	
	using Base::capacity;
	
	GLMapRangeVertexBuffer(const GLMapRangeVertexBuffer &) = delete;
	GLMapRangeVertexBuffer & operator=(const GLMapRangeVertexBuffer &) = delete;
	
	GLMapRangeVertexBuffer(OpenGLRenderer * renderer, size_t size,
	                       Renderer::BufferUsage usage)
		: Base(renderer, size, usage)
	{ }

    Vertex * lock(BufferFlags flags, size_t offset, size_t count) override {

        arx_assert(offset < capacity());

        bindBuffer(m_buffer);

        if(!m_initialized || (flags & DiscardBuffer)) {
            glBufferData(GL_ARRAY_BUFFER, capacity() * sizeof(Vertex), nullptr, GL_DYNAMIC_DRAW);
            m_initialized = true;
        }

        GLbitfield glflags = GL_MAP_WRITE_BIT;
        if(flags & DiscardBuffer) {
            glflags |= GL_MAP_INVALIDATE_BUFFER_BIT;
        }
        if(flags & DiscardRange) {
            glflags |= GL_MAP_INVALIDATE_RANGE_BIT;
        }
        if(flags & NoOverwrite) {
            glflags |= GL_MAP_UNSYNCHRONIZED_BIT;
        }

        size_t obytes = offset * sizeof(Vertex);
        size_t nbytes = std::min(count, capacity() - offset) * sizeof(Vertex);

        void * buf = glMapBufferRange(GL_ARRAY_BUFFER, obytes, nbytes, glflags);
        if(!buf) {
            LogError << "Could not map vertex buffer!";
            return nullptr;
        }

        return reinterpret_cast<Vertex *>(buf);
    }
	
protected:
	
	using Base::initialize;
	
	using Base::m_buffer;
	using Base::m_initialized;
	
};

#endif // !ANDROID

#if GL_ARB_buffer_storage && !defined(ANDROID)

template <class Vertex>
class BaseGLPersistentVertexBuffer : public BaseGLVertexBuffer<Vertex> {
	
	typedef BaseGLVertexBuffer<Vertex> Base;
	
public:
	
	using Base::capacity;
	
	BaseGLPersistentVertexBuffer(const BaseGLPersistentVertexBuffer &) = delete;
	BaseGLPersistentVertexBuffer & operator=(const BaseGLPersistentVertexBuffer &) = delete;
	
	BaseGLPersistentVertexBuffer(OpenGLRenderer * renderer, size_t size, Renderer::BufferUsage usage,
	                             size_t multiplier = 1)
		: Base(renderer, size, usage)
		, m_multiplier(multiplier)
		, m_mapping(nullptr)
	{
		
		bindBuffer(m_buffer);
		m_mapping = create();
		
	}
	
protected:
	
	size_t size() const { return capacity() * sizeof(Vertex) * m_multiplier; }
	
	Vertex * map(GLbitfield flags) const {
		flags |= GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
		return reinterpret_cast<Vertex *>(glMapBufferRange(GL_ARRAY_BUFFER, 0, size(), flags));
	}
	
	Vertex * create() const {
		
		arx_assert(m_usage != Renderer::Static);
		GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
		if(m_usage == Renderer::Stream) {
			flags |= GL_CLIENT_STORAGE_BIT;
		}
		glBufferStorage(GL_ARRAY_BUFFER, size(), nullptr, flags);
		
		return map(GL_MAP_UNSYNCHRONIZED_BIT);
	}
	
	virtual void discardBuffer() = 0;
	
	
	void syncPersistent(BufferFlags flags) {

		if(flags & DiscardBuffer) {
			discardBuffer();
			return;
		}

		if(flags & NoOverwrite) {
			return;
		}

		discardBuffer();
	}
	
public:
	
	void setData(const Vertex * vertices, size_t count, size_t offset, BufferFlags flags) override {
		
		arx_assert(offset < capacity());
		arx_assert(offset + count <= capacity());
		
		syncPersistent(flags);
		
		if(count > 0) {
			std::memcpy(m_mapping + m_offset + offset, vertices, count * sizeof(Vertex));
		}
		
	}
	
	Vertex * lock(BufferFlags flags, size_t offset, size_t count) override {
		ARX_UNUSED(count);
		
		arx_assert(offset < capacity());
		
		syncPersistent(flags);
		
		return m_mapping + m_offset + offset;
	}
	
	void unlock() override { }
	
	~BaseGLPersistentVertexBuffer() override {
		
		if(m_mapping) {
			bindBuffer(m_buffer);
			GLboolean ret = glUnmapBuffer(GL_ARRAY_BUFFER);
			if(ret == GL_FALSE) {
				LogWarning << "Persistently mapped vertex buffer invalidated";
			}
		}
		
	}
	
protected:
	
	using Base::m_buffer;
	using Base::m_offset;
	using Base::m_usage;
	
	size_t m_multiplier;
	
	Vertex * m_mapping;
	
};

template <class Vertex>
class GLPersistentUnsynchronizedVertexBuffer final : public BaseGLPersistentVertexBuffer<Vertex> {
	
	typedef BaseGLPersistentVertexBuffer<Vertex> Base;
	
public:
	
	GLPersistentUnsynchronizedVertexBuffer(const GLPersistentUnsynchronizedVertexBuffer &) = delete;
	GLPersistentUnsynchronizedVertexBuffer & operator=(const GLPersistentUnsynchronizedVertexBuffer &) = delete;
	
	GLPersistentUnsynchronizedVertexBuffer(OpenGLRenderer * renderer, size_t size,
	                                       Renderer::BufferUsage usage)
		: Base(renderer, size, usage)
	{ }
	
	void discardBuffer() override {
		// Assume the GL already rendered far enough
	}
	
protected:
	
	using Base::map;
	
	using Base::m_buffer;
	using Base::m_mapping;
	
};

template <class Vertex>
class GLPersistentOrphanVertexBuffer final : public BaseGLPersistentVertexBuffer<Vertex> {
	
	typedef BaseGLPersistentVertexBuffer<Vertex> Base;
	
public:
	
	GLPersistentOrphanVertexBuffer(const GLPersistentOrphanVertexBuffer &) = delete;
	GLPersistentOrphanVertexBuffer & operator=(const GLPersistentOrphanVertexBuffer &) = delete;
	
	GLPersistentOrphanVertexBuffer(OpenGLRenderer * renderer, size_t size, Renderer::BufferUsage usage)
		: Base(renderer, size, usage)
	{ }
	
	void discardBuffer() override {
		
		bindBuffer(m_buffer);
		glUnmapBuffer(GL_ARRAY_BUFFER);
		m_mapping = map(GL_MAP_INVALIDATE_BUFFER_BIT);
		
	}
	
protected:
	
	using Base::map;
	
	using Base::m_buffer;
	using Base::m_mapping;
	
};

template <class Vertex, size_t MaxFences>
class GLPersistentFenceVertexBuffer final : public BaseGLPersistentVertexBuffer<Vertex> {
	
	typedef BaseGLPersistentVertexBuffer<Vertex> Base;
	
public:
	
	using Base::capacity;
	
	GLPersistentFenceVertexBuffer(const GLPersistentFenceVertexBuffer &) = delete;
	GLPersistentFenceVertexBuffer & operator=(const GLPersistentFenceVertexBuffer &) = delete;
	
	GLPersistentFenceVertexBuffer(OpenGLRenderer * renderer, size_t size, Renderer::BufferUsage usage,
	                              size_t fenceCount)
		: Base(renderer, size, usage, fenceCount)
		, m_position(0)
	{
		
		arx_assert(m_multiplier <= MaxFences);
		for(size_t i = 0; i < m_multiplier; i++) {
			m_fences[i] = 0;
		}
		
	}
	
	void discardBuffer() override {
		
		// Create a fence for the current buffer
		arx_assert(!m_fences[m_position]);
		m_fences[m_position] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		
		// Switch to the next buffer and wait for its fence
		m_position = (m_position + 1) % m_multiplier;
		if(m_fences[m_position]) {
			GLenum ret = GL_UNSIGNALED;
			while(ret != GL_ALREADY_SIGNALED && ret != GL_CONDITION_SATISFIED) {
				ret = glClientWaitSync(m_fences[m_position], GL_SYNC_FLUSH_COMMANDS_BIT, 1);
			}
			glDeleteSync(m_fences[m_position]);
			m_fences[m_position] = 0;
		}
		
		m_offset = m_position * capacity();
		
	}
	
	~GLPersistentFenceVertexBuffer() override {
		
		for(size_t i = 0; i < m_multiplier; i++) {
			if(m_fences[i]) {
				glDeleteSync(m_fences[i]);
			}
		}
		
	}
	
protected:
	
	using Base::m_offset;
	using Base::m_multiplier;
	
	size_t m_position;
	GLsync m_fences[MaxFences];
	
};

#endif // GL_ARB_buffer_storage

#endif // ARX_GRAPHICS_OPENGL_GLVERTEXBUFFER_H
