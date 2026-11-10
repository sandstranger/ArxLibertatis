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

#ifndef ARX_GRAPHICS_OPENGL_OPENGLRENDERER_H
#define ARX_GRAPHICS_OPENGL_OPENGLRENDERER_H

#include <memory>
#include <unordered_map>
#include <vector>

#include <boost/intrusive/list.hpp>

#include "graphics/Renderer.h"
#include "graphics/opengl/GLTexture.h"
#include "graphics/opengl/OpenGLUtil.h"
#include "math/Rectangle.h"
#include "platform/Platform.h"

class GLTextureStage;

class OpenGLRenderer final : public Renderer {
	
public:
	
	OpenGLRenderer();
	~OpenGLRenderer() override;
	
	void initialize() override;
	
	void beforeResize(bool wasOrIsFullscreen) override;
	void afterResize() override;
	
	// Matrices
	void SetViewMatrix(const glm::mat4x4 & matView) override;
	void SetProjectionMatrix(const glm::mat4x4 & matProj) override;
	
	// Texture management
	void ReleaseAllTextures() override;
	void RestoreAllTextures() override;
	void reloadColorKeyTextures() override;
	
	// Factory
	Texture * createTexture() override;
	
	// Viewport
	void SetViewport(const Rect & viewport) override;
	
	void SetScissor(const Rect & rect) override;
	
	// Render Target
	void Clear(BufferFlags bufferFlags, Color clearColor = Color(), float clearDepth = 1.f,
	           size_t nrects = 0, Rect * rect = 0) override;
	
	// Fog
	void SetFogColor(Color color) override;
	void SetFogParams(float fogStart, float fogEnd) override;
	void setGamma(float gamma) override;
	
	// Rasterizer
	void SetAntialiasing(bool enable) override;
	void SetFillMode(FillMode mode) override;
	
	float getMaxAnisotropy() const { return m_maximumAnisotropy; }
	float getMaxSupportedAnisotropy() const override { return m_maximumSupportedAnisotropy; }
	void setMaxAnisotropy(float value) override;
	
	AlphaCutoutAntialising getMaxSupportedAlphaCutoutAntialiasing() const override;
	
	std::unique_ptr<VertexBuffer<TexturedVertex>> createVertexBufferTL(size_t capacity, BufferUsage usage) override;
	std::unique_ptr<VertexBuffer<SMY_VERTEX>> createVertexBuffer(size_t capacity, BufferUsage usage) override;
	std::unique_ptr<VertexBuffer<SMY_VERTEX3>> createVertexBuffer3(size_t capacity, BufferUsage usage) override;
	
	void drawIndexed(Primitive primitive, const TexturedVertex * vertices, size_t nvertices,
	                 unsigned short * indices, size_t nindices) override;
#ifdef ANDROID
	/*!
	 * Copy indices for one draw into the streaming element array buffer.
	 *
	 * \return the value to pass as the indices pointer of the draw call: an offset into
	 *         the bound element array buffer, or the client pointer itself when buffer
	 *         mapping is not available (the element array buffer is unbound then).
	 */
	const void * streamIndices(const unsigned short * indices, size_t count);

	/*!
	 * Issue an indexed draw with indices returned by streamIndices().
	 * Every index must be smaller than vertexCount, which is passed to glDrawRangeElements*()
	 * so the driver does not have to scan the indices for their range.
	 */
	void drawIndexedElements(Primitive primitive, const void * indices, size_t nindices,
	                         size_t baseVertex, size_t vertexCount);

	void uploadIndexRanges(const IndexRange * ranges, size_t count) override;
	
	void endFrame() override;
	void beginFrame() override;
	
	// Dynamic lighting in the vertex shader (OpenGL ES 3.0 feature level)
	size_t getMaxGPULightSetSize() const override;
	size_t getMaxGPULights() const override { return m_glesLighting ? m_glesMaxLights : 0; }
	size_t getMaxGPULightSets() const override { return m_glesLighting ? m_glesMaxLightSets : 0; }
	void resetGPULightSets() override;
	u32 addGPULightSet(const GPULightSet & set, const GPULight * lights, size_t count) override;
	void setGPULightSet(u32 set) override;
	void drawLitTriangles(const TexturedVertex * vertices, const TexturedVertexLighting * lighting,
	                      size_t count) override;
	
	//! The lighting programs are available
	bool hasGLESLighting() const { return m_glesLighting; }
#endif

	bool getSnapshot(Image & image) override;
	bool getSnapshot(Image & image, size_t width, size_t height) override;

	GLTextureStage * GetTextureStage(size_t textureStage) {
		return reinterpret_cast<GLTextureStage *>(Renderer::GetTextureStage(textureStage));
	}

	template <class Vertex>
	void beforeDraw() {
#ifdef ANDROID
		if(!m_glesPassStarted) {
			startRenderPass();
		}
#endif
		flushState();
		selectTrasform<Vertex>();
#ifdef ANDROID
		prepareGLESProgram(m_currentTransform == GL_ModelViewProjectionTransform, isGLESLitDraw<Vertex>());
#endif
	}
	
	bool hasTextureNPOT() const { return m_hasTextureNPOT; }
	bool hasSizedTextureFormats() const { return m_hasSizedTextureFormats; }
	bool hasIntensityTextures() const { return m_hasIntensityTextures; }
	bool hasBGRTextureTransfer() const { return m_hasBGRTextureTransfer; }
	
	bool hasMapBuffer() const { return m_hasMapBuffer; }
	bool hasMapBufferRange() const { return m_hasMapBufferRange; }
#ifdef ANDROID
	//! glMapBufferRange() from OpenGL ES 3.0 or GL_EXT_map_buffer_range on OpenGL ES 2.0
	void * mapBufferRange(GLenum target, size_t offset, size_t length, GLbitfield access) const {
		if(m_hasCoreMapBufferRange) {
			return glMapBufferRange(target, GLintptr(offset), GLsizeiptr(length), access);
		}
		return glMapBufferRangeEXT(target, GLintptr(offset), GLsizeiptr(length), access);
	}
	bool unmapBuffer(GLenum target) const {
		GLboolean result = m_hasCoreMapBufferRange ? glUnmapBuffer(target) : glUnmapBufferOES(target);
		return result != GL_FALSE;
	}
#endif
	bool hasBufferStorage() const { return m_hasBufferStorage; }
	bool hasBufferUsageStream() const { return m_hasBufferUsageStream; }
	bool hasDrawRangeElements() const { return m_hasDrawRangeElements; }
	bool hasDrawElementsBaseVertex() const {
#ifdef ANDROID
		return m_hasDrawElementsBaseVertex
		       && (glDrawElementsBaseVertexOES != nullptr || glDrawElementsBaseVertexEXT != nullptr);
#else
		return m_hasDrawElementsBaseVertex;
#endif
	}
	bool hasClearDepthf() const { return m_hasClearDepthf; }
	bool hasVertexFogCoordinate() const { return m_hasVertexFogCoordinate; }
	bool hasSampleShading() const { return m_hasSampleShading; }
	
private:
	
	void shutdown();
	void reinit();
	
	Rect viewport;
	
	void flushState();
	
	template <class Vertex>
	void selectTrasform();
	
	void enableTransform();
	void disableTransform();
	
	friend class GLTextureStage;
	
	size_t maxTextureStage; // the highest active texture stage
	
	float m_maximumAnisotropy;
	float m_maximumSupportedAnisotropy;
	
	typedef boost::intrusive::list<GLTexture, boost::intrusive::constant_time_size<false>> TextureList;
	TextureList textures;
	
	RenderState m_glstate;
	bool m_glsampleShading;
	bool m_glalphaToCoverage;
	float m_glalphaFunc;
	GLenum m_glblendSrc;
	GLenum m_glblendDst;
	
	Rect m_scissor;
	
	int m_MSAALevel;
	bool m_hasMSAA;
	
	bool m_hasTextureNPOT;
	bool m_hasSizedTextureFormats;
	bool m_hasIntensityTextures;
	bool m_hasBGRTextureTransfer;
	
	bool m_hasMapBuffer;
	bool m_hasMapBufferRange;
	bool m_hasBufferStorage;
	bool m_hasBufferUsageStream;
	bool m_hasDrawRangeElements;
	bool m_hasDrawElementsBaseVertex;
	bool m_hasClearDepthf;
	bool m_hasVertexFogCoordinate;
	bool m_hasSampleShading;
	bool m_hasFogx;
	bool m_hasFogDistanceMode;
	
	enum GLTransformMode {
		GL_UnsetTransform,
		GL_NoTransform,
		GL_ModelViewProjectionTransform
	};
	
 	GLTransformMode m_currentTransform;
	glm::mat4x4 m_projection;
	glm::mat4x4 m_view;

#ifdef ANDROID
	bool initializeGLESProgram();
	void shutdownGLESProgram();
	void prepareGLESProgram(bool transformed, bool lit);
	
	//! Whether the next draw of this vertex type is lit by the GPU
	template <class Vertex>
	bool isGLESLitDraw() const;
	
	bool setupGLESLightingProgram(GLuint program);
	bool createGLESLightingBuffers();
	void shutdownGLESLighting();
	void uploadGLESLighting();
	void streamGLESVertices(GLuint buffer, size_t bufferSize, size_t offset, const void * data, size_t size);

	// Tile-based render pass management (see beginFrame()/endFrame())
	void startRenderPass();
	void invalidateAttachments(bool defaultFramebuffer, bool color, bool depth);
	bool updateMultisampleFramebuffer();
	bool createMultisampleResolveFramebuffer();
	void destroyMultisampleFramebuffer();
	void resolveMultisampleFramebuffer();
	bool blitMultisampleColor();
	void copyMultisampleColor();

	/*!
	 * One linked program per shader variant.
	 *
	 * Everything that would otherwise be uniform control flow is compiled in only where
	 * it is used: the number of texture stages, alpha cutout (discard), fog, the
	 * brightness gamma curve and dynamic lighting in the vertex shader.
	 * A shader that may discard turns off early depth/stencil updates and forward pixel
	 * kill on Mali for every draw that uses it, so opaque geometry must not share a
	 * program with alpha-tested geometry.
	 * Each program keeps its own uniform values, so the caches are per program.
	 */
	struct GLESProgram {
		GLuint id = 0;
		bool cacheValid = false;
		GLint uniformMVP = -1;
		GLint uniformFogPlane = -1;
		GLint uniformFogParams = -1;
		GLint uniformFogColor = -1;
		GLint uniformAlphaThreshold = -1;
		GLint uniformInvGamma = -1;
		GLint uniformGammaInverted = -1;
		GLint uniformStageA[3] = { -1, -1, -1 };
		GLint uniformStageB[3] = { -1, -1, -1 };
		GLint uniformStageC[3] = { -1, -1, -1 };
		GLint uniformLodBias[3] = { -1, -1, -1 };
		glm::mat4 mvp{1.f};
		glm::vec4 fogPlane{0.f};
		float fogParams[2] = { 0.f, 0.f };
		float fogColor[3] = { 0.f, 0.f, 0.f };
		float alphaThreshold = 0.f;
		float invGamma = 0.f;
		float gammaInverted = 0.f;
		float stageA[3][4] = {};
		float stageB[3][4] = {};
		float stageC[3][4] = {};
		float lodBias[3] = { 0.f, 0.f, 0.f };
	};
	
	enum GLESProgramVariant {
		GLESProgramAlphaCutout = 1 << 0,
		GLESProgramFog = 1 << 1,
		GLESProgramGamma = 1 << 2,
		GLESProgramLighting = 1 << 3, // OpenGL ES 3.0 feature level only
		GLESProgramStageBucketShift = 4,
		GLESProgramStageBucketCount = 3, // 0, 1 or 3 texture stages
		GLESProgramVariantCount = GLESProgramStageBucketCount << GLESProgramStageBucketShift
	};
	
	GLESProgram m_glesPrograms[GLESProgramVariantCount];
	GLuint m_glesBoundProgram = 0;
	bool m_hasCoreMapBufferRange = false;
	std::vector<GLint> m_glesProgramBinaryFormats;
	bool m_hasInvalidateFramebuffer = false;
	bool m_hasDiscardFramebuffer = false;
	bool m_glesBlendEnabled = false;
	
	// Streaming element array buffer (written with unsynchronized maps, orphaned on wrap)
	GLuint m_glesIndexBuffer = 0;
	size_t m_glesIndexBufferCapacity = 0;
	size_t m_glesIndexBufferOffset = 0;
	
	//! Where uploadIndexRanges() put a range in m_glesBatchIndexBuffer
	struct GLESIndexRange {
		size_t count;
		size_t offset; //!< in bytes
	};
	
	// Element array buffer for uploadIndexRanges(), respecified with every upload
	GLuint m_glesBatchIndexBuffer = 0;
	std::unordered_map<const unsigned short *, GLESIndexRange> m_glesIndexRanges;
	std::vector<unsigned short> m_glesBatchIndices; //!< Staging copy without buffer mapping
	
	// Streaming vertex buffer for drawIndexed() with client vertices
	std::unique_ptr<VertexBuffer<TexturedVertex>> m_glesImmediateVertices;
	size_t m_glesImmediateVertexOffset = 0;
	
	/*
	 * Render pass state for the current frame: which attachments the frame cleared
	 * before its first draw, and whether that first draw happened already.
	 */
	bool m_glesPassStarted = false;
	GLbitfield m_glesPassCleared = 0;
	
	// MSAA framebuffer: GL_EXT_multisampled_render_to_texture color texture + depth
	bool m_glesIsES3 = false;
	GLenum m_glesDepthFormat = GL_DEPTH_COMPONENT16;
	int m_glesMaxMSAASamples = 0;
	GLsizei m_glesMSAASamples = 0;
	GLuint m_glesMSAAFramebuffer = 0;
	GLuint m_glesMSAAColor = 0;
	GLuint m_glesMSAAColorBuffer = 0;
	bool m_glesMSAAResolveBlit = false;
	bool m_glesMSAABlitANGLE = false;
	GLenum m_glesWindowColorFormat = GL_RGBA8;
	GLuint m_glesMSAADepth = 0;
	Vec2i m_glesMSAASize = Vec2i(0);
	bool m_glesMSAAFailed = false;
	
	// Single-sampled framebuffer around m_glesMSAAColor, blitted into the window (no storage)
	GLuint m_glesMSAAResolveFramebuffer = 0;
	bool m_glesMSAAColorBlitVerified = false; //!< A blit into the window gave no GL error
	bool m_glesMSAAColorBlitFailed = false;   //!< Use the copy program for this context
	
	// Program and vertices for copying the resolved MSAA image to the window
	GLuint m_glesCopyProgram = 0;
	GLuint m_glesCopyVertices = 0;
	
	Color m_glesFogColor;
	float m_glesGamma = 1.f;
	float m_glesFogStart = 0.f;
	float m_glesFogEnd = 1.f;
	bool m_glesAlphaCutout = false;
	float m_glesAlphaThreshold = 0.5f;
	
	// Transforms for prepareGLESProgram(), only recomputed when their inputs change
	glm::mat4 m_glesWorldTransform{1.f};
	glm::vec4 m_glesWorldFogPlane{0.f};
	bool m_glesWorldTransformValid = false;
	glm::mat4 m_glesScreenTransform{1.f};
	Vec2i m_glesScreenTransformSize = Vec2i(0);
	
	/*
	 * Dynamic lighting in the vertex shader, see docs/gles3-gpu-lighting.md
	 *
	 * Light sets are appended to the ArxLights and ArxLightSets uniform buffers until
	 * resetGPULightSets(), which orphans them for the next uploads.
	 */
	
	//! std140 layout of ArxLight
	struct GLESLight {
		float position[4]; //!< xyz: position, w: fall start
		float color[4];    //!< rgb: color (0-255, intensity applied), w: fall end
		float falloff[4];  //!< x: 1 / (fall end - fall start)
	};
	
	//! std140 layout of ArxLightSet
	struct GLESLightSet {
		float ambient[4];  //!< rgb: ambient (0-255), w: weight of the vertex color
		float factor[4];   //!< rgb: color factor, w: diffuse factor
		float term[4];     //!< rgb: added color (0-255), w: number of lights
		u32 lights[8];     //!< indices into ArxLights, 8 bits each
	};
	
	bool m_glesLightingSupported = false; //!< The device limits allow lighting programs
	bool m_glesLighting = false;          //!< Lighting programs and buffers exist
	size_t m_glesMaxLights = 0;
	size_t m_glesMaxLightSets = 0;
	std::vector<GLESLight> m_glesLights;
	std::vector<GLESLightSet> m_glesLightSets;
	std::unordered_map<const void *, u32> m_glesLightKeys;
	size_t m_glesUploadedLights = 0;
	size_t m_glesUploadedLightSets = 0;
	bool m_glesOrphanLightBuffers = true;
	GLuint m_glesLightBuffer = 0;
	GLuint m_glesLightSetBuffer = 0;
	size_t m_glesLightBufferSize = 0;
	size_t m_glesLightSetBufferSize = 0;
	
	//! Light set of SMY_VERTEX draws (setGPULightSet())
	u32 m_glesRoomLightSet = InvalidGPULightSet;
	//! Current value of the aLightSet attribute while its array is disabled
	u32 m_glesLightSetAttribute = InvalidGPULightSet;
	//! drawLitTriangles() is drawing
	bool m_glesLitDraw = false;
	
	// Streaming vertex buffers for drawLitTriangles(), written in lockstep
	GLuint m_glesLitVertices = 0;
	GLuint m_glesLitLighting = 0;
	size_t m_glesLitOffset = 0;
#endif
	
};

template <class Vertex>
void OpenGLRenderer::selectTrasform() { enableTransform(); }

template <>
inline void OpenGLRenderer::selectTrasform<TexturedVertex>() { disableTransform(); }

#ifdef ANDROID

template <class Vertex>
bool OpenGLRenderer::isGLESLitDraw() const { return false; }

template <>
inline bool OpenGLRenderer::isGLESLitDraw<TexturedVertex>() const { return m_glesLitDraw; }

template <>
inline bool OpenGLRenderer::isGLESLitDraw<SMY_VERTEX>() const { return m_glesRoomLightSet != InvalidGPULightSet; }

#endif

#endif // ARX_GRAPHICS_OPENGL_OPENGLRENDERER_H
