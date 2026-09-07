#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "XOpenGLDrv.h"
#include "XOpenGL.h"

// ------------------------------------------------------------
// Constructor
// ------------------------------------------------------------
UXOpenGLRenderDevice::SSRProgram::SSRProgram(const TCHAR* Name, UXOpenGLRenderDevice* RenDev)
    : ShaderProgramImpl(Name, RenDev)
{
    VertexBufferSize             = 0;
    ParametersBufferSize         = 0;
    ParametersBufferBindingIndex = 0;
    NumTextureSamplers           = 2;   // sceneColor, SSRBuffer
    DrawMode                     = GL_TRIANGLES;
    UseSSBOParametersBuffer      = false;
    ParametersInfo               = nullptr;

    VertexShaderFunc   = nullptr;
    GeoShaderFunc      = nullptr;
    FragmentShaderFunc = nullptr;

    bUseExternalShaders = true;
    ExternalVertexPath   = TEXT("xopengl/shaders/ssr.vert");
    ExternalFragmentPath = TEXT("xopengl/shaders/ssr.frag");
}

// ------------------------------------------------------------
// BindShaderState
// ------------------------------------------------------------
void UXOpenGLRenderDevice::SSRProgram::BindShaderState(CompiledShader* Spec)
{
    ShaderProgramImpl::BindShaderState(Spec);

    //
    // Samplers — bind ONLY sampler slots here (static)
    //
    GetUniformLocation(Spec, uSceneColor, "uSceneColor");
    GetUniformLocation(Spec, uSSRBuffer,  "uSSRBuffer");

    if (uSceneColor != -1) glUniform1i(uSceneColor, 20);
    if (uSSRBuffer  != -1) glUniform1i(uSSRBuffer, 21);

    //
    // Uniform locations — DO NOT set values here
    //
    GetUniformLocation(Spec, uScreenSize,   "uScreenSize");
    GetUniformLocation(Spec, uMaxSteps,     "uMaxSteps");
    GetUniformLocation(Spec, uStepSize,     "uStepSize");
    GetUniformLocation(Spec, uFadeDistance, "uFadeDistance");
}

void UXOpenGLRenderDevice::SSRProgram::MapBuffers() {}
void UXOpenGLRenderDevice::SSRProgram::UnmapBuffers() {}
void UXOpenGLRenderDevice::SSRProgram::Flush(bool Rotate) {}

void UXOpenGLRenderDevice::SSRProgram::ActivateShader()
{
    UseShader();
}

void UXOpenGLRenderDevice::SSRProgram::DeactivateShader()
{
}
