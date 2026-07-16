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

    // Enable all color attachments as draw buffers
    if (!colorTexIDs.empty())
    {
        std::vector<GLenum> bufs(colorTexIDs.size());
        for (size_t i = 0; i < colorTexIDs.size(); ++i)
            bufs[i] = GL_COLOR_ATTACHMENT0 + (GLenum)i;

        glDrawBuffers((GLsizei)bufs.size(), bufs.data());
    }

    CheckStatus();

    CreateDepthSampler();

    // Restore previous FBO
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
}

// Custom static constructor for individual cubemap face FBO view targets
Fbo::Fbo(int size, GLuint sharedColorCubemapID, GLuint sharedDepthCubemapID, int faceIndex)
    : width(size), height(size), samples(1), isCubemap(true)
{
    prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint*)&prevFbo);

    // Instantiate our hardware frame target block cleanly
    glGenFramebuffers(1, &fboID);
    glBindFramebuffer(GL_FRAMEBUFFER, fboID);

    // Statically bind this FBO container to its target cubemap color face stride!
    glFramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_COLOR_ATTACHMENT0,
        GL_TEXTURE_CUBE_MAP_POSITIVE_X + faceIndex,
        sharedColorCubemapID,
        0
    );

    // Statically bind this FBO container to its target cubemap depth face stride!
    glFramebufferTexture2D(
        GL_FRAMEBUFFER,
        GL_DEPTH_ATTACHMENT,
        GL_TEXTURE_CUBE_MAP_POSITIVE_X + faceIndex,
        sharedDepthCubemapID,
        0
    );

    // Execute all of your standard class completion steps seamlessly!
    GLenum bufs[] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, bufs);

    CheckStatus();

    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
}

// all in one cubemap constructor
// NOTE: HeroLight now uses 6 individual FBOs instead of this shared cubemap approach
// because FBO attachment state doesn't survive frame boundaries when reconfigured.
// This constructor remains available if a future use case needs a single cubemap FBO
// with a shared depth RBO (non-persistent across frames).
Fbo::Fbo(int size, int numColorAttachments, GLenum colorFormat)
    : width(size), height(size), samples(1), isCubemap(true)
{
    if (colorFormat == 0)
        colorFormat = GL_RGBA8;

    prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint*)&prevFbo);

    glGenFramebuffers(1, &fboID);
    glBindFramebuffer(GL_FRAMEBUFFER, fboID);

    colorTexIDs.resize(numColorAttachments);

    GLenum externalFormat = GL_RGBA;
    GLenum type = GL_UNSIGNED_BYTE;

    if (colorFormat == GL_R32F || colorFormat == GL_R16F) {
        externalFormat = GL_RED;
        type = GL_FLOAT;
    } else if (colorFormat == GL_RGB16F || colorFormat == GL_RGB32F) {
        externalFormat = GL_RGB;
        type = GL_FLOAT;
    } else if (colorFormat == GL_RGBA16F || colorFormat == GL_RGBA32F) {
        externalFormat = GL_RGBA;
        type = GL_FLOAT;
    }

    // Allocate the multi-attachment cubemaps
    for (int i = 0; i < numColorAttachments; i++)
    {
        glGenTextures(1, &colorTexIDs[i]);
        glBindTexture(GL_TEXTURE_CUBE_MAP, colorTexIDs[i]);

        // Loop over all 6 faces of the cubemap to reserve VRAM allocation space
        for (int face = 0; face < 6; face++)
        {
            glTexImage2D(
                GL_TEXTURE_CUBE_MAP_POSITIVE_X + face,
                0, colorFormat, width, height, 0,
                externalFormat, type, nullptr
            );
        }

        // Texture parameter filters for point shadows
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

        // =========================================================================
        // FIX PART A: Pre-bind a default target face context to the attachments!
        // This mirrors your 2D FBO constructor loop and ensures the FBO is complete.
        // =========================================================================
        glFramebufferTexture2D(
            GL_FRAMEBUFFER,
            GL_COLOR_ATTACHMENT0 + i,
            GL_TEXTURE_CUBE_MAP_POSITIVE_X, // Safe default face index
            colorTexIDs[i],
            0
        );
        // =========================================================================
    }

    // Create a local Shared Depth Renderbuffer
    glGenRenderbuffers(1, &depthRboID);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRboID);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRboID);

    // =========================================================================
    // FIX PART B: Enable all color attachments as draw buffers (MRT Mirror)!
    // This unlocks multi-render target writing paths across your system.
    // =========================================================================
    if (!colorTexIDs.empty())
    {
        std::vector<GLenum> bufs(colorTexIDs.size());
        for (size_t i = 0; i < colorTexIDs.size(); ++i)
            bufs[i] = GL_COLOR_ATTACHMENT0 + (GLenum)i;

        glDrawBuffers((GLsizei)bufs.size(), bufs.data());
    }
    // =========================================================================

    CheckStatus();
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
}

Fbo::~Fbo() {
    Dispose();
}

void Fbo::Bind() {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, (GLint*)&prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fboID);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        debugf(TEXT("FBO broken between frames!"));
    }
}

void Fbo::Unbind() {
    glBindFramebuffer(GL_FRAMEBUFFER, prevFbo);
}

GLuint Fbo::GetColorTexID(GLuint index) {
    if (index >= colorTexIDs.size()) return 0;
    return colorTexIDs[index];
}

GLuint Fbo::GetDepthTexID() {
    return depthTexID;
}

// old way to bind a texture unit.  Used for debug only
void Fbo::BindColorCubemap(GLuint attachmentIndex, GLuint textureUnit)
{
    if (attachmentIndex >= colorTexIDs.size()) return;
    glActiveTexture(GL_TEXTURE0 + textureUnit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, colorTexIDs[attachmentIndex]);
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
