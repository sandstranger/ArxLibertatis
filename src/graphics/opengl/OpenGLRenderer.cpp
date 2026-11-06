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

#include "graphics/opengl/OpenGLRenderer.h"

#include <cstring>
#include <algorithm>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>

#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "platform/Platform.h"
#include "Configure.h"

#if ARX_HAVE_EPOXY && ARX_PLATFORM == ARX_PLATFORM_WIN32
#include <epoxy/wgl.h>
#endif

#include <boost/algorithm/string/predicate.hpp>

#include "core/Application.h"
#include "core/Config.h"
#include "gui/Credits.h"
#include "graphics/opengl/GLDebug.h"
#include "graphics/opengl/GLTexture.h"
#include "graphics/opengl/GLTextureStage.h"
#include "graphics/opengl/GLVertexBuffer.h"
#include "io/log/Logger.h"
#include "platform/CrashHandler.h"
#include "window/RenderWindow.h"

#ifdef ANDROID
extern bool g_useGLES2_0;
#endif


OpenGLRenderer::OpenGLRenderer()
	: maxTextureStage(0)
	, m_maximumAnisotropy(1.f)
	, m_maximumSupportedAnisotropy(1.f)
	, m_glsampleShading(false)
	, m_glalphaToCoverage(false)
	, m_glalphaFunc(0.f)
	, m_glblendSrc(GL_ONE)
	, m_glblendDst(GL_ZERO)
	, m_MSAALevel(0)
	, m_hasMSAA(false)
	, m_hasTextureNPOT(false)
	, m_hasSizedTextureFormats(false)
	, m_hasIntensityTextures(false)
	, m_hasBGRTextureTransfer(false)
	, m_hasMapBuffer(false)
	, m_hasMapBufferRange(false)
	, m_hasBufferStorage(false)
	, m_hasBufferUsageStream(false)
	, m_hasDrawRangeElements(false)
	, m_hasDrawElementsBaseVertex(false)
	, m_hasClearDepthf(false)
	, m_hasVertexFogCoordinate(false)
	, m_hasSampleShading(false)
	, m_hasFogx(false)
	, m_hasFogDistanceMode(false)
	, m_currentTransform(GL_UnsetTransform)
	, m_projection(1.f)
	, m_view(1.f)
{ }

OpenGLRenderer::~OpenGLRenderer() {
	
	if(isInitialized()) {
		shutdown();
	}
	
	// TODO textures must be destructed before OpenGLRenderer or not at all
	
}

void OpenGLRenderer::initialize() {
	
	#if !defined(ANDROID) && ARX_HAVE_EPOXY
	
	#if ARX_PLATFORM == ARX_PLATFORM_WIN32
	epoxy_handle_external_wglMakeCurrent();
	#endif
	
	#elif !defined(ANDROID) && ARX_HAVE_GLEW
	
	if(glewInit() != GLEW_OK) {
		LogError << "GLEW init failed";
		return;
	}
	
	const GLubyte * glewVersion = glewGetString(GLEW_VERSION);
	LogInfo << "Using GLEW " << glewVersion;
	CrashHandler::setVariable("GLEW version", glewVersion);
	
	#endif
	
	OpenGLInfo gl;
#ifdef ANDROID
	// SDL may return a lower compatible context than requested on some devices.
	// Keep the shader and texture upload paths aligned with the actual context.
	if(gl.isES() && !gl.is(3, 0)) {
		g_useGLES2_0 = true;
	}
#endif
	
	LogInfo << "Using OpenGL " << gl.versionString();
	CrashHandler::setVariable("OpenGL version", gl.versionString());
	
	LogInfo << " ├─ Vendor: " << gl.vendor();
	CrashHandler::setVariable("OpenGL vendor", gl.vendor());
	
	LogInfo << " ├─ Device: " << gl.renderer();
	CrashHandler::setVariable("OpenGL device", gl.renderer());
	
	#if !defined(ANDROID) && (defined(GL_CONTEXT_FLAG_DEBUG_BIT) || defined(GL_CONTEXT_FLAG_NO_ERROR_BIT))
	if(gl.isES() ? gl.is(3, 2) : gl.is(3, 0)) {
		GLint flags = 0;
		glGetIntegerv(GL_CONTEXT_FLAGS, &flags);
		#ifdef GL_CONTEXT_FLAG_DEBUG_BIT
		if(flags & GL_CONTEXT_FLAG_DEBUG_BIT) {
			LogInfo << " ├─ Context type: debug";
		}
		#endif
		#ifdef GL_CONTEXT_FLAG_NO_ERROR_BIT
		if(flags & GL_CONTEXT_FLAG_NO_ERROR_BIT) {
			LogInfo << " ├─ Context type: no error";
		}
		#endif
	}
	#endif
	
	u64 totalVRAM = 0, freeVRAM = 0;
	{
		#ifdef GL_NVX_gpu_memory_info
		if(gl.has("GL_NVX_gpu_memory_info")) {
			// Implemented by the NVIDIA blob and radeon drivers in newer Mesa
			GLint tmp = 0;
			glGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &tmp);
			totalVRAM = u64(tmp) * 1024;
			glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &tmp);
			freeVRAM = u64(tmp) * 1024;
		}
		#endif
		#if defined(GL_NVX_gpu_memory_info) && defined(GL_ATI_meminfo)
		else
		#endif
		#ifdef GL_ATI_meminfo
		if(gl.has("GL_ATI_meminfo")) {
			// Implemented by the AMD blob and radeon drivers in newer Mesa
			GLint info[4];
			glGetIntegerv(GL_VBO_FREE_MEMORY_ATI, info);
			freeVRAM = u64(info[0]) * 1024;
			glGetIntegerv(GL_TEXTURE_FREE_MEMORY_ATI, info);
			freeVRAM = std::max(freeVRAM, u64(info[0]) * 1024);
		}
		#endif
		/*
		 * There is also GLX_MESA_query_renderer but being a GLX extension it's too
		 * annoying to use here.
		 */
	}
	{
		std::ostringstream oss;
		if(totalVRAM == 0 && freeVRAM == 0) {
			oss << "(unknown)";
		} else {
			if(totalVRAM != 0) {
				oss << (totalVRAM / 1024 / 1024) << " MiB";
				CrashHandler::setVariable("VRAM size", totalVRAM);
			}
			if(totalVRAM != 0 && freeVRAM != 0) {
				oss << ", ";
			}
			if(freeVRAM != 0) {
				oss << (freeVRAM / 1024 / 1024) << " MiB free";
				CrashHandler::setVariable("VRAM available", freeVRAM);
			}
		}
		LogInfo << " └─ VRAM: " << oss.str();
	}
	
	if(boost::starts_with(gl.versionString(), "ES-CL ")) {
		LogError << "OpenGL ES common lite profile detected but arx requires floating point functionality";
	}
	
	{
		std::ostringstream oss;
		#if ARX_HAVE_EPOXY
		oss << "libepoxy\n";
		#elif ARX_HAVE_GLEW
		oss << "GLEW " << glewVersion << '\n';
		#endif
		const char * start = gl.versionString();
		while(*start == ' ') {
			start++;
		}
		const char * end = start;
		while(*end != '\0' && *end != ' ') {
			end++;
		}
		oss << "OpenGL ";
		oss.write(start, end - start);
		credits::setLibraryCredits("graphics", oss.str());
	}
	
	gldebug::initialize(gl);
	
	if(gl.isES()) {
		if(!gl.is(2, 0)) {
			LogError << "OpenGL ES 2.0 or newer required";
		}
	} else {
		#if ARX_HAVE_EPOXY
		if(!gl.is(1, 4) || !gl.has("GL_ARB_vertex_buffer_object", 1, 5)) {
			LogError << "OpenGL version 1.5 or newer or 1.4 + GL_ARB_vertex_buffer_object required";
		}
		#else
		if(!gl.is(1, 5)) {
			LogError << "OpenGL version 1.5 or newer required";
		}
		#endif
	}
	
	if(gl.isES()) {
		m_hasTextureNPOT = gl.is(3, 0) || gl.has("GL_OES_texture_npot");
		if(!m_hasTextureNPOT) {
			LogWarning << "Missing OpenGL extension GL_OES_texture_npot";
		}
		m_hasSizedTextureFormats = gl.has("GL_OES_required_internalformat", 3, 0);
		m_hasIntensityTextures = false;
		m_hasBGRTextureTransfer = false;
	} else {
		m_hasTextureNPOT = gl.has("GL_ARB_texture_non_power_of_two", 2, 0);
		if(!m_hasTextureNPOT) {
			LogWarning << "Missing OpenGL extension GL_ARB_texture_non_power_of_two";
		}
		m_hasSizedTextureFormats = true;
#ifndef ANDROID       
		m_hasIntensityTextures = true;
#endif        
		m_hasBGRTextureTransfer = true;
	}
	
	// GL_EXT_texture_filter_anisotropic is available for both OpenGL ES and desktop OpenGL
	if(gl.has("GL_ARB_texture_filter_anisotropic", 4, 6) || gl.has("GL_EXT_texture_filter_anisotropic")) {
		GLfloat limit;
		glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &limit);
		m_maximumSupportedAnisotropy = limit;
		setMaxAnisotropy(float(config.video.maxAnisotropicFiltering));
	} else {
		m_maximumSupportedAnisotropy = 1.f;
	}
	
	if(gl.isES()) {
		// OES_draw_elements_base_vertex and EXT_draw_elements_base_vertex
		// are useful only when GLAD resolved their entry points from the
		// active EGL implementation (ANGLE or the native GLES driver).
		const bool hasBaseVertexExtension =
		        gl.has("GL_OES_draw_elements_base_vertex", 3, 2)
		        || gl.has("GL_EXT_draw_elements_base_vertex");
#ifdef ANDROID
		m_hasDrawElementsBaseVertex = hasBaseVertexExtension
		        && (glDrawElementsBaseVertexOES != nullptr
		            || glDrawElementsBaseVertexEXT != nullptr);
		if(hasBaseVertexExtension && !m_hasDrawElementsBaseVertex) {
			LogWarning << "Base-vertex extension advertised without a usable GLES entry point";
		}
#else
		m_hasDrawElementsBaseVertex = hasBaseVertexExtension;
#endif
		m_hasDrawRangeElements = gl.is(3, 0);
#ifdef ANDROID
		if(m_hasDrawElementsBaseVertex) {
			LogInfo << "GLES indexed draws: client indices + "
			        << (glDrawElementsBaseVertexOES ? "GL_OES_draw_elements_base_vertex" :
			                                            "GL_EXT_draw_elements_base_vertex");
		} else {
			LogInfo << "GLES indexed draws: client indices + shifted vertex attributes"
			        << " (no usable base-vertex entry point)";
		}
#endif
	} else {
#ifndef ANDROID       
		m_hasDrawElementsBaseVertex = gl.has("GL_ARB_draw_elements_base_vertex", 3, 2);
		if(!m_hasDrawElementsBaseVertex) {
			LogWarning << "Missing OpenGL extension GL_ARB_draw_elements_base_vertex";
		}
#else
        m_hasDrawElementsBaseVertex = gl.has("GL_ARB_draw_elements_base_vertex");
        if(!m_hasDrawElementsBaseVertex) {
            LogWarning << "Missing OpenGL extension GL_ARB_draw_elements_base_vertex";
        }
#endif        
		m_hasDrawRangeElements = true; // Introduced in OpenGL 1.2
	}
	
	if(gl.isES()) {
#ifdef ANDROID
		// Only trust entry points GLAD actually resolved for this context.
		// OpenGL ES 2.0 needs GL_OES_mapbuffer as well for glUnmapBufferOES().
		m_hasCoreMapBufferRange = gl.is(3, 0) && glMapBufferRange && glUnmapBuffer;
		m_hasMapBufferRange = m_hasCoreMapBufferRange
		                      || (gl.has("GL_EXT_map_buffer_range") && glMapBufferRangeEXT
		                          && glUnmapBufferOES);
#else
		// EXT_map_buffer_range requires OpenGL ES 1.1
		m_hasMapBufferRange = gl.is(3, 0) || gl.has("GL_EXT_map_buffer_range");
#endif
		if(!m_hasMapBufferRange) {
			LogWarning << "Missing OpenGL ES extension GL_EXT_map_buffer_range";
		}
		// OES_mapbuffer requires OpenGL ES 1.1
		m_hasMapBuffer = gl.has("GL_OES_mapbuffer");
		if(!m_hasMapBuffer) {
			LogWarning << "Missing OpenGL extension GL_OES_mapbuffer";
		}
#ifdef ANDROID
		LogInfo << "GLES vertex upload: "
		        << (m_hasMapBufferRange ? "unsynchronized glMapBufferRange ring + orphaning"
		                                : "CPU shadow + buffer upload");
#endif
	} else {
#ifndef ANDROID       
		// ARB_map_buffer_range requires OpenGL 2.1
		m_hasMapBufferRange = gl.has("GL_ARB_map_buffer_range", 3, 0);
		if(!m_hasMapBufferRange) {
			LogWarning << "Missing OpenGL extension GL_ARB_map_buffer_range";
		}
#else
        m_hasMapBufferRange = gl.has("GL_ARB_map_buffer_range");
#endif        
		m_hasMapBuffer = true; // Introduced in OpenGL 1.5
	}
	
	if(gl.isES()) {
		// EXT_buffer_storage requires OpenGL ES 3.1
		m_hasBufferStorage = gl.has("GL_EXT_buffer_storage");
		m_hasBufferUsageStream = gl.is(2, 0);
	} else {
#ifndef ANDROID       
		m_hasBufferStorage = gl.has("GL_ARB_buffer_storage", 4, 4);
#else
        m_hasBufferStorage = false;
#endif        
		m_hasBufferUsageStream = true; // Introduced in OpenGL 1.5
	}
	
#ifndef ANDROID   
	if(gl.isES()) {
		m_hasClearDepthf = true;
	} else {
		m_hasClearDepthf = gl.has("GL_ARB_ES2_compatibility", 4, 1) || gl.has("GL_OES_single_precision");
	}
#else
    // glClearDepthf is core in OpenGL ES 2.0 and newer.
    m_hasClearDepthf = gl.isES() && gl.is(2, 0);
#endif    
	
#ifndef ANDROID   
	// Introduced in OpenGL 1.4, no extension available for OpenGL ES
	m_hasVertexFogCoordinate = !gl.isES();
#else
    m_hasVertexFogCoordinate = false;
#endif
	if(gl.isES()) {
		m_hasSampleShading = gl.has("GL_OES_sample_shading", 3, 2);
	} else {
#ifndef ANDROID       
		#if ARX_HAVE_GLEW
		// The extension and core version have different entry points
		m_hasSampleShading = gl.has("GL_ARB_sample_shading");
		#else
		m_hasSampleShading = gl.has("GL_ARB_sample_shading", 4, 0);
        #endif
#else
        m_hasSampleShading = gl.has("GL_OES_sample_shading");
#endif        
	}
	
	if(gl.isES()) {
		m_hasFogx = true;
		m_hasFogDistanceMode = false;
	} else {
		m_hasFogx = false;
		m_hasFogDistanceMode = gl.has("GL_NV_fog_distance");
	}
}

void OpenGLRenderer::beforeResize(bool wasOrIsFullscreen) {
	
#if ARX_PLATFORM == ARX_PLATFORM_LINUX || ARX_PLATFORM == ARX_PLATFORM_BSD || ARX_PLATFORM == ARX_PLATFORM_HAIKU
	// No re-initialization needed
	ARX_UNUSED(wasOrIsFullscreen);
#else
	
	if(!isInitialized()) {
		return;
	}
	
	#if ARX_PLATFORM == ARX_PLATFORM_WIN32
	if(!wasOrIsFullscreen) {
		return;
	}
	#else
	// By default, always reinit to avoid issues on untested platforms
	ARX_UNUSED(wasOrIsFullscreen);
	#endif
	
	shutdown();
	
#endif
	
}

void OpenGLRenderer::afterResize() {
	if(!isInitialized()) {
		reinit();
	}
}

void OpenGLRenderer::reinit() {
	arx_assert(!isInitialized());
#ifdef ANDROID
	// A recreated EGL context has fresh bindings, viewport/scissor state and
	// disabled vertex attributes. Clear the renderer-side caches as well so
	// the next SetViewport/SetScissor call always reaches the new context.
	resetVertexArrayState();
	viewport = Rect();
	m_scissor = Rect();
	maxTextureStage = 0;
	m_MSAALevel = 0;
	{
		GLint sampleBuffers = 0;
		GLint samples = 0;
		glGetIntegerv(GL_SAMPLE_BUFFERS, &sampleBuffers);
		if(sampleBuffers > 0) {
			glGetIntegerv(GL_SAMPLES, &samples);
			m_MSAALevel = std::max(0, samples);
		}
	}
	m_hasMSAA = m_MSAALevel > 0 && config.video.antialiasing;
	m_glalphaToCoverage = false;
	glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
	m_glstate.setCull(false);
	m_glstate.setFog(false);
	m_glstate.setAlphaCutout(false);
	m_glstate.setDepthTest(false);
	m_glstate.setDepthWrite(true);
	m_glstate.setDepthOffset(0);
	m_glstate.setBlend(BlendOne, BlendZero);
	m_glblendSrc = GL_ONE;
	m_glblendDst = GL_ZERO;
	glDisable(GL_CULL_FACE);
	glDisable(GL_SCISSOR_TEST);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_ALWAYS);
	glDepthMask(GL_TRUE);
	// ONE/ZERO is the same as no blending: keep GL_BLEND off for it (see flushState()).
	glBlendFunc(GL_ONE, GL_ZERO);
	glDisable(GL_BLEND);
	m_glesBlendEnabled = false;
	glEnable(GL_POLYGON_OFFSET_FILL);
	glPolygonOffset(0.f, 0.f);
	glActiveTexture(GL_TEXTURE0);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	GLint texunits = 1;
	glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &texunits);
	m_TextureStages.resize(size_t(std::max(1, std::min(texunits, 3))));
	for(size_t i = 0; i < m_TextureStages.size(); ++i) {
		m_TextureStages[i] = std::make_unique<GLTextureStage>(this, unsigned(i));
	}
	Clear(ColorBuffer | DepthBuffer);
	m_currentTransform = GL_UnsetTransform;
	if(!initializeGLESProgram()) {
		LogError << "Android GLES renderer initialization failed";
		m_TextureStages.clear();
		resetVertexArrayState();
		return;
	}
#else
	m_MSAALevel = 0;
	{
		GLint buffers = 0;
		glGetIntegerv(GL_SAMPLE_BUFFERS, &buffers);
		if(buffers) {
			GLint samples = 0;
			glGetIntegerv(GL_SAMPLES, &samples);
			m_MSAALevel = samples;
		}
	}
	if(m_MSAALevel > 0) glDisable(GL_MULTISAMPLE);
	m_hasMSAA = false;
	m_glstate.setCull(false);
	if(m_hasFogx) {
		#if ARX_HAVE_EPOXY
		glFogx(GL_FOG_MODE, GL_LINEAR);
		#endif
	} else {
		glFogi(GL_FOG_MODE, GL_LINEAR);
		if(m_hasFogDistanceMode) glFogi(GL_FOG_DISTANCE_MODE_NV, GL_EYE_PLANE);
	}
	m_glstate.setFog(false);
	m_glsampleShading = false;
	m_glalphaToCoverage = false;
	glEnable(GL_ALPHA_TEST);
	m_glalphaFunc = -1.f;
#ifdef GL_VERSION_4_0
	if(hasSampleShading()) {
		#if ARX_HAVE_GLEW
		glMinSampleShadingARB(1.f);
		#else
		glMinSampleShading(1.f);
		#endif
	}
#endif
	m_glstate.setAlphaCutout(false);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_ALWAYS);
	m_glstate.setDepthTest(false);
	m_glstate.setDepthWrite(true);
	glEnable(GL_POLYGON_OFFSET_FILL);
	m_glstate.setDepthOffset(0);
	glEnable(GL_BLEND);
	m_glstate.setBlend(BlendZero, BlendOne);
	m_glblendSrc = GL_ONE;
	m_glblendDst = GL_ZERO;
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	GLint texunits = 0;
	glGetIntegerv(GL_MAX_TEXTURE_UNITS, &texunits);
	m_TextureStages.resize(texunits);
	for(size_t i = 0; i < m_TextureStages.size(); ++i) {
		m_TextureStages[i] = std::make_unique<GLTextureStage>(this, i);
	}
	Clear(ColorBuffer | DepthBuffer);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glClientActiveTexture(GL_TEXTURE0);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	m_currentTransform = GL_UnsetTransform;
	switchVertexArray(GL_NoArray, 0, 1, nullptr);
#endif
	onRendererInit();
}

void OpenGLRenderer::shutdown() {
	
	arx_assert(isInitialized());
#ifdef ANDROID
	shutdownGLESProgram();
#endif
	
	onRendererShutdown();
	
	m_TextureStages.clear();
	
	m_maximumAnisotropy = 1.f;
	m_maximumSupportedAnisotropy = 1.f;
	
}

void OpenGLRenderer::enableTransform() {
#ifdef ANDROID
	m_currentTransform = GL_ModelViewProjectionTransform;
#else
	if(m_currentTransform == GL_ModelViewProjectionTransform) return;
	glMatrixMode(GL_MODELVIEW);
	glLoadMatrixf(glm::value_ptr(m_view));
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(glm::value_ptr(m_projection));
	if(hasVertexFogCoordinate()) glFogi(GL_FOG_COORDINATE_SOURCE, GL_FRAGMENT_DEPTH);
	m_currentTransform = GL_ModelViewProjectionTransform;
#endif
}

void OpenGLRenderer::disableTransform() {
#ifdef ANDROID
	m_currentTransform = GL_NoTransform;
#else
	if(m_currentTransform == GL_NoTransform) return;
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glTranslatef(-1.f, 1.f, 0.f);
	glScalef(2.f / viewport.width(), -2.f / viewport.height(), 1.f);
	glTranslatef(0.5f, 0.5f, 0.f);
	if(hasVertexFogCoordinate()) glFogi(GL_FOG_COORDINATE_SOURCE, GL_FOG_COORDINATE);
	m_currentTransform = GL_NoTransform;
#endif
}

void OpenGLRenderer::SetViewMatrix(const glm::mat4x4 & matView) {
	
	if(m_view == matView) {
		return;
	}
	
	if(m_currentTransform == GL_ModelViewProjectionTransform) {
		m_currentTransform = GL_UnsetTransform;
	}
	
	m_view = matView;
}

void OpenGLRenderer::SetProjectionMatrix(const glm::mat4x4 & matProj) {
	
	if(m_projection == matProj) {
		return;
	}
	
	if(m_currentTransform == GL_ModelViewProjectionTransform) {
		m_currentTransform = GL_UnsetTransform;
	}
	
	m_projection = matProj;
}

void OpenGLRenderer::ReleaseAllTextures() {
	for(GLTexture & texture : textures) {
		texture.destroy();
	}
}

void OpenGLRenderer::RestoreAllTextures() {
	for(GLTexture & texture : textures) {
		texture.restore();
	}
}

void OpenGLRenderer::reloadColorKeyTextures() {
	for(GLTexture & texture : textures) {
		if(texture.hasColorKey()) {
			texture.restore();
		}
	}
}

Texture * OpenGLRenderer::createTexture() {
	GLTexture * texture = new GLTexture(this);
	textures.push_back(*texture);
	return texture;
}

void OpenGLRenderer::SetViewport(const Rect & _viewport) {
	
	if(_viewport == viewport) {
		return;
	}
	
	viewport = _viewport;
	
	// TODO maybe it's better to always have the viewport cover the whole window and use glScissor instead?
	
	int height = mainApp->getWindow()->getSize().y;
	
	glViewport(viewport.left, height - viewport.bottom, viewport.width(), viewport.height());
	
	if(m_currentTransform == GL_NoTransform) {
		m_currentTransform = GL_UnsetTransform;
	}
}

void OpenGLRenderer::SetScissor(const Rect & rect) {
	
	if(m_scissor == rect) {
		return;
	}
	
	if(rect.isValid()) {
		if(!m_scissor.isValid()) {
			glEnable(GL_SCISSOR_TEST);
		}
		int height = mainApp->getWindow()->getSize().y;
		glScissor(rect.left, height - rect.bottom, rect.width(), rect.height());
	} else {
		if(m_scissor.isValid()) {
			glDisable(GL_SCISSOR_TEST);
		}
	}
	
	m_scissor = rect;
	
}

void OpenGLRenderer::Clear(BufferFlags bufferFlags, Color clearColor, float clearDepth, size_t nrects, Rect * rect) {
	
	GLbitfield buffers = 0;
	
	if(bufferFlags & ColorBuffer) {
		Color4f colorf(clearColor);
		glClearColor(colorf.r, colorf.g, colorf.b, colorf.a);
		buffers |= GL_COLOR_BUFFER_BIT;
	}
	
	if(bufferFlags & DepthBuffer) {
		if(!m_glstate.getDepthWrite()) {
			// glClear() respects the depth mask
			glDepthMask(GL_TRUE);
			m_glstate.setDepthWrite(true);
		}
        
#ifdef ANDROID
		glClearDepthf(clearDepth);
#else
		#ifdef GL_VERSION_4_1
		if(hasClearDepthf()) {
			glClearDepthf(clearDepth);
		} else
		#endif
		{
			glClearDepth(GLclampd(clearDepth));
		}
#endif
		buffers |= GL_DEPTH_BUFFER_BIT;
	}
	
	if(nrects) {
		
		Rect scissor = m_scissor;
		
		for(size_t i = 0; i < nrects; i++) {
			
			SetScissor(rect[i]);
			
			glClear(buffers);
			
		}
		
		SetScissor(scissor);
		
	} else {
		
		if(m_scissor.isValid()) {
			glDisable(GL_SCISSOR_TEST);
		}
		
		glClear(buffers);
		
		if(m_scissor.isValid()) {
			glEnable(GL_SCISSOR_TEST);
		}
		
	}
}

void OpenGLRenderer::SetFogColor(Color color) {
#ifdef ANDROID
	m_glesFogColor = color;
#else
	Color4f colorf(color);
	GLfloat fogColor[4] = { colorf.r, colorf.g, colorf.b, colorf.a };
	glFogfv(GL_FOG_COLOR, fogColor);
#endif
}

void OpenGLRenderer::SetFogParams(float fogStart, float fogEnd) {
#ifdef ANDROID
	m_glesFogStart = fogStart;
	m_glesFogEnd = std::max(fogEnd, fogStart + 0.0001f);
#else
	glFogf(GL_FOG_START, fogStart);
	glFogf(GL_FOG_END, fogEnd);
#endif
}

void OpenGLRenderer::setGamma(float gamma) {
#ifdef ANDROID
	// The video options slider maps to a 0.5-1.5 brightness multiplier.
	// Keep values above 1 so Android GLES can actually brighten the frame.
	m_glesGamma = glm::clamp(gamma, 0.f, 2.f);
#else
	ARX_UNUSED(gamma);
#endif
}

void OpenGLRenderer::SetAntialiasing(bool enable) {
#ifdef ANDROID
	if(m_MSAALevel <= 0 || (enable && !config.video.antialiasing) || enable == m_hasMSAA) {
		return;
	}
	m_hasMSAA = enable;
	flushState();
#else

	
	if(m_MSAALevel <= 0) {
		return;
	}
	
	if(enable && !config.video.antialiasing) {
		return;
	}
	
	if(enable == m_hasMSAA) {
		return;
	}
	
	// The state used for alpha cutouts can differ between msaa and non-msaa.
	// Clear the old flushed state.
	if(m_glstate.getAlphaCutout()) {
		bool alphaCutout = m_state.getAlphaCutout();
		m_state.setAlphaCutout(false);
		flushState();
		m_state.setAlphaCutout(alphaCutout);
	}
	
	// This is mostly useless as multisampling must be enabled/disabled at GL context creation.
	if(enable) {
		glEnable(GL_MULTISAMPLE);
	} else {
		glDisable(GL_MULTISAMPLE);
	}
	m_hasMSAA = enable;

#endif
}

#ifndef ANDROID
static const GLenum arxToGlFillMode[] = {
	GL_LINE,  // FillWireframe,
	GL_FILL,  // FillSolid
};
#endif

void OpenGLRenderer::SetFillMode(FillMode mode) {
#ifndef ANDROID
	glPolygonMode(GL_FRONT_AND_BACK, arxToGlFillMode[mode]);
#else
	ARX_UNUSED(mode);
#endif
}

void OpenGLRenderer::setMaxAnisotropy(float value) {
	
	float maxAnisotropy = glm::clamp(value, 1.f, m_maximumSupportedAnisotropy);
	if(m_maximumAnisotropy == maxAnisotropy) {
		return;
	}
	
	m_maximumAnisotropy = maxAnisotropy;
	
	for(GLTexture & texture : textures) {
		texture.updateMaxAnisotropy();
	}
	
}

Renderer::AlphaCutoutAntialising OpenGLRenderer::getMaxSupportedAlphaCutoutAntialiasing() const {
#ifdef ANDROID
	return m_MSAALevel > 0 ? FuzzyAlphaCutoutAA : NoAlphaCutoutAA;
#else
	#ifdef GL_VERSION_4_0
	if(hasSampleShading()) {
		return CrispAlphaCutoutAA;
	}
	#endif
	
	return FuzzyAlphaCutoutAA;
#endif
}

template <typename Vertex>
static std::unique_ptr<VertexBuffer<Vertex>> createVertexBufferImpl(OpenGLRenderer * renderer,
                                                                    size_t capacity,
                                                                    Renderer::BufferUsage usage,
                                                                    std::string_view setting) {
#ifndef ANDROID
	bool matched = false;
	
	if(renderer->hasMapBufferRange()) {
		
		#if GL_ARB_buffer_storage && !defined(ANDROID)
		
		if(renderer->hasBufferStorage()) {
			
			if(setting.empty() || setting == "persistent-orphan") {
				if(usage != Renderer::Static) {
					return std::make_unique<GLPersistentOrphanVertexBuffer<Vertex>>(renderer, capacity, usage);
				}
				matched = true;
			}
			if(setting.empty() || setting == "persistent-x3") {
				if(usage == Renderer::Stream) {
					return std::make_unique<GLPersistentFenceVertexBuffer<Vertex, 3>>(renderer, capacity, usage, 3);
				}
				matched = true;
			}
			if(setting.empty() || setting == "persistent-x2") {
				if(usage == Renderer::Stream) {
					return std::make_unique<GLPersistentFenceVertexBuffer<Vertex, 3>>(renderer, capacity, usage, 2);
				}
				matched = true;
			}
			if(setting == "persistent-nosync") {
				if(usage != Renderer::Static) {
					return std::make_unique<GLPersistentUnsynchronizedVertexBuffer<Vertex>>(renderer, capacity, usage);
				}
				matched = true;
			}
			
		}
		
		#endif // GL_ARB_buffer_storage
		
		if(setting.empty() || setting == "maprange" || setting == "maprange+subdata") {
			return std::make_unique<GLMapRangeVertexBuffer<Vertex>>(renderer, capacity, usage);
		}
		
	}
	
	if(renderer->hasMapBuffer()) {
		
		if(setting.empty() || setting == "map" || setting == "map+subdata") {
			return std::make_unique<GLMapVertexBuffer<Vertex>>(renderer, capacity, usage);
		}
		
	}
	
	if(setting.empty() || setting == "shadow" || setting == "shadow+subdata") {
		return std::make_unique<GLShadowVertexBuffer<Vertex>>(renderer, capacity, usage);
	}
	
	static bool warned = false;
	if(!matched && !warned) {
		LogWarning << "Ignoring unsupported video.buffer_upload setting: " << setting;
		warned = true;
	}
	return createVertexBufferImpl<Vertex>(renderer, capacity, usage, std::string_view());
#else
	// Never use glMapBufferOES() here: it always maps the whole buffer synchronously,
	// and ANGLE then has to copy the entire buffer (or wait for the GPU) every time a
	// buffer that queued draws still read is locked.
	const bool forceShadow = (setting == "shadow" || setting == "shadow+subdata");
	if(renderer->hasMapBufferRange() && !forceShadow) {
		return std::make_unique<GLESMapRangeVertexBuffer<Vertex>>(renderer, capacity, usage);
	}
	return std::make_unique<GLShadowVertexBuffer<Vertex>>(renderer, capacity, usage);
#endif
}

template <typename Vertex>
static std::unique_ptr<VertexBuffer<Vertex>> createVertexBufferImpl(OpenGLRenderer * renderer,
                                                                    size_t capacity,
                                                                    Renderer::BufferUsage usage) {
	return createVertexBufferImpl<Vertex>(renderer, capacity, usage, config.video.bufferUpload);
}

std::unique_ptr<VertexBuffer<TexturedVertex>> OpenGLRenderer::createVertexBufferTL(size_t capacity,
                                                                                   BufferUsage usage) {
	return createVertexBufferImpl<TexturedVertex>(this, capacity, usage);
}

std::unique_ptr<VertexBuffer<SMY_VERTEX>> OpenGLRenderer::createVertexBuffer(size_t capacity,
                                                                             BufferUsage usage) {
	return createVertexBufferImpl<SMY_VERTEX>(this, capacity, usage);
}

std::unique_ptr<VertexBuffer<SMY_VERTEX3>> OpenGLRenderer::createVertexBuffer3(size_t capacity,
                                                                               BufferUsage usage) {
	return createVertexBufferImpl<SMY_VERTEX3>(this, capacity, usage);
}

const GLenum arxToGlPrimitiveType[] = {
	GL_TRIANGLES, // TriangleList,
	GL_TRIANGLE_STRIP, // TriangleStrip,
	GL_TRIANGLE_FAN, // TriangleFan,
	GL_LINES, // LineList,
	GL_LINE_STRIP // LineStrip
};

void OpenGLRenderer::drawIndexed(Primitive primitive, const TexturedVertex * vertices, size_t nvertices, unsigned short * indices, size_t nindices) {
	
	beforeDraw<TexturedVertex>();

#ifdef ANDROID
	if(nvertices == 0 || nindices == 0) {
		return;
	}
	// Draw straight from client memory like the desktop renderer: the driver (or
	// ANGLE) copies the data into its own transient storage. Appending into a GL
	// buffer with glBufferSubData() instead forces ANGLE to end the render pass
	// for every draw, which is extremely slow on tiled GPUs such as Mali.
	bindBuffer(GL_NONE);
	setVertexArray(this, vertices, vertices);
	drawIndexedElements(primitive, indices, nindices, 0, nvertices);
#else
	bindBuffer(GL_NONE);
	setVertexArray(this, vertices, vertices);
	if(hasDrawRangeElements()) {
		glDrawRangeElements(arxToGlPrimitiveType[primitive], 0, nvertices - 1, nindices, GL_UNSIGNED_SHORT, indices);
	} else {
		glDrawElements(arxToGlPrimitiveType[primitive], nindices, GL_UNSIGNED_SHORT, indices);
	}
#endif
	
}

void OpenGLRenderer::drawIndexedElements(Primitive primitive, const unsigned short * indices,
                                              size_t nindices, size_t baseVertex, size_t vertexCount) {
#ifdef ANDROID
	if(nindices == 0) {
		return;
	}
	arx_assert(nindices <= size_t(std::numeric_limits<GLsizei>::max()));
	arx_assert(baseVertex <= size_t(std::numeric_limits<GLint>::max()));

	// Indices always come from client memory: this renderer never binds an
	// element array buffer (resetVertexArrayState() unbinds it). Native drivers
	// and ANGLE stream client indices through their own transient buffers, which
	// never interrupts the current render pass. Writing them into a shared EBO
	// with glBufferSubData() made ANGLE end the render pass on every draw.
	const GLenum mode = arxToGlPrimitiveType[primitive];
	const GLsizei count = GLsizei(nindices);

	if(baseVertex != 0) {
		// Only reached when hasDrawElementsBaseVertex() is true, otherwise
		// BaseGLVertexBuffer::drawIndexed() moves the attribute pointers instead.
		arx_assert(hasDrawElementsBaseVertex());
		if(glDrawElementsBaseVertexOES) {
			glDrawElementsBaseVertexOES(mode, count, GL_UNSIGNED_SHORT, indices, GLint(baseVertex));
		} else {
			glDrawElementsBaseVertexEXT(mode, count, GL_UNSIGNED_SHORT, indices, GLint(baseVertex));
		}
		return;
	}

	if(hasDrawRangeElements() && vertexCount > 0) {
		glDrawRangeElements(mode, 0, GLuint(vertexCount - 1), count, GL_UNSIGNED_SHORT, indices);
	} else {
		glDrawElements(mode, count, GL_UNSIGNED_SHORT, indices);
	}
#else
	ARX_UNUSED(primitive);
	ARX_UNUSED(indices);
	ARX_UNUSED(nindices);
	ARX_UNUSED(baseVertex);
	ARX_UNUSED(vertexCount);
#endif
}

bool OpenGLRenderer::getSnapshot(Image & image) {
	
	Vec2i size = mainApp->getWindow()->getSize();
	
	image.create(size_t(size.x), size_t(size.y), Image::Format_R8G8B8);
#ifdef ANDROID
	// GLES 2 only guarantees RGBA readback. Strip alpha after reading to keep
	// the renderer's existing RGB Image contract.
	Image rgba;
	rgba.create(size_t(size.x), size_t(size.y), Image::Format_R8G8B8A8);
	glReadPixels(0, 0, size.x, size.y, GL_RGBA, GL_UNSIGNED_BYTE, rgba.getData());
	const unsigned char * src = rgba.getData();
	unsigned char * dst = image.getData();
	for(size_t i = 0, pixels = size_t(size.x) * size_t(size.y); i < pixels; ++i) {
		dst[0] = src[0];
		dst[1] = src[1];
		dst[2] = src[2];
		src += 4;
		dst += 3;
	}
#else
	glReadPixels(0, 0, size.x, size.y, GL_RGB, GL_UNSIGNED_BYTE, image.getData());
#endif
	
	image.flipY();
	
	return true;
}

bool OpenGLRenderer::getSnapshot(Image & image, size_t width, size_t height) {
	
	// TODO handle scaling on the GPU so we don't need to download the whole image
	
	Image fullsize;
	getSnapshot(fullsize);
	
	image.resizeFrom(fullsize, width, height);
	
	return true;
}

static const GLenum arxToGlBlendFactor[] = {
	GL_ZERO, // BlendZero,
	GL_ONE, // BlendOne,
	GL_SRC_COLOR, // BlendSrcColor,
	GL_SRC_ALPHA, // BlendSrcAlpha,
	GL_ONE_MINUS_SRC_COLOR, // BlendInvSrcColor,
	GL_ONE_MINUS_SRC_ALPHA, // BlendInvSrcAlpha,
	GL_SRC_ALPHA_SATURATE, // BlendSrcAlphaSaturate,
	GL_DST_COLOR, // BlendDstColor,
	GL_DST_ALPHA, // BlendDstAlpha,
	GL_ONE_MINUS_DST_COLOR, // BlendInvDstColor,
	GL_ONE_MINUS_DST_ALPHA // BlendInvDstAlpha
};

void OpenGLRenderer::flushState() {
#ifdef ANDROID
	BlendingFactor srcFactor = m_state.getBlendSrc();
	BlendingFactor dstFactor = m_state.getBlendDst();
	m_glesAlphaThreshold = 0.5f;
	bool useAlphaToCoverage = false;
	if(m_state.getAlphaCutout()) {
		if(srcFactor == BlendOne && dstFactor == BlendZero) {
			useAlphaToCoverage = m_hasMSAA
			        && config.video.alphaCutoutAntialiasing != int(NoAlphaCutoutAA);
			if(useAlphaToCoverage) {
				m_glesAlphaThreshold = 0.f;
			}
		} else if(srcFactor == BlendOne && dstFactor == BlendOne) {
			srcFactor = BlendSrcAlpha;
			m_glesAlphaThreshold = 0.f;
		} else if(srcFactor == BlendSrcAlpha || dstFactor == BlendInvSrcAlpha) {
			m_glesAlphaThreshold = 0.f;
		}
	}
	if(useAlphaToCoverage != m_glalphaToCoverage) {
		if(useAlphaToCoverage) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
		else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
		m_glalphaToCoverage = useAlphaToCoverage;
	}

	if(m_glstate != m_state) {
		if(m_glstate.getCull() != m_state.getCull()) {
			if(m_state.getCull()) glEnable(GL_CULL_FACE);
			else glDisable(GL_CULL_FACE);
		}
		if(m_glstate.getBlendSrc() != m_state.getBlendSrc()
		   || m_glstate.getBlendDst() != m_state.getBlendDst()
		   || m_glstate.getAlphaCutout() != m_state.getAlphaCutout()) {
			GLenum src = arxToGlBlendFactor[srcFactor];
			GLenum dst = arxToGlBlendFactor[dstFactor];
			// ONE/ZERO writes the fragment unchanged, so disable blending for it:
			// tiled GPUs (Mali forward pixel kill, Adreno LRZ) only treat draws with
			// blending turned off as opaque.
			const bool blend = !(src == GL_ONE && dst == GL_ZERO);
			if(blend != m_glesBlendEnabled) {
				if(blend) glEnable(GL_BLEND);
				else glDisable(GL_BLEND);
				m_glesBlendEnabled = blend;
			}
			if(blend && (m_glblendSrc != src || m_glblendDst != dst)) {
				glBlendFunc(src, dst);
				m_glblendSrc = src;
				m_glblendDst = dst;
			}
		}
		if(m_glstate.getDepthTest() != m_state.getDepthTest()) {
			glDepthFunc(m_state.getDepthTest() ? GL_LEQUAL : GL_ALWAYS);
		}
		if(m_glstate.getDepthWrite() != m_state.getDepthWrite()) {
			glDepthMask(m_state.getDepthWrite() ? GL_TRUE : GL_FALSE);
		}
		if(m_glstate.getDepthOffset() != m_state.getDepthOffset()) {
			GLfloat offset = -GLfloat(m_state.getDepthOffset());
			glPolygonOffset(offset, offset);
		}
		m_glstate = m_state;
	}
	m_glesAlphaCutout = m_state.getAlphaCutout();
	glActiveTexture(GL_TEXTURE0);
	for(size_t i = 0; i < m_TextureStages.size() && i <= maxTextureStage; ++i) {
		GetTextureStage(i)->apply();
	}
	glActiveTexture(GL_TEXTURE0);
#else

	
	if(m_glstate != m_state) {
		
		if(m_glstate.getCull() != m_state.getCull()) {
			if(m_state.getCull()) {
				glEnable(GL_CULL_FACE);
			} else {
				glDisable(GL_CULL_FACE);
			}
		}
		
		if(m_glstate.getFog() != m_state.getFog()) {
			if(m_state.getFog()) {
				glEnable(GL_FOG);
			} else {
				glDisable(GL_FOG);
			}
		}
		
		if(m_glstate.getBlendSrc() != m_state.getBlendSrc()
		   || m_glstate.getBlendDst() != m_state.getBlendDst()
		   || m_glstate.getAlphaCutout() != m_state.getAlphaCutout()) {
			
			enum TestMode {
				TestSS,
				TestA2C,
				TestStrict,
				TestConservative,
				TestNone,
			};
			
			bool useSS = m_hasMSAA && hasSampleShading() && config.video.alphaCutoutAntialiasing == int(CrispAlphaCutoutAA);
			bool useA2C = m_hasMSAA && config.video.alphaCutoutAntialiasing == int(FuzzyAlphaCutoutAA);
			
			/* When rendering alpha cutouts with alpha blending enabled we still
			* need to 'discard' transparent texels, as blending might not use the src alpha!
			* On the other hand, we can't use GL_SAMPLE_ALPHA_TO_COVERAGE when blending
			* as that could result in the src alpha being applied twice (e.g. for text).
			* So we must toggle between alpha to coverage and alpha test when toggling blending.
			*/
			BlendingFactor blendSrc = m_state.getBlendSrc();
			BlendingFactor blendDst = m_state.getBlendDst();
			TestMode alphaTest = TestNone;
			if(m_state.getAlphaCutout()) {
				arx_assert_msg(blendSrc != BlendInvSrcAlpha && blendDst != BlendSrcAlpha,
				               "inverted alpha blending combined with alpha test makes no sense");
				if(blendSrc == BlendOne && blendDst == BlendZero) {
					alphaTest = useSS ? TestSS : useA2C ? TestA2C : TestStrict;
				} else if(blendSrc == BlendOne && blendDst == BlendOne) {
					blendSrc = BlendSrcAlpha;
					alphaTest = TestConservative; // optimization only, could use TestNone
				} else if(blendSrc == BlendSrcAlpha || blendDst == BlendInvSrcAlpha) {
					alphaTest = TestConservative;
				} else {
					alphaTest = useSS ? TestSS : TestStrict;
				}
			}
			
			if(m_glsampleShading && alphaTest != TestSS) {
				glDisable(GL_SAMPLE_SHADING_ARB);
				m_glsampleShading = false;
			} else if(!m_glsampleShading && alphaTest == TestSS) {
				glEnable(GL_SAMPLE_SHADING_ARB);
				m_glsampleShading = true;
			}
			
			if(m_glalphaToCoverage && alphaTest != TestA2C) {
				glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
				m_glalphaToCoverage = false;
			} else if(!m_glalphaToCoverage && alphaTest == TestA2C) {
				glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
				m_glalphaToCoverage = true;
			}
			
			if(alphaTest == TestNone) {
				if(m_glalphaFunc >= 0.f) {
					glAlphaFunc(GL_ALWAYS, 0.f);
					m_glalphaFunc = -1.f;
				}
			} else if(alphaTest == TestConservative || alphaTest == TestA2C) {
				if(m_glalphaFunc != 0.f) {
					glAlphaFunc(GL_GREATER, 0.f);
					m_glalphaFunc = 0.f;
				}
			} else {
				if(m_glalphaFunc != 0.5f) {
					glAlphaFunc(GL_GREATER, 0.5f);
					m_glalphaFunc = 0.5f;
				}
			}
			
			GLenum glblendSrc = arxToGlBlendFactor[blendSrc];
			GLenum glblendDst = arxToGlBlendFactor[blendDst];
			if(m_glblendSrc != glblendSrc || m_glblendDst != glblendDst) {
				glBlendFunc(glblendSrc, glblendDst);
				m_glblendSrc = glblendSrc;
				m_glblendDst = glblendDst;
			}
			
		}
		
		if(m_glstate.getDepthTest() != m_state.getDepthTest()) {
			glDepthFunc(m_state.getDepthTest() ? GL_LEQUAL : GL_ALWAYS);
		}
		
		if(m_glstate.getDepthWrite() != m_state.getDepthWrite()) {
			glDepthMask(m_state.getDepthWrite() ? GL_TRUE : GL_FALSE);
		}
		
		if(m_glstate.getDepthOffset() != m_state.getDepthOffset()) {
			GLfloat depthOffset = -GLfloat(m_state.getDepthOffset());
			glPolygonOffset(depthOffset, depthOffset);
		}
		
		m_glstate = m_state;
	}
	
	for(size_t i = 0; i <= maxTextureStage; i++) {
		GetTextureStage(i)->apply();
	}
	

#endif
}

#ifdef ANDROID
namespace {

GLuint compileGLESShader(GLenum type, const std::string & source) {
	GLuint shader = glCreateShader(type);
	if(!shader) {
		LogError << "Could not create GLES shader";
		return 0;
	}
	const char * text = source.c_str();
	glShaderSource(shader, 1, &text, nullptr);
	glCompileShader(shader);
	GLint compiled = GL_FALSE;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
	if(compiled != GL_TRUE) {
		GLint length = 0;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
		std::string log(size_t(std::max(1, length)), '\0');
		glGetShaderInfoLog(shader, GLsizei(log.size()), nullptr, log.data());
		LogError << "GLES shader compilation failed: " << log;
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

GLuint linkGLESProgram(GLuint vertex, GLuint fragment) {
	GLuint program = glCreateProgram();
	if(!program) {
		LogError << "Could not create GLES shader program";
		return 0;
	}
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glBindAttribLocation(program, 0, "aPosition");
	glBindAttribLocation(program, 1, "aColor");
	glBindAttribLocation(program, 2, "aTexCoord0");
	glBindAttribLocation(program, 3, "aTexCoord1");
	glBindAttribLocation(program, 4, "aTexCoord2");
	glLinkProgram(program);
	GLint linked = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if(linked != GL_TRUE) {
		GLint length = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
		std::string log(size_t(std::max(1, length)), '\0');
		glGetProgramInfoLog(program, GLsizei(log.size()), nullptr, log.data());
		LogError << "GLES program link failed: " << log;
		glDeleteProgram(program);
		return 0;
	}
	return program;
}

/*
 * Shader sources are assembled as <prefix><variant defines><body>.
 * The prefix maps the few keywords that differ between GLSL ES 1.00 and 3.00,
 * so both language versions share one body and cannot drift apart.
 * "#version" has to stay on the very first line for GLSL ES 3.00.
 */
const char * const glesShaderPrefix100 =
	"#define ARX_VS_IN attribute\n"
	"#define ARX_VARYING_OUT varying\n"
	"#define ARX_VARYING_IN varying\n"
	"#define ARX_TEXTURE texture2D\n"
	"#define ARX_FRAG_COLOR gl_FragColor\n";

const char * const glesShaderPrefix300 =
	"#version 300 es\n"
	"#define ARX_GLSL_300 1\n"
	"#define ARX_VS_IN in\n"
	"#define ARX_VARYING_OUT out\n"
	"#define ARX_VARYING_IN in\n"
	"#define ARX_TEXTURE texture\n"
	"#define ARX_FRAG_COLOR arxFragColor\n";

const char * const glesVertexShader = R"(
precision highp float;
ARX_VS_IN vec4 aPosition;
ARX_VS_IN vec4 aColor;
ARX_VS_IN vec2 aTexCoord0;
ARX_VS_IN vec2 aTexCoord1;
ARX_VS_IN vec2 aTexCoord2;
uniform mat4 uProjection;
uniform mat4 uView;
uniform int uTransform;
ARX_VARYING_OUT mediump vec4 vColor;
ARX_VARYING_OUT mediump vec2 vTexCoord0;
ARX_VARYING_OUT mediump vec2 vTexCoord1;
ARX_VARYING_OUT mediump vec2 vTexCoord2;
ARX_VARYING_OUT mediump float vFogDistance;
void main() {
    if(uTransform != 0) {
        vec4 viewPosition = uView * aPosition;
        gl_Position = uProjection * viewPosition;
        vFogDistance = max(-viewPosition.z, 0.0);
    } else {
        gl_Position = uProjection * aPosition;
        vFogDistance = abs(aPosition.w);
    }
    vColor = aColor;
    vTexCoord0 = aTexCoord0;
    vTexCoord1 = aTexCoord1;
    vTexCoord2 = aTexCoord2;
}
)";

/*
 * ARX_ALPHA_CUTOUT and ARX_FOG select the fragment shader variant. Keeping
 * "discard" out of the programs used for opaque geometry matters on Mali:
 * a shader that may discard disables forward pixel kill and early depth
 * updates for every draw that uses it.
 */
const char * const glesFragmentShader = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
#ifdef ARX_GLSL_300
out vec4 arxFragColor;
#endif
uniform sampler2D uTexture0;
uniform sampler2D uTexture1;
uniform sampler2D uTexture2;
uniform float uLodBias0;
uniform float uLodBias1;
uniform float uLodBias2;
uniform int uUseTexture0;
uniform int uUseTexture1;
uniform int uUseTexture2;
uniform int uColorOp0;
uniform int uColorOp1;
uniform int uColorOp2;
uniform int uAlphaOp0;
uniform int uAlphaOp1;
uniform int uAlphaOp2;
#ifdef ARX_ALPHA_CUTOUT
uniform float uAlphaThreshold;
#endif
#ifdef ARX_FOG
uniform vec4 uFogColor;
uniform vec2 uFogRange;
ARX_VARYING_IN mediump float vFogDistance;
#endif
uniform float uGamma;
ARX_VARYING_IN mediump vec4 vColor;
ARX_VARYING_IN mediump vec2 vTexCoord0;
ARX_VARYING_IN mediump vec2 vTexCoord1;
ARX_VARYING_IN mediump vec2 vTexCoord2;
float opScale(int op) {
    if(op == 3) return 2.0;
    if(op == 4) return 4.0;
    return 1.0;
}
vec4 applyStage(vec4 previous, vec4 texel, int colorOp, int alphaOp) {
    if(colorOp == 1) previous.rgb = texel.rgb;
    else if(colorOp >= 2) previous.rgb *= texel.rgb * opScale(colorOp);
    if(alphaOp == 1) previous.a = texel.a;
    else if(alphaOp >= 2) previous.a *= texel.a * opScale(alphaOp);
    return clamp(previous, 0.0, 1.0);
}
void main() {
    vec4 color = vColor;
    if(uUseTexture0 != 0) color = applyStage(color, ARX_TEXTURE(uTexture0, vTexCoord0, uLodBias0), uColorOp0, uAlphaOp0);
    if(uUseTexture1 != 0) color = applyStage(color, ARX_TEXTURE(uTexture1, vTexCoord1, uLodBias1), uColorOp1, uAlphaOp1);
    if(uUseTexture2 != 0) color = applyStage(color, ARX_TEXTURE(uTexture2, vTexCoord2, uLodBias2), uColorOp2, uAlphaOp2);
#ifdef ARX_ALPHA_CUTOUT
    if(color.a <= uAlphaThreshold) discard;
#endif
#ifdef ARX_FOG
    float fog = clamp((uFogRange.y - vFogDistance) / max(uFogRange.y - uFogRange.x, 0.0001), 0.0, 1.0);
    color.rgb = mix(uFogColor.rgb, color.rgb, fog);
#endif
    color.rgb = clamp(color.rgb * uGamma, 0.0, 1.0);
    ARX_FRAG_COLOR = color;
}
)";

} // namespace

bool OpenGLRenderer::initializeGLESProgram() {

	shutdownGLESProgram();

	const std::string prefix = g_useGLES2_0 ? glesShaderPrefix100 : glesShaderPrefix300;

	GLuint vertex = compileGLESShader(GL_VERTEX_SHADER, prefix + glesVertexShader);
	if(!vertex) {
		return false;
	}

	const char * samplerNames[3] = { "uTexture0", "uTexture1", "uTexture2" };
	const char * useNames[3] = { "uUseTexture0", "uUseTexture1", "uUseTexture2" };
	const char * colorNames[3] = { "uColorOp0", "uColorOp1", "uColorOp2" };
	const char * alphaNames[3] = { "uAlphaOp0", "uAlphaOp1", "uAlphaOp2" };
	const char * lodBiasNames[3] = { "uLodBias0", "uLodBias1", "uLodBias2" };

	bool success = true;
	for(size_t variant = 0; variant < GLESProgramVariantCount; variant++) {

		std::string source = prefix;
		if(variant & GLESProgramAlphaCutout) {
			source += "#define ARX_ALPHA_CUTOUT 1\n";
		}
		if(variant & GLESProgramFog) {
			source += "#define ARX_FOG 1\n";
		}
		source += glesFragmentShader;

		GLuint fragment = compileGLESShader(GL_FRAGMENT_SHADER, source);
		if(!fragment) {
			success = false;
			break;
		}
		GLuint id = linkGLESProgram(vertex, fragment);
		glDeleteShader(fragment);
		if(!id) {
			success = false;
			break;
		}

		GLESProgram & program = m_glesPrograms[variant];
		program.id = id;
		program.cacheValid = false;
		program.uniformProjection = glGetUniformLocation(id, "uProjection");
		program.uniformView = glGetUniformLocation(id, "uView");
		program.uniformTransform = glGetUniformLocation(id, "uTransform");
		program.uniformFogColor = glGetUniformLocation(id, "uFogColor");
		program.uniformFogRange = glGetUniformLocation(id, "uFogRange");
		program.uniformAlphaThreshold = glGetUniformLocation(id, "uAlphaThreshold");
		program.uniformGamma = glGetUniformLocation(id, "uGamma");
		glUseProgram(id);
		m_glesBoundProgram = id;
		for(GLint i = 0; i < 3; ++i) {
			GLint sampler = glGetUniformLocation(id, samplerNames[i]);
			if(sampler >= 0) {
				glUniform1i(sampler, i);
			}
			program.uniformUseTexture[i] = glGetUniformLocation(id, useNames[i]);
			program.uniformColorOp[i] = glGetUniformLocation(id, colorNames[i]);
			program.uniformAlphaOp[i] = glGetUniformLocation(id, alphaNames[i]);
			program.uniformLodBias[i] = glGetUniformLocation(id, lodBiasNames[i]);
		}

	}

	glDeleteShader(vertex);

	if(!success) {
		shutdownGLESProgram();
		return false;
	}

	glUseProgram(0);
	m_glesBoundProgram = 0;

	return true;
}

void OpenGLRenderer::shutdownGLESProgram() {

	if(m_glesBoundProgram) {
		glUseProgram(0);
		m_glesBoundProgram = 0;
	}

	for(GLESProgram & program : m_glesPrograms) {
		if(program.id) {
			glDeleteProgram(program.id);
		}
		program = GLESProgram();
	}

}

void OpenGLRenderer::prepareGLESProgram(bool transformed) {

	size_t variant = 0;
	if(m_glesAlphaCutout) {
		variant |= GLESProgramAlphaCutout;
	}
	if(m_state.getFog()) {
		variant |= GLESProgramFog;
	}

	GLESProgram & program = m_glesPrograms[variant];
	if(!program.id) {
		return;
	}

	if(m_glesBoundProgram != program.id) {
		glUseProgram(program.id);
		m_glesBoundProgram = program.id;
	}

	glm::mat4 projection = m_projection;
	glm::mat4 view = m_view;
	if(!transformed) {
		const float width = float(std::max(1, viewport.width()));
		const float height = float(std::max(1, viewport.height()));
		projection = glm::mat4(1.f);
		projection = glm::translate(projection, glm::vec3(-1.f, 1.f, 0.f));
		projection = glm::scale(projection, glm::vec3(2.f / width, -2.f / height, 1.f));
		projection = glm::translate(projection, glm::vec3(0.5f, 0.5f, 0.f));
		view = glm::mat4(1.f);
	}

	const bool cacheValid = program.cacheValid;

	if(program.uniformProjection >= 0
	   && (!cacheValid || std::memcmp(glm::value_ptr(program.projection), glm::value_ptr(projection),
	                                  sizeof(glm::mat4)) != 0)) {
		glUniformMatrix4fv(program.uniformProjection, 1, GL_FALSE, glm::value_ptr(projection));
		program.projection = projection;
	}

	if(program.uniformView >= 0
	   && (!cacheValid || std::memcmp(glm::value_ptr(program.view), glm::value_ptr(view),
	                                  sizeof(glm::mat4)) != 0)) {
		glUniformMatrix4fv(program.uniformView, 1, GL_FALSE, glm::value_ptr(view));
		program.view = view;
	}

	const GLint transform = transformed ? 1 : 0;
	if(program.uniformTransform >= 0 && (!cacheValid || program.transform != transform)) {
		glUniform1i(program.uniformTransform, transform);
		program.transform = transform;
	}

	if(program.uniformFogColor >= 0) {
		Color4f fog(m_glesFogColor);
		if(!cacheValid || program.fogColor[0] != fog.r || program.fogColor[1] != fog.g
		   || program.fogColor[2] != fog.b || program.fogColor[3] != fog.a) {
			glUniform4f(program.uniformFogColor, fog.r, fog.g, fog.b, fog.a);
			program.fogColor[0] = fog.r;
			program.fogColor[1] = fog.g;
			program.fogColor[2] = fog.b;
			program.fogColor[3] = fog.a;
		}
	}

	if(program.uniformFogRange >= 0
	   && (!cacheValid || program.fogStart != m_glesFogStart || program.fogEnd != m_glesFogEnd)) {
		glUniform2f(program.uniformFogRange, m_glesFogStart, m_glesFogEnd);
		program.fogStart = m_glesFogStart;
		program.fogEnd = m_glesFogEnd;
	}

	if(program.uniformAlphaThreshold >= 0
	   && (!cacheValid || program.alphaThreshold != m_glesAlphaThreshold)) {
		glUniform1f(program.uniformAlphaThreshold, m_glesAlphaThreshold);
		program.alphaThreshold = m_glesAlphaThreshold;
	}

	if(program.uniformGamma >= 0 && (!cacheValid || program.gamma != m_glesGamma)) {
		glUniform1f(program.uniformGamma, m_glesGamma);
		program.gamma = m_glesGamma;
	}

	for(size_t i = 0; i < 3; ++i) {
		GLTextureStage * stage = i < m_TextureStages.size() ? GetTextureStage(i) : nullptr;
		const GLint useTexture = stage && i <= maxTextureStage && stage->getTexture() && stage->isEnabled() ? 1 : 0;
		const GLint colorOp = stage ? GLint(stage->getColorOp()) : GLint(TextureStage::OpDisable);
		const GLint alphaOp = stage ? GLint(stage->getAlphaOp()) : GLint(TextureStage::OpDisable);
		const float lodBias = stage ? stage->getMipMapLODBias() : 0.f;
		if(program.uniformUseTexture[i] >= 0 && (!cacheValid || program.useTexture[i] != useTexture)) {
			glUniform1i(program.uniformUseTexture[i], useTexture);
			program.useTexture[i] = useTexture;
		}
		if(program.uniformColorOp[i] >= 0 && (!cacheValid || program.colorOp[i] != colorOp)) {
			glUniform1i(program.uniformColorOp[i], colorOp);
			program.colorOp[i] = colorOp;
		}
		if(program.uniformAlphaOp[i] >= 0 && (!cacheValid || program.alphaOp[i] != alphaOp)) {
			glUniform1i(program.uniformAlphaOp[i], alphaOp);
			program.alphaOp[i] = alphaOp;
		}
		if(program.uniformLodBias[i] >= 0 && (!cacheValid || program.lodBias[i] != lodBias)) {
			glUniform1f(program.uniformLodBias[i], lodBias);
			program.lodBias[i] = lodBias;
		}
	}

	program.cacheValid = true;
}
#endif

