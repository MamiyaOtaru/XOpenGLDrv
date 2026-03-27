#pragma once
#include <vector>

// Minimal GL type forward declarations (no GL headers here)
typedef int           GLint;
typedef unsigned int  GLuint;
typedef unsigned long long GLuint64;
typedef unsigned int  GLenum;
typedef int           GLsizei;

class Fbo
{
public:
    GLuint fboID = 0;
    GLuint depthTexID = 0;
    GLuint depthRboID = 0;
    std::vector<GLuint> colorTexIDs;

    int width = 0;
    int height = 0;
    int samples = 1;

    GLuint depthSampler = 0;
    GLuint64 depthBindlessHandle = 0;
 
    // NOTE: no GL_RGBA8 here – just a plain GLenum with a default 0
    Fbo(int W,
        int H,
        int InSamples,
        int NumColorAttachments,
        bool bDepthTexture,
        bool bDepthRbo,
        GLenum colorFormat = 0);

    ~Fbo();

    void Bind();
    void Unbind();
    void Dispose();

    // Declarations only – implementations go in FBO.cpp
    void CreateDepthSampler(GLint MinFilter = 0, GLint MagFilter = 0);
    void BindDepthTexture(GLuint TextureUnit);
    GLuint64 GetDepthBindlessHandle();

private:
    GLuint prevFbo = 0;

    void CheckStatus();
};
