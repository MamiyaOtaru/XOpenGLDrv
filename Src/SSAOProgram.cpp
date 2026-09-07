#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include "XOpenGLDrv.h"
#include "XOpenGL.h"

// Constructor
UXOpenGLRenderDevice::SSAOProgram::SSAOProgram(const TCHAR* Name, UXOpenGLRenderDevice* RenDev)
    : ShaderProgramImpl(Name, RenDev)
{
    VertexBufferSize             = 0;
    ParametersBufferSize         = 0;
    ParametersBufferBindingIndex = 0;
    NumTextureSamplers           = 3;
    DrawMode                     = GL_TRIANGLES;
    UseSSBOParametersBuffer      = false;
    ParametersInfo               = nullptr;

    VertexShaderFunc   = nullptr;
    GeoShaderFunc      = nullptr;
    FragmentShaderFunc = nullptr;

    bUseExternalShaders = true;
    ExternalVertexPath   = TEXT("xopengl/shaders/ssao.vert");
    ExternalFragmentPath = TEXT("xopengl/shaders/ssao.frag");
}

// BindShaderState
void UXOpenGLRenderDevice::SSAOProgram::BindShaderState(CompiledShader* Spec)
{
    // Bind all UBOs and default samplers
    ShaderProgramImpl::BindShaderState(Spec);

    // SSAO-specific samplers
    GetUniformLocation(Spec, uDepth,  "gDepth");
    GetUniformLocation(Spec, uNormal, "gNormal");
    GetUniformLocation(Spec, uNoise,  "texNoise");

    if (uDepth  != -1) glUniform1i(uDepth,  1);
    if (uNormal != -1) glUniform1i(uNormal, 2);
    if (uNoise  != -1) glUniform1i(uNoise,  3);

    // SSAO scalar uniforms
    GetUniformLocation(Spec, uKernelSize, "kernelSize");
    GetUniformLocation(Spec, uNearPlane,  "nearPlane");
    GetUniformLocation(Spec, uFarPlane,   "farPlane");
    GetUniformLocation(Spec, uNoiseScale, "noiseScale");

    // SSAO kernel array
    for (int i = 0; i < 32; i++)
    {
        char name[32];
        sprintf(name, "samples[%d]", i);
        GetUniformLocation(Spec, uSamples[i], name);
    }
}

// MapBuffers
void UXOpenGLRenderDevice::SSAOProgram::MapBuffers()
{
}

// UnmapBuffers
void UXOpenGLRenderDevice::SSAOProgram::UnmapBuffers()
{
}

// Flush
void UXOpenGLRenderDevice::SSAOProgram::Flush(bool Rotate)
{
}

// ActivateShader
void UXOpenGLRenderDevice::SSAOProgram::ActivateShader()
{
    UseShader();
}

// DeactivateShader
void UXOpenGLRenderDevice::SSAOProgram::DeactivateShader()
{
}
