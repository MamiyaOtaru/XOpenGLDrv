#include "Fbo.h"
#include <cstdio>
extern "C"
{
	#include "glad.h"
}
#include "XOpenGLDrv.h"

Fbo::Fbo(int w, int h,
         int samples,
         int numColorAttachments,
         bool depthTexture,
         bool depthRbo,
         GLenum colorFormat)
    : width(w), height(h), samples(samples)
{
    if (colorFormat == 0)
        colorFormat = GL_RGBA8; 
    // Save previous FBO
    prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint*)&prevFbo);

    // Create FBO
    glGenFramebuffers(1, &fboID);
    glBindFramebuffer(GL_FRAMEBUFFER, fboID);

    // --- COLOR ATTACHMENTS ---
    colorTexIDs.resize(numColorAttachments);

    GLenum externalFormat;
    GLenum type;

    switch (colorFormat)
    {
        case GL_RGB16F:
        case GL_RGB32F:
            externalFormat = GL_RGB;
            type = GL_FLOAT;
            break;

        case GL_RGBA16F:
        case GL_RGBA32F:
            externalFormat = GL_RGBA;
            type = GL_FLOAT;
            break;

        default: // GL_RGBA8 or similar
            externalFormat = GL_RGBA;
            type = GL_UNSIGNED_BYTE;
            break;
    }

    for (int i = 0; i < numColorAttachments; i++)
    {
        glGenTextures(1, &colorTexIDs[i]);

        if (samples > 1)
        {
            // --- MULTISAMPLE COLOR ATTACHMENT ---
            glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, colorTexIDs[i]);

            glTexImage2DMultisample(
                GL_TEXTURE_2D_MULTISAMPLE,
                samples,
                colorFormat,        // <-- use the chosen internal format
                width, height,
                GL_TRUE
            );

            glFramebufferTexture2D(
                GL_FRAMEBUFFER,
                GL_COLOR_ATTACHMENT0 + i,
                GL_TEXTURE_2D_MULTISAMPLE,
                colorTexIDs[i],
                0
            );
        }
        else
        {
            // --- SINGLE-SAMPLE COLOR ATTACHMENT ---
            glBindTexture(GL_TEXTURE_2D, colorTexIDs[i]);

            glTexImage2D(
                GL_TEXTURE_2D,
                0,
                colorFormat,        // <-- use the chosen internal format
                width, height,
                0,
                externalFormat,     // <-- derived from internal format
                type,               // <-- derived from internal format
                nullptr
            );

            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

            glFramebufferTexture2D(
                GL_FRAMEBUFFER,
                GL_COLOR_ATTACHMENT0 + i,
                GL_TEXTURE_2D,
                colorTexIDs[i],
                0
            );
        }
    }

    // --- DEPTH ATTACHMENT ---
    if (depthTexture) {
        glGenTextures(1, &depthTexID);

        if (samples > 1) {
            glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, depthTexID);
            glTexImage2DMultisample(GL_TEXTURE_2D_MULTISAMPLE,
                                    samples,
                                    GL_DEPTH_COMPONENT24,
                                    width, height,
                                    GL_TRUE);

            glFramebufferTexture2D(GL_FRAMEBUFFER,
                                   GL_DEPTH_ATTACHMENT,
                                   GL_TEXTURE_2D_MULTISAMPLE,
                                   depthTexID, 0);
        } else {
            glBindTexture(GL_TEXTURE_2D, depthTexID);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24,
                         width, height, 0,
                         GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);

            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL); 

            glFramebufferTexture2D(GL_FRAMEBUFFER,
                                   GL_DEPTH_ATTACHMENT,
                                   GL_TEXTURE_2D,
                                   depthTexID, 0);
        }
    }

    if (depthRbo) {
        glGenRenderbuffers(1, &depthRboID);
        glBindRenderbuffer(GL_RENDERBUFFER, depthRboID);

        if (samples > 1) {
            glRenderbufferStorageMultisample(GL_RENDERBUFFER,
                                             samples,
                                             GL_DEPTH_COMPONENT24,
                                             width, height);
        } else {
            glRenderbufferStorage(GL_RENDERBUFFER,
                                  GL_DEPTH_COMPONENT24,
                                  width, height);
        }

        glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                  GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER,
                                  depthRboID);
    }

    CheckStatus();

    CreateDepthSampler();

    // Restore previous FBO
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
}

Fbo::~Fbo() {
    Dispose();
}

void Fbo::Bind() {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint*)&prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fboID);
}

void Fbo::Unbind() {
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
}

void Fbo::Dispose() {
    if (fboID == 0) return;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    for (GLuint tex : colorTexIDs)
        glDeleteTextures(1, &tex);

    if (depthTexID)
        glDeleteTextures(1, &depthTexID);

    if (depthRboID)
        glDeleteRenderbuffers(1, &depthRboID);

    glDeleteFramebuffers(1, &fboID);

    fboID = 0;
}

void Fbo::CheckStatus() {
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        char msg[128];
        sprintf(msg, "FBO incomplete: %u", status);
        throw msg;
    }
}

void Fbo::CreateDepthSampler(GLint MinFilter, GLint MagFilter)
{
    if (!depthTexID)
        return;

    if (MinFilter == 0) MinFilter = GL_NEAREST;
    if (MagFilter == 0) MagFilter = GL_NEAREST;

    glGenSamplers(1, &depthSampler);
    glSamplerParameteri(depthSampler, GL_TEXTURE_MIN_FILTER, MinFilter);
    glSamplerParameteri(depthSampler, GL_TEXTURE_MAG_FILTER, MagFilter);
    glSamplerParameteri(depthSampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(depthSampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void Fbo::BindDepthTexture(GLuint TextureUnit)
{
    if (!depthTexID)
        return;

    glActiveTexture(GL_TEXTURE0 + TextureUnit);

    GLenum Target = (samples > 1) ? GL_TEXTURE_2D_MULTISAMPLE : GL_TEXTURE_2D;
    glBindTexture(Target, depthTexID);

    if (depthSampler)
        glBindSampler(TextureUnit, depthSampler);
}

GLuint64 Fbo::GetDepthBindlessHandle()
{
    if (!depthTexID)
        return 0;

    if (!depthBindlessHandle)
    {
        depthBindlessHandle = glGetTextureSamplerHandleARB(depthTexID, depthSampler);
        glMakeTextureHandleResidentARB(depthBindlessHandle);
    }

    return depthBindlessHandle;
}