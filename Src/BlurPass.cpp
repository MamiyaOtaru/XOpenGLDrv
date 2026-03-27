#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include "XOpenGLDrv.h"
#include "XOpenGL.h"


UXOpenGLRenderDevice::SsaoBlurProgram::SsaoBlurProgram(const TCHAR* Name, UXOpenGLRenderDevice* RenDev)
    : ShaderProgramImpl(Name, RenDev)
    {
        bUseExternalShaders = true;
        ExternalVertexPath   = TEXT("xopengl/shaders/ssao.vert");
        ExternalFragmentPath = TEXT("xopengl/shaders/ssao_blur.frag");
    }


void UXOpenGLRenderDevice::SsaoBlurProgram::BindShaderState(CompiledShader* Spec)
{
	// Cache uniform locations
	ssaoInputLoc = glGetUniformLocation(Spec->ShaderProgramObject, "image");
	offsetLoc    = glGetUniformLocation(Spec->ShaderProgramObject, "offset");
	resolutionLoc = glGetUniformLocation(Spec->ShaderProgramObject, "resolution");
}

void UXOpenGLRenderDevice::SsaoBlurProgram::SetOffset(float x, float y)
{
	glUniform2f(offsetLoc, x, y);
}

void UXOpenGLRenderDevice::SsaoBlurProgram::SetResolution(float w, float h)
{
	glUniform2f(resolutionLoc, w, h);
}

void UXOpenGLRenderDevice::SsaoBlurProgram::SetInput(int unit)
{
	glUniform1i(ssaoInputLoc, unit);
}

void UXOpenGLRenderDevice::SsaoBlurProgram::ActivateShader()
{
    UseShader();
}

