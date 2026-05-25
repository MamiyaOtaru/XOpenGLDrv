#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "XOpenGLDrv.h"
#include "XOpenGL.h"

// ------------------------------------------------------------
// Constructor
// ------------------------------------------------------------
UXOpenGLRenderDevice::SSGIProgram::SSGIProgram(const TCHAR* Name, UXOpenGLRenderDevice* RenDev)
    : ShaderProgramImpl(Name, RenDev)
{
    VertexBufferSize             = 0;
    ParametersBufferSize         = 0;
    ParametersBufferBindingIndex = 0;
    NumTextureSamplers           = 4;   // depth, normal, albedo, noise
    DrawMode                     = GL_TRIANGLES;
    UseSSBOParametersBuffer      = false;
    ParametersInfo               = nullptr;

    VertexShaderFunc   = nullptr;
    GeoShaderFunc      = nullptr;
    FragmentShaderFunc = nullptr;

    bUseExternalShaders = true;
    ExternalVertexPath   = TEXT("xopengl/shaders/ssgi.vert");
    ExternalFragmentPath = TEXT("xopengl/shaders/ssgi.frag");
}

// ------------------------------------------------------------
// BindShaderState
// ------------------------------------------------------------
void UXOpenGLRenderDevice::SSGIProgram::BindShaderState(CompiledShader* Spec)
{
    ShaderProgramImpl::BindShaderState(Spec);

    // Samplers
    GetUniformLocation(Spec, uDepth,  "gDepth");
    GetUniformLocation(Spec, uNormal, "gNormal");
    GetUniformLocation(Spec, uAlbedo, "gAlbedo");
    GetUniformLocation(Spec, uNoise,  "texNoise");

    if (uDepth  != -1) glUniform1i(uDepth,  0);
    if (uNormal != -1) glUniform1i(uNormal, 1);
    if (uAlbedo != -1) glUniform1i(uAlbedo, 2);
    if (uNoise  != -1) glUniform1i(uNoise,  3);

    // Kernel + params
    GetUniformLocation(Spec, uKernelSize, "kernelSize");
    GetUniformLocation(Spec, uRadius,     "radius");
    GetUniformLocation(Spec, uBias,       "bias");
    GetUniformLocation(Spec, uIntensity,  "intensity");

    // Screen size
    GetUniformLocation(Spec, uScreenSize, "uScreenSize");

    // Kernel array
    for (int i = 0; i < 64; i++)
    {
        char name[32];
        sprintf(name, "samples[%d]", i);
        GetUniformLocation(Spec, uSamples[i], name);
    }
}

void UXOpenGLRenderDevice::SSGIProgram::MapBuffers() {}
void UXOpenGLRenderDevice::SSGIProgram::UnmapBuffers() {}
void UXOpenGLRenderDevice::SSGIProgram::Flush(bool Rotate) {}

void UXOpenGLRenderDevice::SSGIProgram::ActivateShader()
{
    UseShader();
}

void UXOpenGLRenderDevice::SSGIProgram::DeactivateShader()
{
}
