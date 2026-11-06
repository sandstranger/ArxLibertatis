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
	void drawIndexedElements(Primitive primitive, const unsigned short * indices, size_t nindices);
	
	bool getSnapshot(Image & image) override;
	bool getSnapshot(Image & image, size_t width, size_t height) override;
	
	GLTextureStage * GetTextureStage(size_t textureStage) {
		return reinterpret_cast<GLTextureStage *>(Renderer::GetTextureStage(textureStage));
	}
	
	template <class Vertex>
	void beforeDraw() {
		flushState();
		selectTrasform<Vertex>();
#ifdef ANDROID
		prepareGLESProgram(m_currentTransform == GL_ModelViewProjectionTransform);
#endif
	}
	
	bool hasTextureNPOT() const { return m_hasTextureNPOT; }
	bool hasSizedTextureFormats() const { return m_hasSizedTextureFormats; }
	bool hasIntensityTextures() const { return m_hasIntensityTextures; }
	bool hasBGRTextureTransfer() const { return m_hasBGRTextureTransfer; }
	
	bool hasMapBuffer() const { return m_hasMapBuffer; }
	bool hasMapBufferRange() const { return m_hasMapBufferRange; }
	bool hasBufferStorage() const { return m_hasBufferStorage; }
	bool hasBufferUsageStream() const { return m_hasBufferUsageStream; }
	bool hasDrawRangeElements() const { return m_hasDrawRangeElements; }
	bool hasDrawElementsBaseVertex() const { return m_hasDrawElementsBaseVertex; }
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
	void prepareGLESProgram(bool transformed);

	GLuint m_glesProgram = 0;
	GLuint m_glesImmediateVertexBuffer = 0;
	GLuint m_glesImmediateIndexBuffer = 0;
	bool m_glesProgramBound = false;
	bool m_glesUniformCacheValid = false;
	glm::mat4 m_glesCachedProjection{1.f};
	glm::mat4 m_glesCachedView{1.f};
	GLint m_glesCachedTransform = -1;
	GLint m_glesCachedFogEnabled = -1;
	float m_glesCachedFogColor[4] = {0.f, 0.f, 0.f, 0.f};
	float m_glesCachedFogStart = 0.f;
	float m_glesCachedFogEnd = 0.f;
	GLint m_glesCachedAlphaCutout = -1;
	float m_glesCachedAlphaThreshold = 0.f;
	float m_glesCachedGamma = 0.f;
	GLint m_glesCachedUseTexture[3] = {-1, -1, -1};
	GLint m_glesCachedColorOp[3] = {-1, -1, -1};
	GLint m_glesCachedAlphaOp[3] = {-1, -1, -1};
	float m_glesCachedLodBias[3] = {0.f, 0.f, 0.f};
	GLint m_glesUniformProjection = -1;
	GLint m_glesUniformView = -1;
	GLint m_glesUniformTransform = -1;
	GLint m_glesUniformFogEnabled = -1;
	GLint m_glesUniformFogColor = -1;
	GLint m_glesUniformFogRange = -1;
	GLint m_glesUniformAlphaCutout = -1;
	GLint m_glesUniformAlphaThreshold = -1;
	GLint m_glesUniformGamma = -1;
	GLint m_glesUniformLodBias[3] = {-1, -1, -1};
	GLint m_glesUniformUseTexture[3] = {-1, -1, -1};
	GLint m_glesUniformColorOp[3] = {-1, -1, -1};
	GLint m_glesUniformAlphaOp[3] = {-1, -1, -1};
	Color m_glesFogColor;
	float m_glesGamma = 1.f;
	float m_glesFogStart = 0.f;
	float m_glesFogEnd = 1.f;
	bool m_glesAlphaCutout = false;
	float m_glesAlphaThreshold = 0.5f;
#endif
	
};

template <class Vertex>
void OpenGLRenderer::selectTrasform() { enableTransform(); }

template <>
inline void OpenGLRenderer::selectTrasform<TexturedVertex>() { disableTransform(); }

#endif // ARX_GRAPHICS_OPENGL_OPENGLRENDERER_H
