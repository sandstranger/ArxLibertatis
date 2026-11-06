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
#include <sstream>
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
	
	#if ARX_HAVE_EPOXY
	
	#if ARX_PLATFORM == ARX_PLATFORM_WIN32
	epoxy_handle_external_wglMakeCurrent();
	#endif
	
	#elif ARX_HAVE_GLEW
	
	if(glewInit() != GLEW_OK) {
		LogError << "GLEW init failed";
		return;
	}
	
	const GLubyte * glewVersion = glewGetString(GLEW_VERSION);
	LogInfo << "Using GLEW " << glewVersion;
	CrashHandler::setVariable("GLEW version", glewVersion);
	
	#endif
	
	OpenGLInfo gl;
	
	LogInfo << "Using OpenGL " << gl.versionString();
	CrashHandler::setVariable("OpenGL version", gl.versionString());
	
	LogInfo << " ├─ Vendor: " << gl.vendor();
	CrashHandler::setVariable("OpenGL vendor", gl.vendor());
	
	LogInfo << " ├─ Device: " << gl.renderer();
	CrashHandler::setVariable("OpenGL device", gl.renderer());
	
	#if !defined(ANDROID) && defined(GL_CONTEXT_FLAG_DEBUG_BIT) || defined(GL_CONTEXT_FLAG_NO_ERROR_BIT)
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
		m_hasTextureNPOT = gl.is(3, 0) || gl.has("GL_OES_texture_npot", 2, 0);
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
		// OES_draw_elements_base_vertex requires OpenGL ES 2.0
		// EXT_draw_elements_base_vertex requires OpenGL ES 2.0
		m_hasDrawElementsBaseVertex = gl.has("GL_OES_draw_elements_base_vertex", 3, 2)
		                              || gl.has("GL_EXT_draw_elements_base_vertex");
		m_hasDrawRangeElements = gl.is(3, 0);
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
		// EXT_map_buffer_range requires OpenGL ES 1.1
		m_hasMapBufferRange = gl.is(3, 0) || gl.has("GL_EXT_map_buffer_range");
		if(!m_hasMapBufferRange) {
			LogWarning << "Missing OpenGL extension GL_EXT_map_buffer_range";
		}
		// OES_mapbuffer requires OpenGL ES 1.1
		m_hasMapBuffer = gl.has("GL_OES_mapbuffer");
		if(!m_hasMapBuffer) {
			LogWarning << "Missing OpenGL extension GL_OES_mapbuffer";
		}
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
    m_hasClearDepthf = gl.has("GL_ARB_ES2_compatibility" ) || gl.has("GL_OES_single_precision");
#endif    
	
#ifndef ANDROID   
	// Introduced in OpenGL 1.4, no extension available for OpenGL ES
	m_hasVertexFogCoordinate = !gl.isES();
#else
    extern bool g_useGLES2_0;
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
	m_MSAALevel = 0;
	m_hasMSAA = false;
	m_glstate.setCull(false);
	m_glstate.setFog(false);
	m_glstate.setAlphaCutout(false);
	m_glstate.setDepthTest(false);
	m_glstate.setDepthWrite(true);
	m_glstate.setDepthOffset(0);
	m_glstate.setBlend(BlendOne, BlendZero);
	m_glblendSrc = GL_ONE;
	m_glblendDst = GL_ZERO;
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_ALWAYS);
	glDepthMask(GL_TRUE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ZERO);
	glEnable(GL_POLYGON_OFFSET_FILL);
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
	initializeGLESProgram();
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
	switchVertexArray(GL_NoArray, 0, 1);
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

void OpenGLRenderer::SetAntialiasing(bool enable) {
#ifdef ANDROID
	ARX_UNUSED(enable);
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

static const GLenum arxToGlFillMode[] = {
	GL_LINE,  // FillWireframe,
	GL_FILL,  // FillSolid
};

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
	
	#ifdef GL_VERSION_4_0
	if(hasSampleShading()) {
		return CrispAlphaCutoutAA;
	}
	#endif
	
	return FuzzyAlphaCutoutAA;
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
	
	bindBuffer(GL_NONE);
	
	setVertexArray(this, vertices, vertices);
	
#ifdef ANDROID
	glDrawElements(arxToGlPrimitiveType[primitive], GLsizei(nindices), GL_UNSIGNED_SHORT, indices);
#else
	if(hasDrawRangeElements()) {
		glDrawRangeElements(arxToGlPrimitiveType[primitive], 0, nvertices - 1, nindices, GL_UNSIGNED_SHORT, indices);
	} else {
		glDrawElements(arxToGlPrimitiveType[primitive], nindices, GL_UNSIGNED_SHORT, indices);
	}
#endif
	
}

bool OpenGLRenderer::getSnapshot(Image & image) {
	
	Vec2i size = mainApp->getWindow()->getSize();
	
	image.create(size_t(size.x), size_t(size.y), Image::Format_R8G8B8);
	
	glReadPixels(0, 0, size.x, size.y, GL_RGB, GL_UNSIGNED_BYTE, image.getData());
	
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
	if(m_glstate != m_state) {
		if(m_glstate.getCull() != m_state.getCull()) {
			if(m_state.getCull()) glEnable(GL_CULL_FACE);
			else glDisable(GL_CULL_FACE);
		}
		if(m_glstate.getBlendSrc() != m_state.getBlendSrc()
		   || m_glstate.getBlendDst() != m_state.getBlendDst()) {
			GLenum src = arxToGlBlendFactor[m_state.getBlendSrc()];
			GLenum dst = arxToGlBlendFactor[m_state.getBlendDst()];
			if(m_glblendSrc != src || m_glblendDst != dst) {
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
	m_glesAlphaThreshold = (m_glesAlphaCutout && m_state.isBlendEnabled()) ? 0.0f : 0.5f;
	for(size_t i = 0; i < m_TextureStages.size(); ++i) {
		GetTextureStage(i)->apply();
	}
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

static GLuint compileGLESShader(GLenum type, const char * source) {
	GLuint shader = glCreateShader(type);
	if(!shader) {
		LogError << "Could not create GLES shader";
		return 0;
	}
	glShaderSource(shader, 1, &source, nullptr);
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

static const char * gles2VertexShader = R"(
precision highp float;
attribute vec4 aPosition;
attribute vec4 aColor;
attribute vec2 aTexCoord0;
attribute vec2 aTexCoord1;
attribute vec2 aTexCoord2;
uniform mat4 uProjection;
uniform mat4 uView;
uniform int uTransform;
varying vec4 vColor;
varying vec2 vTexCoord0;
varying vec2 vTexCoord1;
varying vec2 vTexCoord2;
varying float vFogDistance;
void main() {
    if(uTransform != 0) {
        vec4 viewPosition = uView * vec4(aPosition.xyz, 1.0);
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

static const char * gles2FragmentShader = R"(
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D uTexture0;
uniform sampler2D uTexture1;
uniform sampler2D uTexture2;
uniform int uUseTexture0;
uniform int uUseTexture1;
uniform int uUseTexture2;
uniform int uColorOp0;
uniform int uColorOp1;
uniform int uColorOp2;
uniform int uAlphaOp0;
uniform int uAlphaOp1;
uniform int uAlphaOp2;
uniform int uFogEnabled;
uniform vec4 uFogColor;
uniform vec2 uFogRange;
uniform int uAlphaCutout;
uniform float uAlphaThreshold;
varying vec4 vColor;
varying vec2 vTexCoord0;
varying vec2 vTexCoord1;
varying vec2 vTexCoord2;
varying float vFogDistance;
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
    return previous;
}
void main() {
    vec4 color = vColor;
    if(uUseTexture0 != 0) color = applyStage(color, texture2D(uTexture0, vTexCoord0), uColorOp0, uAlphaOp0);
    if(uUseTexture1 != 0) color = applyStage(color, texture2D(uTexture1, vTexCoord1), uColorOp1, uAlphaOp1);
    if(uUseTexture2 != 0) color = applyStage(color, texture2D(uTexture2, vTexCoord2), uColorOp2, uAlphaOp2);
    if(uAlphaCutout != 0 && color.a < uAlphaThreshold) discard;
    if(uFogEnabled != 0) {
        float fog = clamp((uFogRange.y - vFogDistance) / max(uFogRange.y - uFogRange.x, 0.0001), 0.0, 1.0);
        color.rgb = mix(uFogColor.rgb, color.rgb, fog);
    }
    gl_FragColor = color;
}
)";

static const char * gles3VertexShader = R"(
#version 300 es
precision highp float;
in vec4 aPosition;
in vec4 aColor;
in vec2 aTexCoord0;
in vec2 aTexCoord1;
in vec2 aTexCoord2;
uniform mat4 uProjection;
uniform mat4 uView;
uniform int uTransform;
out vec4 vColor;
out vec2 vTexCoord0;
out vec2 vTexCoord1;
out vec2 vTexCoord2;
out float vFogDistance;
void main() {
    if(uTransform != 0) {
        vec4 viewPosition = uView * vec4(aPosition.xyz, 1.0);
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

static const char * gles3FragmentShader = R"(
#version 300 es
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform sampler2D uTexture0;
uniform sampler2D uTexture1;
uniform sampler2D uTexture2;
uniform int uUseTexture0;
uniform int uUseTexture1;
uniform int uUseTexture2;
uniform int uColorOp0;
uniform int uColorOp1;
uniform int uColorOp2;
uniform int uAlphaOp0;
uniform int uAlphaOp1;
uniform int uAlphaOp2;
uniform int uFogEnabled;
uniform vec4 uFogColor;
uniform vec2 uFogRange;
uniform int uAlphaCutout;
uniform float uAlphaThreshold;
in vec4 vColor;
in vec2 vTexCoord0;
in vec2 vTexCoord1;
in vec2 vTexCoord2;
in float vFogDistance;
out vec4 fragColor;
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
    return previous;
}
void main() {
    vec4 color = vColor;
    if(uUseTexture0 != 0) color = applyStage(color, texture(uTexture0, vTexCoord0), uColorOp0, uAlphaOp0);
    if(uUseTexture1 != 0) color = applyStage(color, texture(uTexture1, vTexCoord1), uColorOp1, uAlphaOp1);
    if(uUseTexture2 != 0) color = applyStage(color, texture(uTexture2, vTexCoord2), uColorOp2, uAlphaOp2);
    if(uAlphaCutout != 0 && color.a < uAlphaThreshold) discard;
    if(uFogEnabled != 0) {
        float fog = clamp((uFogRange.y - vFogDistance) / max(uFogRange.y - uFogRange.x, 0.0001), 0.0, 1.0);
        color.rgb = mix(uFogColor.rgb, color.rgb, fog);
    }
    fragColor = color;
}
)";

} // namespace

void OpenGLRenderer::initializeGLESProgram() {
	GLuint vertex = compileGLESShader(GL_VERTEX_SHADER, g_useGLES2_0 ? gles2VertexShader : gles3VertexShader);
	GLuint fragment = compileGLESShader(GL_FRAGMENT_SHADER, g_useGLES2_0 ? gles2FragmentShader : gles3FragmentShader);
	if(!vertex || !fragment) {
		if(vertex) glDeleteShader(vertex);
		if(fragment) glDeleteShader(fragment);
		return;
	}
	GLuint program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glBindAttribLocation(program, 0, "aPosition");
	glBindAttribLocation(program, 1, "aColor");
	glBindAttribLocation(program, 2, "aTexCoord0");
	glBindAttribLocation(program, 3, "aTexCoord1");
	glBindAttribLocation(program, 4, "aTexCoord2");
	glLinkProgram(program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	GLint linked = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if(linked != GL_TRUE) {
		GLint length = 0;
		glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
		std::string log(size_t(std::max(1, length)), '\0');
		glGetProgramInfoLog(program, GLsizei(log.size()), nullptr, log.data());
		LogError << "GLES program link failed: " << log;
		glDeleteProgram(program);
		return;
	}
	shutdownGLESProgram();
	m_glesProgram = program;
	m_glesUniformProjection = glGetUniformLocation(program, "uProjection");
	m_glesUniformView = glGetUniformLocation(program, "uView");
	m_glesUniformTransform = glGetUniformLocation(program, "uTransform");
	m_glesUniformFogEnabled = glGetUniformLocation(program, "uFogEnabled");
	m_glesUniformFogColor = glGetUniformLocation(program, "uFogColor");
	m_glesUniformFogRange = glGetUniformLocation(program, "uFogRange");
	m_glesUniformAlphaCutout = glGetUniformLocation(program, "uAlphaCutout");
	m_glesUniformAlphaThreshold = glGetUniformLocation(program, "uAlphaThreshold");
	glUseProgram(program);
	const char * samplerNames[3] = {"uTexture0", "uTexture1", "uTexture2"};
	const char * useNames[3] = {"uUseTexture0", "uUseTexture1", "uUseTexture2"};
	const char * colorNames[3] = {"uColorOp0", "uColorOp1", "uColorOp2"};
	const char * alphaNames[3] = {"uAlphaOp0", "uAlphaOp1", "uAlphaOp2"};
	for(GLint i = 0; i < 3; ++i) {
		GLint sampler = glGetUniformLocation(program, samplerNames[i]);
		if(sampler >= 0) glUniform1i(sampler, i);
		m_glesUniformUseTexture[i] = glGetUniformLocation(program, useNames[i]);
		m_glesUniformColorOp[i] = glGetUniformLocation(program, colorNames[i]);
		m_glesUniformAlphaOp[i] = glGetUniformLocation(program, alphaNames[i]);
	}
	glUseProgram(0);
}

void OpenGLRenderer::shutdownGLESProgram() {
	if(m_glesProgram) {
		glDeleteProgram(m_glesProgram);
		m_glesProgram = 0;
	}
}

void OpenGLRenderer::prepareGLESProgram(bool transformed) {
	if(!m_glesProgram) return;
	glUseProgram(m_glesProgram);
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
	if(m_glesUniformProjection >= 0) glUniformMatrix4fv(m_glesUniformProjection, 1, GL_FALSE, glm::value_ptr(projection));
	if(m_glesUniformView >= 0) glUniformMatrix4fv(m_glesUniformView, 1, GL_FALSE, glm::value_ptr(view));
	if(m_glesUniformTransform >= 0) glUniform1i(m_glesUniformTransform, transformed ? 1 : 0);
	if(m_glesUniformFogEnabled >= 0) glUniform1i(m_glesUniformFogEnabled, m_state.getFog() ? 1 : 0);
	Color4f fog(m_glesFogColor);
	if(m_glesUniformFogColor >= 0) glUniform4f(m_glesUniformFogColor, fog.r, fog.g, fog.b, fog.a);
	if(m_glesUniformFogRange >= 0) glUniform2f(m_glesUniformFogRange, m_glesFogStart, m_glesFogEnd);
	if(m_glesUniformAlphaCutout >= 0) glUniform1i(m_glesUniformAlphaCutout, m_glesAlphaCutout ? 1 : 0);
	if(m_glesUniformAlphaThreshold >= 0) glUniform1f(m_glesUniformAlphaThreshold, m_glesAlphaThreshold);
	for(size_t i = 0; i < 3; ++i) {
		GLTextureStage * stage = i < m_TextureStages.size() ? GetTextureStage(i) : nullptr;
		if(m_glesUniformUseTexture[i] >= 0) glUniform1i(m_glesUniformUseTexture[i], stage && stage->getTexture() ? 1 : 0);
		if(m_glesUniformColorOp[i] >= 0) glUniform1i(m_glesUniformColorOp[i], stage ? GLint(stage->getColorOp()) : GLint(TextureStage::OpDisable));
		if(m_glesUniformAlphaOp[i] >= 0) glUniform1i(m_glesUniformAlphaOp[i], stage ? GLint(stage->getAlphaOp()) : GLint(TextureStage::OpDisable));
	}
}
#endif

