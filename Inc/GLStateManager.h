#pragma once
#include <glad.h>

namespace GLStateManager {

    void ForceFlushAllStates();

    // --- MOVE HOOK SIGNATURES TO THE PUBLIC NAMESPACE ---
    // This allows our global macros to find and execute them from any source file!
    GLboolean APIENTRY hook_glIsEnabled(GLenum cap);
    void APIENTRY GetCachedIntegerv(GLenum pName, GLint* params);
    void APIENTRY hook_glActiveTexture(GLenum textureUnit);
    void APIENTRY hook_glBindTexture(GLenum target, GLuint textureID);
    void APIENTRY hook_glDeleteTextures(GLsizei n, const GLuint* textures);
    void APIENTRY hook_glUseProgram(GLuint programID);
    void APIENTRY hook_glBindVertexArray(GLuint vaoID);
    void APIENTRY hook_glDeleteVertexArrays(GLsizei n, const GLuint* vaos);
    void APIENTRY hook_glBindFramebuffer(GLenum target, GLuint fboID);
    void APIENTRY hook_glDeleteFramebuffers(GLsizei n, const GLuint* fbos);
    void APIENTRY hook_glEnable(GLenum cap);
    void APIENTRY hook_glDisable(GLenum cap);
    void APIENTRY hook_glBlendEquation(GLenum mode);
    void APIENTRY hook_glBlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha);
    void APIENTRY hook_glBlendFunc(GLenum sfactor, GLenum dfactor);
    void APIENTRY hook_glDepthFunc(GLenum func);
    void APIENTRY hook_glDepthMask(GLboolean flag);
    void APIENTRY hook_glColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
    void APIENTRY hook_glViewport(GLint x, GLint y, GLsizei width, GLsizei height);
    void APIENTRY hook_glBindBuffer(GLenum target, GLuint bufferID);
    void APIENTRY hook_glBindSampler(GLuint unit, GLuint samplerID);
    void APIENTRY hook_glDeleteBuffers(GLsizei n, const GLuint* buffers);
    void APIENTRY hook_glDeleteSamplers(GLsizei n, const GLuint* samplers);
    void APIENTRY hook_glFrontFace(GLenum mode);
    void APIENTRY hook_glCullFace(GLenum mode);
}

// =========================================================================
// --- THE DIRECT COMPILATION RE-ROUTE ---
// =========================================================================
#ifndef BUILDING_STATE_MANAGER

// 1. Strip any overlapping or hidden glad/driver macro translations first
#undef glIsEnabled
#undef glGetIntegerv
#undef glActiveTexture
#undef glBindTexture
#undef glDeleteTextures
#undef glUseProgram
#undef glBindVertexArray
#undef glDeleteVertexArrays
#undef glBindFramebuffer
#undef glDeleteFramebuffers
#undef glEnable
#undef glDisable
#undef glBlendEquation
#undef glBlendEquationSeparate
#undef glBlendFunc
#undef glDepthFunc
#undef glDepthMask
#undef glColorMask
#undef glViewport
#undef glBindBuffer
#undef glBindSampler
#undef glDeleteBuffers
#undef glDeleteSamplers
#undef glFrontFace
#undef glCullFace

// 2. Map the base functions directly to your state manager hooks!
#define glIsEnabled             GLStateManager::hook_glIsEnabled
#define glGetIntegerv           GLStateManager::GetCachedIntegerv
#define glActiveTexture         GLStateManager::hook_glActiveTexture
#define glBindTexture           GLStateManager::hook_glBindTexture
#define glDeleteTextures        GLStateManager::hook_glDeleteTextures
#define glUseProgram            GLStateManager::hook_glUseProgram
#define glBindVertexArray       GLStateManager::hook_glBindVertexArray
#define glDeleteVertexArrays    GLStateManager::hook_glDeleteVertexArrays
#define glBindFramebuffer       GLStateManager::hook_glBindFramebuffer
#define glDeleteFramebuffers    GLStateManager::hook_glDeleteFramebuffers
#define glEnable                GLStateManager::hook_glEnable
#define glDisable               GLStateManager::hook_glDisable
#define glBlendEquation         GLStateManager::hook_glBlendEquation
#define glBlendEquationSeparate GLStateManager::hook_glBlendEquationSeparate
#define glBlendFunc             GLStateManager::hook_glBlendFunc
#define glDepthFunc             GLStateManager::hook_glDepthFunc
#define glDepthMask             GLStateManager::hook_glDepthMask
#define glColorMask             GLStateManager::hook_glColorMask
#define glViewport              GLStateManager::hook_glViewport
#define glBindBuffer            GLStateManager::hook_glBindBuffer
#define glBindSampler           GLStateManager::hook_glBindSampler
#define glDeleteBuffers         GLStateManager::hook_glDeleteBuffers
#define glDeleteSamplers        GLStateManager::hook_glDeleteSamplers
#define glFrontFace             GLStateManager::hook_glFrontFace
#define glCullFace              GLStateManager::hook_glCullFace

#endif // BUILDING_STATE_MANAGER
