#include "GLStateManager.h"

namespace GLStateManager {

    // --- TRACKING STORAGE CACHES ---
    // --- Initialize with invalid values so the first engine calls ALWAYS flush to the driver! ---
    GLuint activeTextureUnit  = 0; // Not GL_TEXTURE0
    GLuint activeProgramID    = 0xFFFFFFFF;
    GLuint activeVAOID        = 0xFFFFFFFF;
    GLuint activeDrawFBO      = 0xFFFFFFFF;
    GLuint activeReadFBO      = 0xFFFFFFFF;

    // Initialize all 32 texture slot caches to an invalid state identifier
    GLuint boundTextures[32] = {
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF
    };

    // Use an explicit un-initialized state flag (-1) for blending properties
    GLboolean blendEnabled        = -1; // Neither GL_TRUE nor GL_FALSE
    GLenum    blendEquationRGB    = -1;
    GLenum    blendEquationAlpha  = -1;
    GLenum    blendSrcFunc        = -1;
    GLenum    blendDstFunc        = -1;

    GLboolean depthTestEnabled    = -1; 
    GLenum    depthFunction       = -1;
    GLboolean depthWriteMask      = -1;

    GLboolean cullFaceEnabled     = -1;
    GLenum    activeFrontFace     = -1;
    GLenum    activeCullFaceMode  = -1;

    GLboolean stencilEnabled      = -1;
    GLboolean scissorEnabled      = -1;

    GLboolean colorMaskR = 2, colorMaskG = 2, colorMaskB = 2, colorMaskA = 2;

    GLuint activeArrayBuffer          = 0xFFFFFFFF;
    GLuint activeElementArrayBuffer   = 0xFFFFFFFF;
    GLuint activeUniformBuffer        = 0xFFFFFFFF;
    GLuint activeShaderStorageBuffer  = 0xFFFFFFFF;

    GLuint boundSamplers[32] = {
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
        0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF
    };
        
    struct GLViewportRect { GLint x, y; GLsizei width, height; };
    // Force viewport metrics out of bounds so the main window setup pass runs cleanly
    GLViewportRect activeViewport = { -1, -1, -1, -1 };

    // --- HOOKED CACHING WRAPPERS ---
    void APIENTRY hook_glActiveTexture(GLenum textureUnit) {
        if (textureUnit != activeTextureUnit) {
            (glad_glActiveTexture)(textureUnit); // Parentheses completely bypass our macros!
            activeTextureUnit = textureUnit;
        }
    }

    void APIENTRY hook_glBindTexture(GLenum target, GLuint textureID) {
        if (activeTextureUnit >= GL_TEXTURE0) {
            GLuint unitIdx = activeTextureUnit - GL_TEXTURE0;
            if (unitIdx < 32) {
                if (boundTextures[unitIdx] != textureID) {
                    (glad_glBindTexture)(target, textureID);
                    boundTextures[unitIdx] = textureID;
                }
                return;
            }
        }
        (glad_glBindTexture)(target, textureID); // Fallback boundary safety
    }

    void APIENTRY hook_glDeleteTextures(GLsizei n, const GLuint* textures) {
        for (GLsizei i = 0; i < n; ++i) {
            for (int t = 0; t < 32; ++t) {
                if (boundTextures[t] == textures[i]) boundTextures[t] = 0;
            }
        }
        (glad_glDeleteTextures)(n, textures);
    }

    void APIENTRY hook_glUseProgram(GLuint programID) {
        if (programID != activeProgramID) {
            (glad_glUseProgram)(programID);
            activeProgramID = programID;
        }
    }

    void APIENTRY hook_glBindVertexArray(GLuint vaoID) {
        if (vaoID != activeVAOID) {
            (glad_glBindVertexArray)(vaoID);
            activeVAOID = vaoID;
        }
    }

    void APIENTRY hook_glDeleteVertexArrays(GLsizei n, const GLuint* vaos) {
        for (GLsizei i = 0; i < n; ++i) {
            if (activeVAOID == vaos[i]) activeVAOID = 0;
        }
        (glad_glDeleteVertexArrays)(n, vaos);
    }

    void APIENTRY hook_glBindFramebuffer(GLenum target, GLuint fboID) {
        // CASE A: The engine explicitly binds BOTH pipelines simultaneously
        if (target == GL_FRAMEBUFFER) {
            if (fboID != activeDrawFBO || fboID != activeReadFBO) {
                (glad_glBindFramebuffer)(GL_FRAMEBUFFER, fboID);
                activeDrawFBO = fboID;
                activeReadFBO = fboID;
            }
        }
        // CASE B: The engine is binding ONLY the Draw/Paint target buffer.
        // --- FIXED: Always forward this call to the hardware driver! ---
        // This ensures the driver's rasterizer unit seamlessly catches cubemap face switches,
        // attachment swaps, and draw buffer shifts without altering frame rates.
        else if (target == GL_DRAW_FRAMEBUFFER) {
            (glad_glBindFramebuffer)(GL_DRAW_FRAMEBUFFER, fboID);
            activeDrawFBO = fboID;
        }
        // CASE C: The engine is binding ONLY the Read/Blit target buffer
        else if (target == GL_READ_FRAMEBUFFER) {
            if (fboID != activeReadFBO) {
                (glad_glBindFramebuffer)(GL_READ_FRAMEBUFFER, fboID);
                activeReadFBO = fboID;
            }
        }
    }

    void APIENTRY hook_glDeleteFramebuffers(GLsizei n, const GLuint* fbos) {
        for (GLsizei i = 0; i < n; ++i) {
            if (activeDrawFBO == fbos[i]) activeDrawFBO = 0;
            if (activeReadFBO == fbos[i]) activeReadFBO = 0;
        }
        (glad_glDeleteFramebuffers)(n, fbos);
    }

    void APIENTRY hook_glEnable(GLenum cap) {
        if (cap == GL_BLEND) {
            if (blendEnabled != GL_TRUE) { (glad_glEnable)(GL_BLEND); blendEnabled = GL_TRUE; }
        } else if (cap == GL_DEPTH_TEST) {
            if (depthTestEnabled != GL_TRUE) { (glad_glEnable)(GL_DEPTH_TEST); depthTestEnabled = GL_TRUE; }
        } else if (cap == GL_CULL_FACE) {
            if (cullFaceEnabled != GL_TRUE) { (glad_glEnable)(GL_CULL_FACE); cullFaceEnabled = GL_TRUE; }
        } else if (cap == GL_STENCIL_TEST) {
            if (stencilEnabled != GL_TRUE) { (glad_glEnable)(GL_STENCIL_TEST); stencilEnabled = GL_TRUE; }
        } else if (cap == GL_SCISSOR_TEST) {
            if (scissorEnabled != GL_TRUE) { (glad_glEnable)(GL_SCISSOR_TEST); scissorEnabled = GL_TRUE; }
        } else {
            (glad_glEnable)(cap);
        }
    }

    void APIENTRY hook_glDisable(GLenum cap) {
        if (cap == GL_BLEND) {
            if (blendEnabled != GL_FALSE) { (glad_glDisable)(GL_BLEND); blendEnabled = GL_FALSE; }
        } else if (cap == GL_DEPTH_TEST) {
            if (depthTestEnabled != GL_FALSE) { (glad_glDisable)(GL_DEPTH_TEST); depthTestEnabled = GL_FALSE; }
        } else if (cap == GL_CULL_FACE) {
            if (cullFaceEnabled != GL_FALSE) { (glad_glDisable)(GL_CULL_FACE); cullFaceEnabled = GL_FALSE; }
        } else if (cap == GL_STENCIL_TEST) {
            if (stencilEnabled != GL_FALSE) { (glad_glDisable)(GL_STENCIL_TEST); stencilEnabled = GL_FALSE; }
        } else if (cap == GL_SCISSOR_TEST) {
            if (scissorEnabled != GL_FALSE) { (glad_glDisable)(GL_SCISSOR_TEST); scissorEnabled = GL_FALSE; }
        } else {
            (glad_glDisable)(cap);
        }
    }

    void APIENTRY hook_glBlendEquation(GLenum mode) {
        if (blendEquationRGB != mode || blendEquationAlpha != mode) {
            (glad_glBlendEquation)(mode);
            blendEquationRGB = mode; blendEquationAlpha = mode;
        }
    }

    void APIENTRY hook_glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha) {
        if (blendEquationRGB != modeRGB || blendEquationAlpha != modeAlpha) {
            (glad_glBlendEquationSeparate)(modeRGB, modeAlpha);
            blendEquationRGB = modeRGB; blendEquationAlpha = modeAlpha;
        }
    }

    void APIENTRY hook_glBlendFunc(GLenum sfactor, GLenum dfactor) {
        if (blendSrcFunc != sfactor || blendDstFunc != dfactor) {
            (glad_glBlendFunc)(sfactor, dfactor);
            blendSrcFunc = sfactor; blendDstFunc = dfactor;
        }
    }

    void APIENTRY hook_glDepthFunc(GLenum func) {
        if (depthFunction != func) {
            (glad_glDepthFunc)(func);
            depthFunction = func;
        }
    }

    void APIENTRY hook_glDepthMask(GLboolean flag) {
        if (depthWriteMask != flag) {
            (glad_glDepthMask)(flag);
            depthWriteMask = flag;
        }
    }

    void APIENTRY hook_glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) {
        if (colorMaskR != red || colorMaskG != green || colorMaskB != blue || colorMaskA != alpha) {
            (glad_glColorMask)(red, green, blue, alpha);
            colorMaskR = red; colorMaskG = green; colorMaskB = blue; colorMaskA = alpha;
        }
    }

    void APIENTRY hook_glViewport(GLint x, GLint y, GLsizei width, GLsizei height) {
        if (activeViewport.x != x || activeViewport.y != y || activeViewport.width != width || activeViewport.height != height) {
            (glad_glViewport)(x, y, width, height);
            activeViewport.x = x; activeViewport.y = y;
            activeViewport.width = width; activeViewport.height = height;
        }
    }

    void APIENTRY hook_glBindBuffer(GLenum target, GLuint bufferID) {
        if (target == GL_ARRAY_BUFFER) {
            if (bufferID != activeArrayBuffer) {
                (glad_glBindBuffer)(GL_ARRAY_BUFFER, bufferID);
                activeArrayBuffer = bufferID;
            }
        }
        else if (target == GL_ELEMENT_ARRAY_BUFFER) {
            if (bufferID != activeElementArrayBuffer) {
                (glad_glBindBuffer)(GL_ELEMENT_ARRAY_BUFFER, bufferID);
                activeElementArrayBuffer = bufferID;
            }
        }
        else if (target == GL_UNIFORM_BUFFER) {
            if (bufferID != activeUniformBuffer) {
                (glad_glBindBuffer)(GL_UNIFORM_BUFFER, bufferID);
                activeUniformBuffer = bufferID;
            }
        }
        else if (target == GL_SHADER_STORAGE_BUFFER) {
            if (bufferID != activeShaderStorageBuffer) {
                (glad_glBindBuffer)(GL_SHADER_STORAGE_BUFFER, bufferID);
                activeShaderStorageBuffer = bufferID;
            }
        }
        else {
            // Safety fallback for rare buffer targets (e.g. transform feedback, pixel unpack)
            (glad_glBindBuffer)(target, bufferID);
        }
    }

    void APIENTRY hook_glBindSampler(GLuint unit, GLuint samplerID) {
        if (unit < 32) {
            if (boundSamplers[unit] != samplerID) {
                (glad_glBindSampler)(unit, samplerID);
                boundSamplers[unit] = samplerID;
            }
        } else {
            (glad_glBindSampler)(unit, samplerID); // Out of bounds safety fallback
        }
    }

    void APIENTRY hook_glDeleteBuffers(GLsizei n, const GLuint* buffers) {
        for (GLsizei i = 0; i < n; ++i) {
            if (activeArrayBuffer          == buffers[i]) activeArrayBuffer          = 0;
            if (activeElementArrayBuffer   == buffers[i]) activeElementArrayBuffer   = 0;
            if (activeUniformBuffer        == buffers[i]) activeUniformBuffer        = 0;
            if (activeShaderStorageBuffer  == buffers[i]) activeShaderStorageBuffer  = 0;
        }
        (glad_glDeleteBuffers)(n, buffers);
    }

    void APIENTRY hook_glDeleteSamplers(GLsizei n, const GLuint* samplers) {
        for (GLsizei i = 0; i < n; ++i) {
            for (int s = 0; s < 32; ++s) {
                if (boundSamplers[s] == samplers[i]) boundSamplers[s] = 0;
            }
        }
        (glad_glDeleteSamplers)(n, samplers);
    }

    void APIENTRY hook_glFrontFace(GLenum mode) {
        // Only hit the hardware if the requested mode shifts (e.g., GL_CW vs GL_CCW)
        if (mode != activeFrontFace) {
            (glad_glFrontFace)(mode);
            activeFrontFace = mode;
        }
    }

    void APIENTRY hook_glCullFace(GLenum mode) {
        // Only hit the hardware if the face target changes (e.g., GL_BACK vs GL_FRONT)
        if (mode != activeCullFaceMode) {
            (glad_glCullFace)(mode);
            activeCullFaceMode = mode;
        }
    }

    GLboolean APIENTRY hook_glIsEnabled(GLenum cap) {
        if (cap == GL_BLEND) {
            // If our cache is in an unknown state (2), force a safe synchronous driver query
            if (blendEnabled == 2) blendEnabled = (glad_glIsEnabled)(GL_BLEND);
            return blendEnabled;
        }
        if (cap == GL_DEPTH_TEST) {
            if (depthTestEnabled == 2) depthTestEnabled = (glad_glIsEnabled)(GL_DEPTH_TEST);
            return depthTestEnabled;
        }
        if (cap == GL_CULL_FACE) {
            if (cullFaceEnabled == 2) cullFaceEnabled = (glad_glIsEnabled)(GL_CULL_FACE);
            return cullFaceEnabled;
        }
        if (cap == GL_STENCIL_TEST) {
            if (stencilEnabled == 2) stencilEnabled = (glad_glIsEnabled)(GL_STENCIL_TEST);
            return stencilEnabled;
        }
        if (cap == GL_SCISSOR_TEST) {
            if (scissorEnabled == 2) scissorEnabled = (glad_glIsEnabled)(GL_SCISSOR_TEST);
            return scissorEnabled;
        }

        // Hardware driver fallback for un-tracked structural capabilities
        return (glad_glIsEnabled)(cap);
    }

    void APIENTRY GetCachedIntegerv(GLenum pName, GLint* params) {
        if (!params) return;

        switch (pName) {
            //case GL_FRAMEBUFFER_BINDING: // same as the below
            case GL_DRAW_FRAMEBUFFER_BINDING:
                params[0] = activeDrawFBO;
                return;

            case GL_READ_FRAMEBUFFER_BINDING:
                params[0] = activeReadFBO;
                return;

            case GL_CURRENT_PROGRAM:
                params[0] = activeProgramID;
                return;

            case GL_VERTEX_ARRAY_BINDING:
                params[0] = activeVAOID;
                return;

            case GL_ACTIVE_TEXTURE:
                params[0] = activeTextureUnit;
                return;

            // --- NEW METRIC CACHES: BUFFERS ---
            case GL_ARRAY_BUFFER_BINDING:
                params[0] = activeArrayBuffer;
                return;

            case GL_ELEMENT_ARRAY_BUFFER_BINDING:
                params[0] = activeElementArrayBuffer;
                return;

            case GL_UNIFORM_BUFFER_BINDING:
                params[0] = activeUniformBuffer;
                return;

            case GL_SHADER_STORAGE_BUFFER_BINDING:
                params[0] = activeShaderStorageBuffer;
                return;

            case GL_VIEWPORT:
                // Safely copy all 4 viewport parameters out of our localized memory struct
                params[0] = activeViewport.x;
                params[1] = activeViewport.y;
                params[2] = activeViewport.width;
                params[3] = activeViewport.height;
                return;

            case GL_BLEND:
                params[0] = (blendEnabled == GL_TRUE) ? 1 : 0;
                return;

            case GL_DEPTH_TEST:
                params[0] = (depthTestEnabled == GL_TRUE) ? 1 : 0;
                return;

            case GL_CULL_FACE:
                params[0] = (cullFaceEnabled == GL_TRUE) ? 1 : 0;
                return;

            case GL_FRONT_FACE:
                params[0] = activeFrontFace;
                return;

            case GL_CULL_FACE_MODE:
                params[0] = activeCullFaceMode;
                return;

            case GL_BLEND_SRC:
                params[0] = blendSrcFunc;
                return;

            case GL_BLEND_DST:
                params[0] = blendDstFunc;
                return;

            case GL_DEPTH_FUNC:
                params[0] = depthFunction;
                return;

            case GL_DEPTH_WRITEMASK:
                params[0] = depthWriteMask;
                return;

            case GL_BLEND_EQUATION:
                params[0] = blendEquationRGB;
                return;

            case GL_BLEND_EQUATION_ALPHA:
                params[0] = blendEquationAlpha;
                return;

            case GL_STENCIL_TEST:
                params[0] = (stencilEnabled == GL_TRUE) ? 1 : 0;
                return;

            case GL_SCISSOR_TEST:
                params[0] = (scissorEnabled == GL_TRUE) ? 1 : 0;
                return;

            case GL_COLOR_WRITEMASK:
                params[0] = colorMaskR;
                params[1] = colorMaskG;
                params[2] = colorMaskB;
                params[3] = colorMaskA;
                return;

            default:
                // Hardware fallback for un-cached matrix or specialized parameters
                (glad_glGetIntegerv)(pName, params);
                return;
        }
    }

    void ForceFlushAllStates() {
        activeTextureUnit = GL_TEXTURE0; activeProgramID = 0; activeVAOID = 0; activeDrawFBO = 0; activeReadFBO = 0;
        for (int i = 0; i < 32; ++i) boundTextures[i] = 0;
        blendEnabled = -1; depthTestEnabled = -1; depthWriteMask = -1;
        colorMaskR = -1; colorMaskG = -1; colorMaskB = -1; colorMaskA = -1;
        activeArrayBuffer = 0xFFFFFFFF; activeElementArrayBuffer = 0xFFFFFFFF;
        activeUniformBuffer = 0xFFFFFFFF; activeShaderStorageBuffer = 0xFFFFFFFF;
        for (int i = 0; i < 32; ++i) boundSamplers[i] = 0xFFFFFFFF;
        activeViewport = { -1, -1, -1, -1 };
        cullFaceEnabled    = -1;
        activeFrontFace    = -1;
        activeCullFaceMode = -1;
        stencilEnabled = -1; scissorEnabled = -1;
    }
}