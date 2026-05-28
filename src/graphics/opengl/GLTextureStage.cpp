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

#include "graphics/opengl/GLTextureStage.h"

#include "graphics/opengl/GLTexture.h"
#include "graphics/opengl/OpenGLRenderer.h"
#include "io/log/Logger.h"

GLTextureStage::GLTextureStage(OpenGLRenderer * _renderer, unsigned stage)
        : TextureStage(stage)
        , renderer(_renderer)
        , tex(nullptr)
        , current(nullptr)
        , m_dirty(false)
        , m_enabled(false)
        , m_mipMapLODBias(0.f)
        , m_appliedMipMapLODBias(0.f)
{
    if(mStage == 0) {
        ops[ColorOp] = OpModulate;
        ops[AlphaOp] = OpSelectArg1;
        m_enabled = true;

        glActiveTexture(GL_TEXTURE0);
        setTexEnv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        setTexEnv(GL_TEXTURE_ENV, GL_COMBINE_ALPHA, GL_REPLACE);
        glEnable(GL_TEXTURE_2D);
    } else {
        ops[ColorOp] = OpDisable;
        ops[AlphaOp] = OpDisable;
    }

    current = nullptr;
    tex = nullptr;
    m_dirty = false;
}

GLTextureStage::~GLTextureStage() {
	resetTexture();
}

Texture * GLTextureStage::getTexture() const {
	return tex;
}

void GLTextureStage::setTexture(Texture * texture) {
    arx_assert(texture != nullptr);

    GLTexture * newTex = reinterpret_cast<GLTexture *>(texture);
    if(tex == newTex) {
        return;
    }

    tex = newTex;
    m_dirty = true;
    renderer->m_textureStagesDirty = true;
}

void GLTextureStage::resetTexture() {
    if(tex == nullptr) {
        return;
    }

    tex = nullptr;
    m_dirty = true;
    renderer->m_textureStagesDirty = true;
}

struct GLTexEnvParam {
	
	GLenum combine;
	
	GLenum source;
	
	GLenum scale;
	
};

static const GLTexEnvParam glTexEnv[] = {
	{
		GL_COMBINE_RGB,
		GL_SOURCE0_RGB,
		GL_RGB_SCALE
	}, {
		GL_COMBINE_ALPHA,
		GL_SOURCE0_ALPHA,
		GL_ALPHA_SCALE
	}
};

void GLTextureStage::setArg(OpType alpha, GLint arg) {
	
	setTexEnv(GL_TEXTURE_ENV, glTexEnv[alpha].source, arg);
}

void GLTextureStage::setOp(OpType alpha, GLint op, GLint scale) {

	setTexEnv(GL_TEXTURE_ENV, glTexEnv[alpha].combine, op);
	setTexEnv(GL_TEXTURE_ENV, glTexEnv[alpha].scale, scale);
}

void GLTextureStage::setOp(OpType alpha, TextureOp op) {

    if(ops[alpha] == op) {
        return;
    }

    bool wasEnabled = isEnabled();
    ops[alpha] = op;
    bool enabled = isEnabled();

    if(wasEnabled != enabled) {
        if(enabled) {
            renderer->maxTextureStage = std::max<size_t>(mStage, renderer->maxTextureStage);
        } else if(renderer->maxTextureStage == mStage) {
            renderer->maxTextureStage = 0;
            for(int stage = int(mStage) - 1; stage >= 0; --stage) {
                if(renderer->GetTextureStage(stage)->isEnabled()) {
                    renderer->maxTextureStage = size_t(stage);
                    break;
                }
            }
        }
    }

    m_dirty = true;
    renderer->m_textureStagesDirty = true;
}

void GLTextureStage::setTexEnv(GLenum target, GLenum pname, GLint param) {

	IntegerStateCache::iterator it = m_stateCacheIntegers.find(pname);
	if(it == m_stateCacheIntegers.end() || it->second != param) {
		glTexEnvi(target, pname, param);
		m_stateCacheIntegers[pname] = param;
	}
}

void GLTextureStage::setColorOp(TextureOp op) {
	setOp(ColorOp, op);
}

void GLTextureStage::setAlphaOp(TextureOp op) {
	setOp(AlphaOp, op);
}

void GLTextureStage::setMipMapLODBias(float bias) {
    if(m_mipMapLODBias == bias) {
        return;
    }

    m_mipMapLODBias = bias;
    m_dirty = true;
    renderer->m_textureStagesDirty = true;
}

void GLTextureStage::apply() {

    if(!m_dirty) {
        return;
    }

    if(mStage != 0) {
        glActiveTexture(GL_TEXTURE0 + mStage);
    }

    bool enabled = isEnabled();
    if(m_enabled != enabled) {
        if(enabled) {
            glEnable(GL_TEXTURE_2D);
            setTexEnv(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_COMBINE);
        } else {
            glDisable(GL_TEXTURE_2D);
        }
        m_enabled = enabled;
    }

    switch(ops[ColorOp]) {
        case OpDisable:      setOp(ColorOp, GL_REPLACE, 1); setArg(ColorOp, GL_PREVIOUS); break;
        case OpSelectArg1:   setOp(ColorOp, GL_REPLACE, 1); setArg(ColorOp, GL_TEXTURE); break;
        case OpModulate:     setOp(ColorOp, GL_MODULATE, 1); setArg(ColorOp, GL_TEXTURE); break;
        case OpModulate2X:   setOp(ColorOp, GL_MODULATE, 2); setArg(ColorOp, GL_TEXTURE); break;
        case OpModulate4X:   setOp(ColorOp, GL_MODULATE, 4); setArg(ColorOp, GL_TEXTURE); break;
    }

    switch(ops[AlphaOp]) {
        case OpDisable:      setOp(AlphaOp, GL_REPLACE, 1); setArg(AlphaOp, GL_PREVIOUS); break;
        case OpSelectArg1:   setOp(AlphaOp, GL_REPLACE, 1); setArg(AlphaOp, GL_TEXTURE); break;
        case OpModulate:     setOp(AlphaOp, GL_MODULATE, 1); setArg(AlphaOp, GL_TEXTURE); break;
        case OpModulate2X:   setOp(AlphaOp, GL_MODULATE, 2); setArg(AlphaOp, GL_TEXTURE); break;
        case OpModulate4X:   setOp(AlphaOp, GL_MODULATE, 4); setArg(AlphaOp, GL_TEXTURE); break;
    }

    if(m_appliedMipMapLODBias != m_mipMapLODBias) {
        glTexEnvf(GL_TEXTURE_FILTER_CONTROL, GL_TEXTURE_LOD_BIAS, m_mipMapLODBias);
        m_appliedMipMapLODBias = m_mipMapLODBias;
    }

    if(tex != current) {
        glBindTexture(GL_TEXTURE_2D, tex ? tex->tex : GL_NONE);
        current = tex;
    }

    if(tex) {
        bool apply = true;
        for(size_t i = 0; i < mStage; i++) {
            GLTextureStage * stage = renderer->GetTextureStage(i);
            if(stage->tex == tex && stage->isEnabled()) {
                apply = false;
#ifdef ARX_DEBUG
                if(stage->getWrapMode() != getWrapMode()
				   || stage->getMinFilter() != getMinFilter()
				   || stage->getMagFilter() != getMagFilter()) {
					static bool warned = false;
					if(!warned) {
						LogWarning << "Same texture used in multiple stages with different attributes.";
						warned = true;
					}
				}
#else
                break;
#endif
            }
        }

        if(apply) {
            tex->apply(this);
        }
    }

    if(mStage != 0) {
        glActiveTexture(GL_TEXTURE0);
    }

    m_dirty = false;
}
