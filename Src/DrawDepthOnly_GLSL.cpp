/*=============================================================================
	DrawDepthOnly_GLSL.cpp: UE1 BSP Depth Prepass Rendering Shaders

	Revision history:
		* Created by MamiyaOtaru
=============================================================================*/

#include <glm/glm.hpp>
#include "XOpenGLDrv.h"
#include "XOpenGL.h"

/*-----------------------------------------------------------------------------
	GLSL Code
-----------------------------------------------------------------------------*/

void UXOpenGLRenderDevice::DrawPrepassProgram::BuildVertexShader(GLuint ShaderType, UXOpenGLRenderDevice* GL, FShaderWriterX& Out)
{
    Out << R"(
layout(location = 0) in vec3 Coords;
void main() {
    gl_Position = modelviewprojMat * vec4(Coords.xyz, 1.0);
}
    )";
}

void UXOpenGLRenderDevice::DrawPrepassProgram::BuildFragmentShader(GLuint ShaderType, UXOpenGLRenderDevice* GL, FShaderWriterX& Out)
{
    Out << R"(
layout(location = 0) out vec4 color;
void main() {
  // linearization
  /*float n = 1.0;
  float f = 65336.0;
  float sceneL  = (2.0 * n) / (f + n - gl_FragCoord.z  * (f - n));

  color = vec4(sceneL, 0.0, 0.0, 1.0); */
  // gl_FragDepth is automatically written with the depth value of the fragment
}
    )";
}