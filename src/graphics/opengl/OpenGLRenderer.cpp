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

#include <cmath>
#include <cstring>
#include <algorithm>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#ifdef ANDROID
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>
#endif

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
	// Feature level: the legacy OpenGL ES 2.0 mode uses OpenGL ES 2.0 and its extensions
	// only, even when the driver created a newer context (Mali and Adreno always do).
	m_glesIsES3 = gl.isES() && gl.is(3, 0) && !g_useGLES2_0;
	LogInfo << "OpenGL ES feature level: " << (m_glesIsES3 ? "3.0" : "2.0");
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
#ifdef ANDROID
		m_hasTextureNPOT = m_glesIsES3 || gl.has("GL_OES_texture_npot");
#else
		m_hasTextureNPOT = gl.is(3, 0) || gl.has("GL_OES_texture_npot");
#endif
		if(!m_hasTextureNPOT) {
			LogWarning << "Missing OpenGL extension GL_OES_texture_npot";
		}
#ifdef ANDROID
		m_hasSizedTextureFormats = m_glesIsES3 || gl.has("GL_OES_required_internalformat");
#else
		m_hasSizedTextureFormats = gl.has("GL_OES_required_internalformat", 3, 0);
#endif
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
		// The OpenGL ES 2.0 feature level moves the attribute pointers instead.
		m_hasDrawElementsBaseVertex = m_glesIsES3 && hasBaseVertexExtension
		        && (glDrawElementsBaseVertexOES != nullptr
		            || glDrawElementsBaseVertexEXT != nullptr);
		if(m_glesIsES3 && hasBaseVertexExtension && !m_hasDrawElementsBaseVertex) {
			LogWarning << "Base-vertex extension advertised without a usable GLES entry point";
		}
		m_hasDrawRangeElements = m_glesIsES3;
		if(m_hasDrawElementsBaseVertex) {
			LogInfo << "GLES indexed draws: "
			        << (glDrawElementsBaseVertexOES ? "GL_OES_draw_elements_base_vertex" :
			                                            "GL_EXT_draw_elements_base_vertex");
		} else {
			LogInfo << "GLES indexed draws: shifted vertex attributes";
		}
#else
		m_hasDrawElementsBaseVertex = hasBaseVertexExtension;
		m_hasDrawRangeElements = gl.is(3, 0);
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
		m_hasCoreMapBufferRange = m_glesIsES3 && glMapBufferRange && glUnmapBuffer;
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
		LogInfo << "GLES vertex and index upload: "
		        << (m_hasMapBufferRange ? "unsynchronized glMapBufferRange + orphaning"
		                                : "client memory streaming");

		m_glesDepthFormat = (m_glesIsES3 || gl.has("GL_OES_depth24")) ? GL_DEPTH_COMPONENT24_OES
		                                                               : GL_DEPTH_COMPONENT16;
		
		// Attachment invalidation at the end of render passes
		m_hasInvalidateFramebuffer = m_glesIsES3 && glInvalidateFramebuffer;
		m_hasDiscardFramebuffer = gl.has("GL_EXT_discard_framebuffer") && glDiscardFramebufferEXT;

		// Multisampling with the samples resolved in tile memory:
		//  - GL_EXT_multisampled_render_to_texture renders into a single-sampled texture
		//    (native Mali, Adreno and PowerVR drivers),
		//  - without it (ANGLE does not expose the extension on Mali) a multisampled
		//    framebuffer is resolved into the window with glBlitFramebuffer(): ANGLE turns
		//    a whole-framebuffer resolve into a resolve attachment of the open render pass.
		m_glesMaxMSAASamples = 0;
		m_glesMSAAResolveBlit = false;
		m_glesMSAABlitANGLE = false;
		if(gl.has("GL_EXT_multisampled_render_to_texture")
		   && glFramebufferTexture2DMultisampleEXT && glRenderbufferStorageMultisampleEXT) {
			GLint maxSamples = 0;
			glGetIntegerv(GL_MAX_SAMPLES_EXT, &maxSamples);
			m_glesMaxMSAASamples = std::max(0, int(maxSamples));
		} else if(m_glesIsES3 && glRenderbufferStorageMultisample && glBlitFramebuffer
		          && glGetFramebufferAttachmentParameteriv) {
			GLint maxSamples = 0;
			glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
			m_glesMaxMSAASamples = std::max(0, int(maxSamples));
			m_glesMSAAResolveBlit = true;
		} else if(gl.has("GL_ANGLE_framebuffer_multisample") && gl.has("GL_ANGLE_framebuffer_blit")
		          && gl.has("GL_OES_rgb8_rgba8")
		          && glRenderbufferStorageMultisampleANGLE && glBlitFramebufferANGLE) {
			// OpenGL ES 2.0 feature level under ANGLE: the same multisampled framebuffer and
			// resolve through GL_ANGLE_framebuffer_multisample and GL_ANGLE_framebuffer_blit.
			GLint maxSamples = 0;
			glGetIntegerv(GL_MAX_SAMPLES_ANGLE, &maxSamples);
			m_glesMaxMSAASamples = std::max(0, int(maxSamples));
			m_glesMSAAResolveBlit = true;
			m_glesMSAABlitANGLE = true;
		}
		if(m_glesMSAAResolveBlit) {
			// The resolve can only become a resolve attachment if the multisampled color
			// format matches the window surface. Query it the way the actual context
			// version allows: OpenGL ES 3.0 describes the window as attachments of the
			// default framebuffer, OpenGL ES 2.0 has the x_BITS state.
			GLint red = 0, green = 0, blue = 0, alpha = 0;
			if(gl.is(3, 0)) {
				glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &red);
				glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK, GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE, &green);
				glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK, GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE, &blue);
				glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK, GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE, &alpha);
			} else {
				glGetIntegerv(GL_RED_BITS, &red);
				glGetIntegerv(GL_GREEN_BITS, &green);
				glGetIntegerv(GL_BLUE_BITS, &blue);
				glGetIntegerv(GL_ALPHA_BITS, &alpha);
			}
			if(red == 5 && green == 6 && blue == 5 && alpha == 0) {
				m_glesWindowColorFormat = GL_RGB565;
			} else if(alpha == 0) {
				m_glesWindowColorFormat = GL_RGB8_OES;
			} else {
				m_glesWindowColorFormat = GL_RGBA8_OES;
			}
		}
		LogInfo << "GLES render passes: "
		        << (m_hasInvalidateFramebuffer ? "glInvalidateFramebuffer"
		            : m_hasDiscardFramebuffer ? "glDiscardFramebufferEXT" : "no invalidation")
		        << ", MSAA: "
		        << (m_glesMaxMSAASamples <= 1 ? "not supported"
		            : m_glesMSAABlitANGLE ? "multisampled framebuffer + glBlitFramebufferANGLE resolve"
		            : m_glesMSAAResolveBlit ? "multisampled framebuffer + glBlitFramebuffer resolve"
		            : "GL_EXT_multisampled_render_to_texture");
		
		// Program binaries for the shader cache (OpenGL ES 3.0 feature level only)
		m_glesProgramBinaryFormats.clear();
		if(m_glesIsES3 && glGetProgramBinary && glProgramBinary && glProgramParameteri) {
			GLint count = 0;
			glGetIntegerv(GL_NUM_PROGRAM_BINARY_FORMATS, &count);
			if(count > 0) {
				m_glesProgramBinaryFormats.resize(size_t(count));
				glGetIntegerv(GL_PROGRAM_BINARY_FORMATS, m_glesProgramBinaryFormats.data());
			}
		}
		LogInfo << "GLES program binaries: "
		        << (m_glesProgramBinaryFormats.empty() ? std::string("not supported")
		            : std::to_string(m_glesProgramBinaryFormats.size()) + " format(s)");
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
#ifdef ANDROID
		// Per-sample shading for crisp alpha cutout antialiasing
		m_hasSampleShading = m_glesIsES3 && gl.has("GL_OES_sample_shading") && glMinSampleShadingOES;
		LogInfo << "GLES sample shading: " << (m_hasSampleShading ? "GL_OES_sample_shading" : "not supported");
#else
		m_hasSampleShading = gl.has("GL_OES_sample_shading", 3, 2);
#endif
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
	// The window surface is never multisampled: MSAA is done in m_glesMSAAFramebuffer,
	// which beginFrame() creates and binds. A new context starts without any of the
	// renderer's objects.
	m_MSAALevel = 0;
	m_hasMSAA = false;
	m_glesMSAAFramebuffer = 0;
	m_glesMSAAColor = 0;
	m_glesMSAAColorBuffer = 0;
	m_glesMSAADepth = 0;
	m_glesMSAASamples = 0;
	m_glesMSAASize = Vec2i(0);
	m_glesMSAAFailed = false;
	m_glesIndexBuffer = 0;
	m_glesIndexBufferCapacity = 0;
	m_glesIndexBufferOffset = 0;
	m_glesImmediateVertexOffset = 0;
	m_glalphaToCoverage = false;
	glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
	m_glsampleShading = false;
	if(hasSampleShading()) {
		glDisable(GL_SAMPLE_SHADING_OES);
		glMinSampleShadingOES(1.f);
	}
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
	if(hasMapBufferRange()) {
		m_glesImmediateVertices = createVertexBufferTL(16 * 1024, Stream);
	}
	beginFrame();
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
	destroyMultisampleFramebuffer();
	m_glesImmediateVertices.reset();
	if(m_glesIndexBuffer) {
		unbindIndexBuffer(m_glesIndexBuffer);
		glDeleteBuffers(1, &m_glesIndexBuffer);
		m_glesIndexBuffer = 0;
	}
	m_glesIndexBufferCapacity = 0;
	m_glesIndexBufferOffset = 0;
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

#ifdef ANDROID
	if(!m_glesPassStarted) {
		if(nrects == 0) {
			// A full clear before the first draw becomes the load operation of the
			// render pass: tile memory is initialized, nothing is read from memory.
			if(bufferFlags & ColorBuffer) {
				m_glesPassCleared |= GL_COLOR_BUFFER_BIT;
			}
			if(bufferFlags & DepthBuffer) {
				m_glesPassCleared |= GL_DEPTH_BUFFER_BIT;
			}
		} else {
			// Partial clears keep the rest of the attachments: start the pass first.
			startRenderPass();
		}
	}
#endif

	GLbitfield buffers = 0;
	
	if(bufferFlags & ColorBuffer) {
		Color4f colorf(clearColor);
#ifdef ANDROID
		if(std::abs(m_glesGamma - 1.f) > 0.001f) {
			// The brightness curve of glesFragmentShader for the background as well: it is
			// the fog color, and without the curve it showed through the fog as a dark (or
			// light) hole wherever nothing is drawn past the far plane.
			const float invGamma = 1.f / m_glesGamma;
			colorf.r = std::pow(colorf.r, invGamma);
			colorf.g = std::pow(colorf.g, invGamma);
			colorf.b = std::pow(colorf.b, invGamma);
		}
#endif
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
	// The video options slider gives 0.5 (darker) to 1.5 (brighter), 1 is neutral.
	// It is applied as a gamma curve like the desktop window gamma ramp (and gl4es),
	// see glesFragmentShader.
	m_glesGamma = glm::clamp(gamma, 0.1f, 4.f);
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
	if(m_glesMaxMSAASamples <= 1) {
		return NoAlphaCutoutAA;
	}
	return hasSampleShading() ? CrispAlphaCutoutAA : FuzzyAlphaCutoutAA;
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
	
#ifdef ANDROID
	
	if(nvertices == 0 || nindices == 0) {
		return;
	}
	
	if(m_glesImmediateVertices && nvertices <= m_glesImmediateVertices->capacity()) {
		// Append to a streaming vertex buffer with unsynchronized maps, orphan it on wrap.
		size_t offset = m_glesImmediateVertexOffset;
		if(offset + nvertices > m_glesImmediateVertices->capacity()) {
			offset = 0;
		}
		m_glesImmediateVertices->setData(vertices, nvertices, offset, offset ? NoOverwrite : DiscardBuffer);
		m_glesImmediateVertices->drawIndexed(primitive, nvertices, offset, indices, nindices);
		m_glesImmediateVertexOffset = offset + nvertices;
		return;
	}
	
	// No buffer mapping: let the driver copy the vertices from client memory.
	beforeDraw<TexturedVertex>();
	bindBuffer(GL_NONE);
	setVertexArray(this, vertices, vertices);
	const void * indexData = streamIndices(indices, nindices);
	drawIndexedElements(primitive, indexData, nindices, 0, nvertices);
	
#else
	
	beforeDraw<TexturedVertex>();
	
	bindBuffer(GL_NONE);
	setVertexArray(this, vertices, vertices);
	if(hasDrawRangeElements()) {
		glDrawRangeElements(arxToGlPrimitiveType[primitive], 0, nvertices - 1, nindices, GL_UNSIGNED_SHORT, indices);
	} else {
		glDrawElements(arxToGlPrimitiveType[primitive], nindices, GL_UNSIGNED_SHORT, indices);
	}
	
#endif
	
}

#ifdef ANDROID

const void * OpenGLRenderer::streamIndices(const unsigned short * indices, size_t count) {
	
	const size_t bytes = count * sizeof(unsigned short);
	
	if(!hasMapBufferRange() || bytes == 0) {
		// Client memory indices require that no element array buffer is bound.
		bindIndexBuffer(GL_NONE);
		return indices;
	}
	
	if(!m_glesIndexBuffer) {
		glGenBuffers(1, &m_glesIndexBuffer);
	}
	bindIndexBuffer(m_glesIndexBuffer);
	
	// Indices are appended to one buffer. Queued draws may still read everything before
	// the write position, so only unused space is written, without synchronization.
	// When the buffer is full it is orphaned with glBufferData(): the driver hands out
	// new storage and keeps the old one alive for the queued draws.
	size_t offset = (m_glesIndexBufferOffset + 3) & ~size_t(3);
	if(offset + bytes > m_glesIndexBufferCapacity) {
		size_t capacity = std::max(m_glesIndexBufferCapacity, size_t(1024 * 1024));
		while(capacity < bytes) {
			capacity *= 2;
		}
		m_glesIndexBufferCapacity = capacity;
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, GLsizeiptr(capacity), nullptr, GL_STREAM_DRAW);
		offset = 0;
	}
	
	void * buffer = mapBufferRange(GL_ELEMENT_ARRAY_BUFFER, offset, bytes,
	                               GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT);
	if(!buffer) {
		LogError << "Could not map index buffer!";
		bindIndexBuffer(GL_NONE);
		return indices;
	}
	std::memcpy(buffer, indices, bytes);
	unmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
	
	m_glesIndexBufferOffset = offset + bytes;
	
	return reinterpret_cast<const void *>(offset);
}

void OpenGLRenderer::drawIndexedElements(Primitive primitive, const void * indices, size_t nindices,
                                         size_t baseVertex, size_t vertexCount) {
	
	if(nindices == 0) {
		return;
	}
	arx_assert(nindices <= size_t(std::numeric_limits<GLsizei>::max()));
	arx_assert(baseVertex <= size_t(std::numeric_limits<GLint>::max()));
	
	const GLenum mode = arxToGlPrimitiveType[primitive];
	const GLsizei count = GLsizei(nindices);
	
	// Passing the vertex range saves the driver from scanning the indices.
	const bool range = hasDrawRangeElements() && vertexCount > 0;
	const GLuint end = range ? GLuint(vertexCount - 1) : 0;
	
	if(baseVertex != 0) {
		// Only reached when hasDrawElementsBaseVertex() is true, otherwise
		// BaseGLVertexBuffer::drawIndexed() moves the attribute pointers instead.
		arx_assert(hasDrawElementsBaseVertex());
		const GLint base = GLint(baseVertex);
		if(range && glDrawRangeElementsBaseVertexOES) {
			glDrawRangeElementsBaseVertexOES(mode, 0, end, count, GL_UNSIGNED_SHORT, indices, base);
		} else if(range && glDrawRangeElementsBaseVertexEXT) {
			glDrawRangeElementsBaseVertexEXT(mode, 0, end, count, GL_UNSIGNED_SHORT, indices, base);
		} else if(glDrawElementsBaseVertexOES) {
			glDrawElementsBaseVertexOES(mode, count, GL_UNSIGNED_SHORT, indices, base);
		} else {
			glDrawElementsBaseVertexEXT(mode, count, GL_UNSIGNED_SHORT, indices, base);
		}
		return;
	}
	
	if(range) {
		glDrawRangeElements(mode, 0, end, count, GL_UNSIGNED_SHORT, indices);
	} else {
		glDrawElements(mode, count, GL_UNSIGNED_SHORT, indices);
	}
	
}

#endif // ANDROID

bool OpenGLRenderer::getSnapshot(Image & image) {
	
	Vec2i size = mainApp->getWindow()->getSize();
	
	image.create(size_t(size.x), size_t(size.y), Image::Format_R8G8B8);
#ifdef ANDROID
	// GLES 2 only guarantees RGBA readback. Strip alpha after reading to keep
	// the renderer's existing RGB Image contract.
	Image rgba;
	rgba.create(size_t(size.x), size_t(size.y), Image::Format_R8G8B8A8);
	// Multisampled framebuffers cannot be read: use the window, which holds the last
	// resolved frame, like when rendering without MSAA.
	const bool readWindow = m_glesMSAAResolveBlit && m_glesMSAAFramebuffer;
	if(readWindow) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, GL_NONE);
	}
	glReadPixels(0, 0, size.x, size.y, GL_RGBA, GL_UNSIGNED_BYTE, rgba.getData());
	if(readWindow) {
		glBindFramebuffer(GL_READ_FRAMEBUFFER, m_glesMSAAFramebuffer);
	}
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
	// Alpha cutouts like the desktop renderer: with MSAA either per-sample shading
	// (crisp) or alpha-to-coverage (fuzzy), otherwise texels at or below alpha 0.5 are
	// discarded. Blended cutouts only discard fully transparent texels.
	const int cutoutAA = config.video.alphaCutoutAntialiasing;
	const bool useSampleShading = m_hasMSAA && hasSampleShading() && cutoutAA == int(CrispAlphaCutoutAA);
	const bool useAlphaToCoverage = m_hasMSAA && cutoutAA == int(FuzzyAlphaCutoutAA);
	bool sampleShading = false;
	bool alphaToCoverage = false;
	m_glesAlphaThreshold = 0.5f;
	if(m_state.getAlphaCutout()) {
		if(srcFactor == BlendOne && dstFactor == BlendZero) {
			if(useSampleShading) {
				sampleShading = true;
			} else if(useAlphaToCoverage) {
				alphaToCoverage = true;
				m_glesAlphaThreshold = 0.f;
			}
		} else if(srcFactor == BlendOne && dstFactor == BlendOne) {
			srcFactor = BlendSrcAlpha;
			m_glesAlphaThreshold = 0.f;
		} else if(srcFactor == BlendSrcAlpha || dstFactor == BlendInvSrcAlpha) {
			m_glesAlphaThreshold = 0.f;
		} else {
			sampleShading = useSampleShading;
		}
	}
	if(alphaToCoverage != m_glalphaToCoverage) {
		if(alphaToCoverage) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
		else glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
		m_glalphaToCoverage = alphaToCoverage;
	}
	if(sampleShading != m_glsampleShading) {
		if(sampleShading) glEnable(GL_SAMPLE_SHADING_OES);
		else glDisable(GL_SAMPLE_SHADING_OES);
		m_glsampleShading = sampleShading;
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
	// GL_TEXTURE0 is always the active unit outside of GLTextureStage::apply().
	for(size_t i = 0; i < m_TextureStages.size() && i <= maxTextureStage; ++i) {
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

//! MSAA sample count, resolved in tile memory by GL_EXT_multisampled_render_to_texture
const GLsizei glesMSAASamples = 2;

//! Texture unit for copying the resolved MSAA image, above the renderer's texture stages
const GLuint glesCopyTextureUnit = 3;

//! Number of texture stages compiled into each stage bucket of the shader variants
const int glesStageCounts[3] = { 0, 1, 3 };

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

//! Directory of the program binary cache, set from Java through JNA (empty: no cache)
std::string g_glesProgramCacheDir;

/*
 * Program binary cache (OpenGL ES 3.0 feature level)
 *
 * One file per program, named after a hash of the shader sources and the driver
 * identification (vendor, renderer, version strings). The driver can still reject a
 * binary, for example after an update that kept the version strings: the program is
 * then compiled from source again and the file replaced.
 */
const char glesProgramCacheMagic[4] = { 'A', 'R', 'X', 'P' };
const u32 glesProgramCacheVersion = 1;
const u32 glesProgramCacheMaxSize = 64 * 1024 * 1024;

struct GLESProgramCacheHeader {
	char magic[4];
	u32 version;
	u64 key;
	u32 format;
	u32 size;
};

struct GLESProgramCache {
	std::string directory;
	u64 driverHash = 0;
	const std::vector<GLint> * formats = nullptr;
	std::vector<std::string> usedFiles;
	size_t loaded = 0;
	size_t compiled = 0;
};

u64 hashBytes(u64 hash, const void * data, size_t size) {
	const unsigned char * bytes = static_cast<const unsigned char *>(data);
	for(size_t i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

u64 hashString(u64 hash, const char * string) {
	if(string) {
		hash = hashBytes(hash, string, std::strlen(string));
	}
	return hashBytes(hash, "", 1);
}

std::string glesProgramCacheFile(u64 key) {
	char name[32];
	std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(key));
	return name;
}

GLuint loadCachedGLESProgram(const GLESProgramCache & cache, const std::string & file, u64 key) {

	const std::string path = cache.directory + "/" + file;
	FILE * stream = std::fopen(path.c_str(), "rb");
	if(!stream) {
		return 0;
	}

	GLESProgramCacheHeader header;
	std::vector<char> binary;
	// Only formats the driver lists may be passed to glProgramBinary(): anything else is
	// an error, and errors are undefined behavior in a no-error context.
	bool valid = std::fread(&header, sizeof(header), 1, stream) == 1
	             && std::memcmp(header.magic, glesProgramCacheMagic, sizeof(header.magic)) == 0
	             && header.version == glesProgramCacheVersion && header.key == key
	             && header.size > 0 && header.size <= glesProgramCacheMaxSize
	             && std::find(cache.formats->begin(), cache.formats->end(), GLint(header.format))
	                != cache.formats->end();
	if(valid) {
		binary.resize(header.size);
		valid = std::fread(binary.data(), 1, binary.size(), stream) == binary.size();
	}
	std::fclose(stream);
	if(!valid) {
		std::remove(path.c_str());
		return 0;
	}

	GLuint program = glCreateProgram();
	if(!program) {
		return 0;
	}
	glProgramBinary(program, GLenum(header.format), binary.data(), GLsizei(binary.size()));
	GLint linked = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if(linked != GL_TRUE) {
		glDeleteProgram(program);
		std::remove(path.c_str());
		return 0;
	}

	return program;
}

void saveCachedGLESProgram(const GLESProgramCache & cache, GLuint program, const std::string & file,
                           u64 key) {

	GLint length = 0;
	glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
	if(length <= 0 || u32(length) > glesProgramCacheMaxSize) {
		return;
	}

	std::vector<char> binary(static_cast<size_t>(length));
	GLsizei written = 0;
	GLenum format = GL_NONE;
	glGetProgramBinary(program, length, &written, &format, binary.data());
	if(written <= 0) {
		return;
	}

	GLESProgramCacheHeader header;
	std::memcpy(header.magic, glesProgramCacheMagic, sizeof(header.magic));
	header.version = glesProgramCacheVersion;
	header.key = key;
	header.format = u32(format);
	header.size = u32(written);

	// Write to a temporary file first so an interrupted write never leaves a broken binary.
	const std::string path = cache.directory + "/" + file;
	const std::string temporary = path + ".tmp";
	FILE * stream = std::fopen(temporary.c_str(), "wb");
	if(!stream) {
		return;
	}
	bool ok = std::fwrite(&header, sizeof(header), 1, stream) == 1
	                  && std::fwrite(binary.data(), 1, size_t(written), stream) == size_t(written);
	ok = (std::fclose(stream) == 0) && ok;
	if(!ok || std::rename(temporary.c_str(), path.c_str()) != 0) {
		std::remove(temporary.c_str());
	}

}

//! Remove binaries of other shader sources or driver versions
void cleanGLESProgramCache(const GLESProgramCache & cache) {

	DIR * directory = opendir(cache.directory.c_str());
	if(!directory) {
		return;
	}

	while(dirent * entry = readdir(directory)) {
		const std::string name = entry->d_name;
		const bool binary = name.size() == 20 && name.compare(16, 4, ".bin") == 0;
		const bool temporary = name.size() == 24 && name.compare(16, 8, ".bin.tmp") == 0;
		if((!binary && !temporary) || name.find_first_not_of("0123456789abcdef") != 16) {
			continue;
		}
		if(temporary || std::find(cache.usedFiles.begin(), cache.usedFiles.end(), name) == cache.usedFiles.end()) {
			std::remove((cache.directory + "/" + name).c_str());
		}
	}

	closedir(directory);
}

GLuint buildGLESProgram(const std::string & vertexSource, const std::string & fragmentSource,
                        GLESProgramCache * cache) {

	u64 key = 0;
	std::string file;
	if(cache) {
		key = hashBytes(cache->driverHash, vertexSource.data(), vertexSource.size());
		key = hashBytes(key, "", 1);
		key = hashBytes(key, fragmentSource.data(), fragmentSource.size());
		file = glesProgramCacheFile(key);
		cache->usedFiles.push_back(file);
		if(GLuint program = loadCachedGLESProgram(*cache, file, key)) {
			cache->loaded++;
			return program;
		}
	}

	GLuint vertex = compileGLESShader(GL_VERTEX_SHADER, vertexSource);
	if(!vertex) {
		return 0;
	}
	GLuint fragment = compileGLESShader(GL_FRAGMENT_SHADER, fragmentSource);
	if(!fragment) {
		glDeleteShader(vertex);
		return 0;
	}

	GLuint program = glCreateProgram();
	if(!program) {
		LogError << "Could not create GLES shader program";
		glDeleteShader(vertex);
		glDeleteShader(fragment);
		return 0;
	}
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glBindAttribLocation(program, 0, "aPosition");
	glBindAttribLocation(program, 1, "aColor");
	glBindAttribLocation(program, 2, "aTexCoord0");
	glBindAttribLocation(program, 3, "aTexCoord1");
	glBindAttribLocation(program, 4, "aTexCoord2");
	if(cache) {
		glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
	}
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
		return 0;
	}

	if(cache) {
		saveCachedGLESProgram(*cache, program, file, key);
		cache->compiled++;
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

/*
 * Variants: ARX_STAGES (0, 1 or 3 texture stages), ARX_ALPHA_CUTOUT, ARX_FOG and ARX_GAMMA.
 * Nothing in the shaders branches on uniforms, values that only depend on uniforms
 * (MVP matrix, fog plane and scale, texture combiner coefficients) come from the CPU.
 * Linear fog is evaluated per vertex: the fog factor is affine in the vertex position,
 * so interpolating it is exact and only the clamp is left to the fragment shader.
 */
const char * const glesVertexShader = R"(
precision highp float;
ARX_VS_IN highp vec4 aPosition;
ARX_VS_IN mediump vec4 aColor;
uniform highp mat4 uMVP;
ARX_VARYING_OUT mediump vec4 vColor;
#if ARX_STAGES >= 1
ARX_VS_IN highp vec2 aTexCoord0;
ARX_VARYING_OUT highp vec2 vTexCoord0;
#endif
#if ARX_STAGES >= 3
ARX_VS_IN highp vec2 aTexCoord1;
ARX_VS_IN highp vec2 aTexCoord2;
ARX_VARYING_OUT highp vec2 vTexCoord1;
ARX_VARYING_OUT highp vec2 vTexCoord2;
#endif
#ifdef ARX_FOG
uniform highp vec4 uFogPlane;
uniform highp vec2 uFogParams;
ARX_VARYING_OUT mediump float vFog;
#endif
void main() {
    gl_Position = uMVP * aPosition;
    vColor = aColor;
#if ARX_STAGES >= 1
    vTexCoord0 = aTexCoord0;
#endif
#if ARX_STAGES >= 3
    vTexCoord1 = aTexCoord1;
    vTexCoord2 = aTexCoord2;
#endif
#ifdef ARX_FOG
    vFog = (uFogParams.x - dot(uFogPlane, aPosition)) * uFogParams.y;
#endif
}
)";

/*
 * Texture stages implement the fixed-function combiner as
 *   result = clamp(previous * A + texel * B + previous * texel * C, 0, 1)
 * with per-stage coefficients (rgb for the color operation, a for the alpha operation).
 * Color math runs in mediump (FP16 on Mali), texture coordinates stay highp.
 */
const char * const glesFragmentShader = R"(
precision mediump float;
#ifdef GL_FRAGMENT_PRECISION_HIGH
#define ARX_HIGHP highp
#else
#define ARX_HIGHP mediump
#endif
#ifdef ARX_GLSL_300
out vec4 arxFragColor;
#endif
ARX_VARYING_IN mediump vec4 vColor;
#ifdef ARX_GAMMA
uniform float uInvGamma;
uniform float uGammaInverted;
#endif
#if ARX_STAGES >= 1
uniform sampler2D uTexture0;
uniform vec4 uStageA0;
uniform vec4 uStageB0;
uniform vec4 uStageC0;
uniform float uLodBias0;
ARX_VARYING_IN ARX_HIGHP vec2 vTexCoord0;
#endif
#if ARX_STAGES >= 3
uniform sampler2D uTexture1;
uniform sampler2D uTexture2;
uniform vec4 uStageA1;
uniform vec4 uStageB1;
uniform vec4 uStageC1;
uniform vec4 uStageA2;
uniform vec4 uStageB2;
uniform vec4 uStageC2;
uniform float uLodBias1;
uniform float uLodBias2;
ARX_VARYING_IN ARX_HIGHP vec2 vTexCoord1;
ARX_VARYING_IN ARX_HIGHP vec2 vTexCoord2;
#endif
#ifdef ARX_ALPHA_CUTOUT
uniform float uAlphaThreshold;
#endif
#ifdef ARX_FOG
uniform vec3 uFogColor;
ARX_VARYING_IN mediump float vFog;
#endif
void main() {
    vec4 color = vColor;
#if ARX_STAGES >= 1
    vec4 texel0 = ARX_TEXTURE(uTexture0, vTexCoord0, uLodBias0);
    color = clamp(color * uStageA0 + texel0 * uStageB0 + color * texel0 * uStageC0, 0.0, 1.0);
#endif
#if ARX_STAGES >= 3
    vec4 texel1 = ARX_TEXTURE(uTexture1, vTexCoord1, uLodBias1);
    color = clamp(color * uStageA1 + texel1 * uStageB1 + color * texel1 * uStageC1, 0.0, 1.0);
    vec4 texel2 = ARX_TEXTURE(uTexture2, vTexCoord2, uLodBias2);
    color = clamp(color * uStageA2 + texel2 * uStageB2 + color * texel2 * uStageC2, 0.0, 1.0);
#endif
#ifdef ARX_ALPHA_CUTOUT
    if(color.a <= uAlphaThreshold) discard;
#endif
#ifdef ARX_FOG
    color.rgb = mix(uFogColor, color.rgb, clamp(vFog, 0.0, 1.0));
#endif
#ifdef ARX_GAMMA
    // Brightness: gamma curve like the desktop window gamma ramp, blacks and whites stay.
    // With uGammaInverted the color is what this draw takes away from the framebuffer
    // (dst * (1 - color): shadows, scorch marks, fades), so the curve goes on what is
    // kept, like the ramp would after the blend.
    vec3 gammaIn = mix(color.rgb, vec3(1.0) - color.rgb, uGammaInverted);
    vec3 gammaOut = pow(gammaIn, vec3(uInvGamma));
    color.rgb = mix(gammaOut, vec3(1.0) - gammaOut, uGammaInverted);
#endif
    ARX_FRAG_COLOR = color;
}
)";

//! Full-screen copy of the resolved MSAA texture into the window surface
const char * const glesCopyVertexShader = R"(
precision highp float;
ARX_VS_IN highp vec2 aPosition;
ARX_VARYING_OUT highp vec2 vTexCoord;
void main() {
    vTexCoord = aPosition * 0.5 + 0.5;
    gl_Position = vec4(aPosition, 0.0, 1.0);
}
)";

const char * const glesCopyFragmentShader = R"(
precision mediump float;
#ifdef GL_FRAGMENT_PRECISION_HIGH
#define ARX_HIGHP highp
#else
#define ARX_HIGHP mediump
#endif
#ifdef ARX_GLSL_300
out vec4 arxFragColor;
#endif
uniform sampler2D uTexture;
ARX_VARYING_IN ARX_HIGHP vec2 vTexCoord;
void main() {
    ARX_FRAG_COLOR = ARX_TEXTURE(uTexture, vTexCoord);
}
)";

//! Combiner coefficients (see glesFragmentShader) for one texture operation
void glesCombinerCoefficients(TextureStage::TextureOp op, float & a, float & b, float & c) {
	a = 0.f;
	b = 0.f;
	c = 0.f;
	switch(op) {
		case TextureStage::OpDisable:    a = 1.f; break;
		case TextureStage::OpSelectArg1: b = 1.f; break;
		case TextureStage::OpModulate:   c = 1.f; break;
		case TextureStage::OpModulate2X: c = 2.f; break;
		case TextureStage::OpModulate4X: c = 4.f; break;
	}
}

} // namespace

extern "C" {

//! Directory for cached OpenGL ES program binaries, empty or null disables the cache
__attribute__((used)) __attribute__((visibility("default")))
void setGLESShaderCacheDir(const char * path) {
	g_glesProgramCacheDir = (path != nullptr) ? path : "";
}

}

bool OpenGLRenderer::initializeGLESProgram() {

	shutdownGLESProgram();

	// Compile and link everything up front, never while rendering. On the OpenGL ES 3.0
	// feature level the linked programs are cached on disk when a cache directory is set.
	GLESProgramCache cacheStorage;
	GLESProgramCache * cache = nullptr;
	if(m_glesIsES3 && !m_glesProgramBinaryFormats.empty() && !g_glesProgramCacheDir.empty()) {
		mkdir(g_glesProgramCacheDir.c_str(), 0700);
		cacheStorage.directory = g_glesProgramCacheDir;
		cacheStorage.formats = &m_glesProgramBinaryFormats;
		u64 driverHash = hashString(14695981039346656037ull, "arx-gles-program-cache");
		const GLenum strings[] = { GL_VENDOR, GL_RENDERER, GL_VERSION, GL_SHADING_LANGUAGE_VERSION };
		for(GLenum name : strings) {
			driverHash = hashString(driverHash, reinterpret_cast<const char *>(glGetString(name)));
		}
		cacheStorage.driverHash = driverHash;
		cache = &cacheStorage;
	}

	const std::string prefix = g_useGLES2_0 ? glesShaderPrefix100 : glesShaderPrefix300;

	const char * samplerNames[3] = { "uTexture0", "uTexture1", "uTexture2" };
	const char * stageANames[3] = { "uStageA0", "uStageA1", "uStageA2" };
	const char * stageBNames[3] = { "uStageB0", "uStageB1", "uStageB2" };
	const char * stageCNames[3] = { "uStageC0", "uStageC1", "uStageC2" };
	const char * lodBiasNames[3] = { "uLodBias0", "uLodBias1", "uLodBias2" };

	for(size_t variant = 0; variant < GLESProgramVariantCount; variant++) {

		std::string defines = prefix;
		defines += "#define ARX_STAGES ";
		defines += std::to_string(glesStageCounts[variant >> GLESProgramStageBucketShift]);
		defines += "\n";
		if(variant & GLESProgramAlphaCutout) {
			defines += "#define ARX_ALPHA_CUTOUT 1\n";
		}
		if(variant & GLESProgramFog) {
			defines += "#define ARX_FOG 1\n";
		}
		if(variant & GLESProgramGamma) {
			defines += "#define ARX_GAMMA 1\n";
		}

		GLuint id = buildGLESProgram(defines + glesVertexShader, defines + glesFragmentShader, cache);
		if(!id) {
			shutdownGLESProgram();
			return false;
		}

		GLESProgram & program = m_glesPrograms[variant];
		program = GLESProgram();
		program.id = id;
		program.uniformMVP = glGetUniformLocation(id, "uMVP");
		program.uniformFogPlane = glGetUniformLocation(id, "uFogPlane");
		program.uniformFogParams = glGetUniformLocation(id, "uFogParams");
		program.uniformFogColor = glGetUniformLocation(id, "uFogColor");
		program.uniformAlphaThreshold = glGetUniformLocation(id, "uAlphaThreshold");
		program.uniformInvGamma = glGetUniformLocation(id, "uInvGamma");
		program.uniformGammaInverted = glGetUniformLocation(id, "uGammaInverted");
		glUseProgram(id);
		m_glesBoundProgram = id;
		for(GLint i = 0; i < 3; ++i) {
			GLint sampler = glGetUniformLocation(id, samplerNames[i]);
			if(sampler >= 0) {
				glUniform1i(sampler, i);
			}
			program.uniformStageA[i] = glGetUniformLocation(id, stageANames[i]);
			program.uniformStageB[i] = glGetUniformLocation(id, stageBNames[i]);
			program.uniformStageC[i] = glGetUniformLocation(id, stageCNames[i]);
			program.uniformLodBias[i] = glGetUniformLocation(id, lodBiasNames[i]);
		}

	}

	// Copy of the resolved MSAA image into the window surface
	m_glesCopyProgram = buildGLESProgram(prefix + glesCopyVertexShader, prefix + glesCopyFragmentShader,
	                                     cache);
	if(m_glesCopyProgram) {
		glUseProgram(m_glesCopyProgram);
		m_glesBoundProgram = m_glesCopyProgram;
		GLint sampler = glGetUniformLocation(m_glesCopyProgram, "uTexture");
		if(sampler >= 0) {
			glUniform1i(sampler, GLint(glesCopyTextureUnit));
		}
		// One triangle covering the whole viewport
		const GLfloat triangle[] = { -1.f, -1.f, 3.f, -1.f, -1.f, 3.f };
		glGenBuffers(1, &m_glesCopyVertices);
		bindBuffer(m_glesCopyVertices);
		glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(sizeof(triangle)), triangle, GL_STATIC_DRAW);
		bindBuffer(GL_NONE);
	} else {
		LogWarning << "MSAA is not available: could not build the GLES copy program";
	}

	glUseProgram(0);
	m_glesBoundProgram = 0;

	if(cache) {
		cleanGLESProgramCache(*cache);
		LogInfo << "GLES program cache: " << cache->loaded << " loaded, " << cache->compiled
		        << " compiled (" << cache->directory << ")";
	} else if(m_glesIsES3 && !g_glesProgramCacheDir.empty()) {
		LogInfo << "GLES program cache disabled: the driver has no program binary formats";
	}

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

	if(m_glesCopyProgram) {
		glDeleteProgram(m_glesCopyProgram);
		m_glesCopyProgram = 0;
	}

	if(m_glesCopyVertices) {
		unbindBuffer(m_glesCopyVertices);
		glDeleteBuffers(1, &m_glesCopyVertices);
		m_glesCopyVertices = 0;
	}

}

void OpenGLRenderer::prepareGLESProgram(bool transformed) {

	// Texture stages that contribute to this draw
	GLTextureStage * stages[3] = { nullptr, nullptr, nullptr };
	bool active[3] = { false, false, false };
	size_t usedStages = 0;
	for(size_t i = 0; i < 3 && i < m_TextureStages.size() && i <= maxTextureStage; ++i) {
		stages[i] = GetTextureStage(i);
		active[i] = stages[i]->getTexture() && stages[i]->isEnabled();
		if(active[i]) {
			usedStages = i + 1;
		}
	}

	const size_t bucket = (usedStages == 0) ? 0 : (usedStages == 1) ? 1 : 2;
	size_t variant = bucket << GLESProgramStageBucketShift;
	if(m_glesAlphaCutout) {
		variant |= GLESProgramAlphaCutout;
	}
	if(m_state.getFog()) {
		variant |= GLESProgramFog;
	}
	if(std::abs(m_glesGamma - 1.f) > 0.001f) {
		variant |= GLESProgramGamma;
	}

	GLESProgram & program = m_glesPrograms[variant];
	if(!program.id) {
		return;
	}

	if(m_glesBoundProgram != program.id) {
		glUseProgram(program.id);
		m_glesBoundProgram = program.id;
	}

	const bool cacheValid = program.cacheValid;

	glm::mat4 mvp;
	glm::vec4 fogPlane;
	if(transformed) {
		mvp = m_projection * m_view;
		// Fog distance: the view-space depth. Arx uses a left-handed view where visible
		// points have z > 0 (createProjectionMatrix() in Camera.cpp puts +z into clip w),
		// the same distance the fixed-function fog of the desktop renderer uses. Taking
		// -z made every distance negative, so the fog factor was always clamped to 1.
		fogPlane = glm::vec4(m_view[0][2], m_view[1][2], m_view[2][2], m_view[3][2]);
	} else {
		// Pre-transformed vertices: pixel coordinates, the view depth is in w
		const float width = float(std::max(1, viewport.width()));
		const float height = float(std::max(1, viewport.height()));
		mvp = glm::mat4(1.f);
		mvp = glm::translate(mvp, glm::vec3(-1.f, 1.f, 0.f));
		mvp = glm::scale(mvp, glm::vec3(2.f / width, -2.f / height, 1.f));
		mvp = glm::translate(mvp, glm::vec3(0.5f, 0.5f, 0.f));
		fogPlane = glm::vec4(0.f, 0.f, 0.f, 1.f);
	}

	if(program.uniformMVP >= 0
	   && (!cacheValid || std::memcmp(glm::value_ptr(program.mvp), glm::value_ptr(mvp), sizeof(glm::mat4)) != 0)) {
		glUniformMatrix4fv(program.uniformMVP, 1, GL_FALSE, glm::value_ptr(mvp));
		program.mvp = mvp;
	}

	if(program.uniformFogPlane >= 0 && (!cacheValid || program.fogPlane != fogPlane)) {
		glUniform4f(program.uniformFogPlane, fogPlane.x, fogPlane.y, fogPlane.z, fogPlane.w);
		program.fogPlane = fogPlane;
	}

	if(program.uniformFogParams >= 0) {
		const float fogEnd = m_glesFogEnd;
		const float fogScale = 1.f / (m_glesFogEnd - m_glesFogStart);
		if(!cacheValid || program.fogParams[0] != fogEnd || program.fogParams[1] != fogScale) {
			glUniform2f(program.uniformFogParams, fogEnd, fogScale);
			program.fogParams[0] = fogEnd;
			program.fogParams[1] = fogScale;
		}
	}

	if(program.uniformFogColor >= 0) {
		Color4f fog(m_glesFogColor);
		if(!cacheValid || program.fogColor[0] != fog.r || program.fogColor[1] != fog.g
		   || program.fogColor[2] != fog.b) {
			glUniform3f(program.uniformFogColor, fog.r, fog.g, fog.b);
			program.fogColor[0] = fog.r;
			program.fogColor[1] = fog.g;
			program.fogColor[2] = fog.b;
		}
	}

	if(program.uniformAlphaThreshold >= 0
	   && (!cacheValid || program.alphaThreshold != m_glesAlphaThreshold)) {
		glUniform1f(program.uniformAlphaThreshold, m_glesAlphaThreshold);
		program.alphaThreshold = m_glesAlphaThreshold;
	}

	if(program.uniformInvGamma >= 0) {
		const float invGamma = 1.f / m_glesGamma;
		if(!cacheValid || program.invGamma != invGamma) {
			glUniform1f(program.uniformInvGamma, invGamma);
			program.invGamma = invGamma;
		}
	}

	if(program.uniformGammaInverted >= 0) {
		// ZERO, ONE_MINUS_SRC_COLOR: the fragment color only darkens what is behind it.
		// Applying the curve to it directly turned faint texels into visible steps, the
		// blob shadows showed every quad as a dark square.
		const float inverted = (m_state.getBlendSrc() == BlendZero
		                        && m_state.getBlendDst() == BlendInvSrcColor) ? 1.f : 0.f;
		if(!cacheValid || program.gammaInverted != inverted) {
			glUniform1f(program.uniformGammaInverted, inverted);
			program.gammaInverted = inverted;
		}
	}

	const size_t stageCount = size_t(glesStageCounts[bucket]);
	for(size_t i = 0; i < stageCount; ++i) {

		float a[4] = { 1.f, 1.f, 1.f, 1.f };
		float b[4] = { 0.f, 0.f, 0.f, 0.f };
		float c[4] = { 0.f, 0.f, 0.f, 0.f };
		if(active[i]) {
			glesCombinerCoefficients(stages[i]->getColorOp(), a[0], b[0], c[0]);
			a[1] = a[2] = a[0];
			b[1] = b[2] = b[0];
			c[1] = c[2] = c[0];
			glesCombinerCoefficients(stages[i]->getAlphaOp(), a[3], b[3], c[3]);
		}

		if(program.uniformStageA[i] >= 0 && (!cacheValid || std::memcmp(program.stageA[i], a, sizeof(a)) != 0)) {
			glUniform4fv(program.uniformStageA[i], 1, a);
			std::memcpy(program.stageA[i], a, sizeof(a));
		}
		if(program.uniformStageB[i] >= 0 && (!cacheValid || std::memcmp(program.stageB[i], b, sizeof(b)) != 0)) {
			glUniform4fv(program.uniformStageB[i], 1, b);
			std::memcpy(program.stageB[i], b, sizeof(b));
		}
		if(program.uniformStageC[i] >= 0 && (!cacheValid || std::memcmp(program.stageC[i], c, sizeof(c)) != 0)) {
			glUniform4fv(program.uniformStageC[i], 1, c);
			std::memcpy(program.stageC[i], c, sizeof(c));
		}

		const float lodBias = stages[i] ? stages[i]->getMipMapLODBias() : 0.f;
		if(program.uniformLodBias[i] >= 0 && (!cacheValid || program.lodBias[i] != lodBias)) {
			glUniform1f(program.uniformLodBias[i], lodBias);
			program.lodBias[i] = lodBias;
		}

	}

	program.cacheValid = true;
}

/*
 * Render passes on tile-based GPUs
 *
 * A frame renders into a single framebuffer: the MSAA framebuffer when antialiasing is
 * enabled, the window surface otherwise.
 *  - Every attachment is cleared before the first draw (Clear() calls of the game,
 *    completed by startRenderPass()), so nothing is ever loaded into tile memory.
 *  - Depth is invalidated before the pass ends, so it never leaves tile memory.
 *  - Multisampled color is resolved into a single-sampled texture as part of the tile
 *    write-back (GL_EXT_multisampled_render_to_texture), the extra samples never reach
 *    memory. One full-screen copy then puts the image into the window surface.
 * This also keeps the window surface single-sampled, so Swappy's sync fence before
 * eglSwapBuffers() cannot force ANGLE to store and resolve multisampled data.
 */

void OpenGLRenderer::beginFrame() {

	m_glesPassStarted = false;
	m_glesPassCleared = 0;

	// Binds the framebuffer for this frame. Its previous contents stay valid until the
	// first draw or clear, for snapshots taken between frames.
	updateMultisampleFramebuffer();

}

void OpenGLRenderer::endFrame() {

	if(m_glesMSAAFramebuffer) {
		resolveMultisampleFramebuffer();
	} else {
		invalidateAttachments(true, false, true);
	}

}

void OpenGLRenderer::startRenderPass() {

	BufferFlags missing = 0;
	if(!(m_glesPassCleared & GL_COLOR_BUFFER_BIT)) {
		missing |= ColorBuffer;
	}
	if(!(m_glesPassCleared & GL_DEPTH_BUFFER_BIT)) {
		missing |= DepthBuffer;
	}
	if(missing) {
		Clear(missing, Color::black, 1.f);
	}

	m_glesPassStarted = true;

}

void OpenGLRenderer::invalidateAttachments(bool defaultFramebuffer, bool color, bool depth) {

	if(!m_hasInvalidateFramebuffer && !m_hasDiscardFramebuffer) {
		return;
	}

	GLenum attachments[3];
	GLsizei count = 0;
	if(color) {
		attachments[count++] = defaultFramebuffer ? GLenum(GL_COLOR) : GLenum(GL_COLOR_ATTACHMENT0);
	}
	if(depth) {
		if(defaultFramebuffer) {
			attachments[count++] = GL_DEPTH;
			attachments[count++] = GL_STENCIL;
		} else {
			attachments[count++] = GL_DEPTH_ATTACHMENT;
		}
	}
	if(count == 0) {
		return;
	}

	if(m_hasInvalidateFramebuffer) {
		glInvalidateFramebuffer(GL_FRAMEBUFFER, count, attachments);
	} else {
		glDiscardFramebufferEXT(GL_FRAMEBUFFER, count, attachments);
	}

}

bool OpenGLRenderer::updateMultisampleFramebuffer() {

	const Vec2i size = mainApp->getWindow()->getSize();
	const bool canResolve = m_glesMSAAResolveBlit || (m_glesCopyProgram && m_glesCopyVertices);
	const bool wanted = config.video.antialiasing && m_glesMaxMSAASamples > 1 && canResolve
	                    && !m_glesMSAAFailed && size.x > 0 && size.y > 0;
	if(!wanted) {
		destroyMultisampleFramebuffer();
		return false;
	}

	if(m_glesMSAAFramebuffer && size == m_glesMSAASize) {
		glBindFramebuffer(GL_FRAMEBUFFER, m_glesMSAAFramebuffer);
		return true;
	}

	destroyMultisampleFramebuffer();

	const GLsizei samples = std::min(GLsizei(m_glesMaxMSAASamples), glesMSAASamples);

	if(m_glesMSAAResolveBlit) {

		// Multisampled color and depth, resolved into the window by resolveMultisampleFramebuffer().
		// Drivers round the sample count up to one they support (Mali has no 2x MSAA).
		glGenRenderbuffers(1, &m_glesMSAAColorBuffer);
		glBindRenderbuffer(GL_RENDERBUFFER, m_glesMSAAColorBuffer);
		if(m_glesMSAABlitANGLE) {
			glRenderbufferStorageMultisampleANGLE(GL_RENDERBUFFER, samples, m_glesWindowColorFormat,
			                                      size.x, size.y);
		} else {
			glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, m_glesWindowColorFormat, size.x, size.y);
		}
		GLint actualSamples = 0;
		glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &actualSamples);
		glGenRenderbuffers(1, &m_glesMSAADepth);
		glBindRenderbuffer(GL_RENDERBUFFER, m_glesMSAADepth);
		if(m_glesMSAABlitANGLE) {
			glRenderbufferStorageMultisampleANGLE(GL_RENDERBUFFER, samples, m_glesDepthFormat, size.x, size.y);
		} else {
			glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, m_glesDepthFormat, size.x, size.y);
		}
		glBindRenderbuffer(GL_RENDERBUFFER, GL_NONE);

		glGenFramebuffers(1, &m_glesMSAAFramebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, m_glesMSAAFramebuffer);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_glesMSAAColorBuffer);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_glesMSAADepth);

		const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		if(status != GL_FRAMEBUFFER_COMPLETE) {
			LogError << "MSAA framebuffer is incomplete (status " << status
			         << "), rendering without antialiasing";
			m_glesMSAAFailed = true;
			destroyMultisampleFramebuffer();
			return false;
		}

		m_glesMSAASamples = GLsizei(std::max(GLint(samples), actualSamples));
		m_glesMSAASize = size;
		m_MSAALevel = int(m_glesMSAASamples);

		LogInfo << "Using " << m_glesMSAASamples << "x MSAA (multisampled framebuffer, "
		        << (m_glesMSAABlitANGLE ? "glBlitFramebufferANGLE" : "glBlitFramebuffer")
		        << " resolve) at " << size.x << "x" << size.y;

		return true;
	}

	// Single-sampled color texture: the samples are resolved into it in tile memory
	glGenTextures(1, &m_glesMSAAColor);
	glActiveTexture(GL_TEXTURE0 + glesCopyTextureUnit);
	glBindTexture(GL_TEXTURE_2D, m_glesMSAAColor);
	glTexImage2D(GL_TEXTURE_2D, 0, m_glesIsES3 ? GL_RGBA8 : GL_RGBA, size.x, size.y, 0,
	             GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, GL_NONE);
	glActiveTexture(GL_TEXTURE0);

	// Multisampled depth that only ever lives in tile memory
	glGenRenderbuffers(1, &m_glesMSAADepth);
	glBindRenderbuffer(GL_RENDERBUFFER, m_glesMSAADepth);
	glRenderbufferStorageMultisampleEXT(GL_RENDERBUFFER, samples, m_glesDepthFormat, size.x, size.y);
	// Drivers round the sample count up to one they support (Mali has no 2x MSAA).
	GLint actualSamples = 0;
	glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &actualSamples);
	glBindRenderbuffer(GL_RENDERBUFFER, GL_NONE);

	glGenFramebuffers(1, &m_glesMSAAFramebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, m_glesMSAAFramebuffer);
	glFramebufferTexture2DMultisampleEXT(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
	                                     m_glesMSAAColor, 0, samples);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_glesMSAADepth);

	const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	if(status != GL_FRAMEBUFFER_COMPLETE) {
		LogError << "MSAA framebuffer is incomplete (status " << status
		         << "), rendering without antialiasing";
		m_glesMSAAFailed = true;
		destroyMultisampleFramebuffer();
		return false;
	}

	m_glesMSAASamples = GLsizei(std::max(GLint(samples), actualSamples));
	m_glesMSAASize = size;
	m_MSAALevel = int(m_glesMSAASamples);

	LogInfo << "Using " << m_glesMSAASamples << "x MSAA (GL_EXT_multisampled_render_to_texture) at "
	        << size.x << "x" << size.y;

	return true;
}

void OpenGLRenderer::destroyMultisampleFramebuffer() {

	if(m_glesMSAAFramebuffer) {
		glBindFramebuffer(GL_FRAMEBUFFER, GL_NONE);
		glDeleteFramebuffers(1, &m_glesMSAAFramebuffer);
		m_glesMSAAFramebuffer = 0;
	}
	if(m_glesMSAAColorBuffer) {
		glDeleteRenderbuffers(1, &m_glesMSAAColorBuffer);
		m_glesMSAAColorBuffer = 0;
	}
	if(m_glesMSAADepth) {
		glDeleteRenderbuffers(1, &m_glesMSAADepth);
		m_glesMSAADepth = 0;
	}
	if(m_glesMSAAColor) {
		glDeleteTextures(1, &m_glesMSAAColor);
		m_glesMSAAColor = 0;
	}

	m_glesMSAASamples = 0;
	m_glesMSAASize = Vec2i(0);
	m_MSAALevel = 0;
	m_hasMSAA = false;

}

void OpenGLRenderer::resolveMultisampleFramebuffer() {

	if(m_glesMSAAResolveBlit) {

		if(!m_glesPassStarted && m_glesPassCleared == 0) {
			// Nothing was drawn or cleared this frame, and the samples of the previous
			// frame were invalidated: there is nothing to resolve.
			return;
		}

		// Depth is not needed after the frame: keep it in tile memory.
		invalidateAttachments(false, false, true);

		// Resolve the whole framebuffer into the window while the render pass is still open:
		// ANGLE then adds the window as a resolve attachment of that render pass, so the
		// samples are resolved during the tile write-back and never stored. The scissor
		// test would limit the blit, the formats match (see initialize()).
		const Vec2i size = m_glesMSAASize;
		if(m_scissor.isValid()) {
			glDisable(GL_SCISSOR_TEST);
		}
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, GL_NONE);
		if(m_glesMSAABlitANGLE) {
			glBlitFramebufferANGLE(0, 0, size.x, size.y, 0, 0, size.x, size.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			// The samples are not needed anymore either. glDiscardFramebufferEXT() only
			// takes GL_FRAMEBUFFER, so bind the multisampled framebuffer for it again.
			glBindFramebuffer(GL_FRAMEBUFFER, m_glesMSAAFramebuffer);
			invalidateAttachments(false, true, false);
		} else {
			glBlitFramebuffer(0, 0, size.x, size.y, 0, 0, size.x, size.y, GL_COLOR_BUFFER_BIT, GL_NEAREST);
			// The samples are not needed anymore either.
			const GLenum color = GL_COLOR_ATTACHMENT0;
			glInvalidateFramebuffer(GL_READ_FRAMEBUFFER, 1, &color);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, GL_NONE);
		if(m_scissor.isValid()) {
			glEnable(GL_SCISSOR_TEST);
		}

		// Only the color is presented
		invalidateAttachments(true, false, true);

		return;
	}

	// Depth is not needed after the frame: keep it in tile memory. The color samples are
	// resolved into m_glesMSAAColor when the render pass ends with the binding change.
	invalidateAttachments(false, false, true);
	glBindFramebuffer(GL_FRAMEBUFFER, GL_NONE);

	// The copy covers the whole window: nothing has to be loaded from memory.
	invalidateAttachments(true, true, true);

	const Vec2i size = m_glesMSAASize;
	glViewport(0, 0, size.x, size.y);
	if(m_scissor.isValid()) {
		glDisable(GL_SCISSOR_TEST);
	}
	if(m_glstate.getCull()) {
		glDisable(GL_CULL_FACE);
	}
	if(m_glesBlendEnabled) {
		glDisable(GL_BLEND);
	}
	if(m_glalphaToCoverage) {
		glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
	}
	glDisable(GL_DEPTH_TEST);

	glUseProgram(m_glesCopyProgram);
	m_glesBoundProgram = m_glesCopyProgram;
	glActiveTexture(GL_TEXTURE0 + glesCopyTextureUnit);
	glBindTexture(GL_TEXTURE_2D, m_glesMSAAColor);

	resetVertexArrayState();
	bindBuffer(m_glesCopyVertices);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);

	glDrawArrays(GL_TRIANGLES, 0, 3);

	// Only the color is presented
	invalidateAttachments(true, false, true);

	// Restore the state the renderer expects
	glBindTexture(GL_TEXTURE_2D, GL_NONE);
	glActiveTexture(GL_TEXTURE0);
	resetVertexArrayState();
	glEnable(GL_DEPTH_TEST);
	if(m_glalphaToCoverage) {
		glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
	}
	if(m_glesBlendEnabled) {
		glEnable(GL_BLEND);
	}
	if(m_glstate.getCull()) {
		glEnable(GL_CULL_FACE);
	}
	if(m_scissor.isValid()) {
		glEnable(GL_SCISSOR_TEST);
	}
	if(viewport.width() > 0 && viewport.height() > 0) {
		glViewport(viewport.left, size.y - viewport.bottom, viewport.width(), viewport.height());
	}

}

#endif

