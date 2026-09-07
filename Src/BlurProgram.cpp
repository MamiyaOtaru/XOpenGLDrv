#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include "XOpenGLDrv.h"
#include "XOpenGL.h"

UXOpenGLRenderDevice::BlurProgramBase::BlurProgramBase(const TCHAR* Name, UXOpenGLRenderDevice* RenDev)
    : ShaderProgramImpl(Name, RenDev)
{
    bUseExternalShaders = true;
    ExternalVertexPath = TEXT("xopengl/shaders/ssao.vert"); // shared fullscreen VS
}

void UXOpenGLRenderDevice::BlurProgramBase::BindShaderState(CompiledShader* Spec)
{
    inputLoc = glGetUniformLocation(Spec->ShaderProgramObject, "image");
    offsetLoc = glGetUniformLocation(Spec->ShaderProgramObject, "offset");
    resolutionLoc = glGetUniformLocation(Spec->ShaderProgramObject, "resolution");
}

void UXOpenGLRenderDevice::BlurProgramBase::SetOffset(float x, float y)
{
    glUniform2f(offsetLoc, x, y);
}

void UXOpenGLRenderDevice::BlurProgramBase::SetResolution(float w, float h)
{
    glUniform2f(resolutionLoc, w, h);
}

void UXOpenGLRenderDevice::BlurProgramBase::SetInput(int unit)
{
    glUniform1i(inputLoc, unit);
}

void UXOpenGLRenderDevice::BlurProgramBase::ActivateShader()
{
    UseShader();
}



UXOpenGLRenderDevice::SsaoBlurProgram::SsaoBlurProgram(const TCHAR* Name, UXOpenGLRenderDevice* RenDev)
    : BlurProgramBase(Name, RenDev)
{
    ExternalFragmentPath = TEXT("xopengl/shaders/ssao_blur.frag");
}

UXOpenGLRenderDevice::SsrBlurProgram::SsrBlurProgram(const TCHAR* Name, UXOpenGLRenderDevice* RenDev)
    : BlurProgramBase(Name, RenDev)
{
    ExternalFragmentPath = TEXT("xopengl/shaders/ssr_blur.frag");
}

void UXOpenGLRenderDevice::SsrBlurProgram::BindShaderState(CompiledShader* Spec)
{
    BlurProgramBase::BindShaderState(Spec);
    depthLoc = glGetUniformLocation(Spec->ShaderProgramObject, "depthTex");
}

void UXOpenGLRenderDevice::SsrBlurProgram::SetDepth(int unit)
{
    glUniform1i(depthLoc, unit);
}

