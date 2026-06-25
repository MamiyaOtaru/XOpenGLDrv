/*=============================================================================
	DrawShadowMap_GLSL.cpp: UE1 shadow cubemap generation

	Revision history:
		* Created by MamiyaOtaru
=============================================================================*/

#include <glm/glm.hpp>
#include "XOpenGLDrv.h"
#include "XOpenGL.h"

const UXOpenGLRenderDevice::ShaderProgram::DrawCallParameterInfo UXOpenGLRenderDevice::DrawShadowMapParametersInfo[] =
{
	{ "mat4",  "u_ModelMatrix",    0 },
	{ "mat4",  "u_View",           0 },
	{ "mat4",  "u_Proj",           0 },
	{ "vec4",  "u_LightWorldPos",  0 },
	{ "float", "u_LightRadius",    0 },
	{ "uint",  "u_IsDynamicActor", 0 },
	{ nullptr, nullptr,            0 } // Termination guard sentinel
};

void UXOpenGLRenderDevice::DrawShadowMapProgram::BuildVertexShader(GLuint ShaderType, UXOpenGLRenderDevice* GL, FShaderWriterX& Out)
{
    Out << R"(
layout(location = 0) in vec3 Coords;
layout(location = 1) in uint DrawID;  // Consumes the framework's native attribute layout channel
layout(location = 2) in uint Class; 
layout(location = 3) in uint Padding;

out vec3 v_WorldPos;
out float v_IsDynamic; // Pass varying flag down to fragment shader stage

void main() {
    // =========================================================================
    // NATIVE MULTI-DRAW MATCHING:
    // Safely reads from the vertex-passed DrawID to index the exact matrix slice
    // matching this specific surface or dynamic mesh draw call execution block.
    // =========================================================================
    uint id = DrawID;
    vDrawID = id;
    v_IsDynamic = Class; // Route your classification indicator smoothly

    vec4 worldPos = DrawDrawShadowMapParams[id].u_ModelMatrix * vec4(Coords.xyz, 1.0);
    v_WorldPos = worldPos.xyz;

    gl_Position = DrawDrawShadowMapParams[id].u_Proj * DrawDrawShadowMapParams[id].u_View * worldPos;
}
    )";
}

void UXOpenGLRenderDevice::DrawShadowMapProgram::BuildFragmentShader(GLuint ShaderType, UXOpenGLRenderDevice* GL, FShaderWriterX& Out)
{
    Out << R"(
layout(location = 0) out vec4 out_ShadowData;    // Bound to Attachment 0 (GL_R16F)

in vec3 v_WorldPos;
in float v_IsDynamic; // Accept dynamic flag varying from vertex shader

void main() {
    // Note: Light world parameters are identical across all draw element blocks 
    // within the active frame pass loop, so reading from index 0 is perfectly correct.
    uint id = vDrawID;

    // 1. Calculate absolute vector distance to light source center extracting .xyz component
    float distanceToLight = length(v_WorldPos - DrawDrawShadowMapParams[id].u_LightWorldPos.xyz);
    
    // Normalize into a clean 0.0 - 1.0 range based on your UT99 light radius constant
    if (v_IsDynamic < 0.5) {
        out_ShadowData.r = clamp(distanceToLight / DrawDrawShadowMapParams[id].u_LightRadius, 0.0, 1.0);
    }
    else {
        out_ShadowData.r = 1.0;
    }

    // =========================================================================
    // VERTEX STREAM CLASSIFICATION PROJECTION:
    // If the vertex-passed flag is greater than 0.5, write a sharp 1.0 (Red) 
    // to isolate character silhouettes inside the classification mask attachment.
    // =========================================================================
    out_ShadowData.a = v_IsDynamic;//(v_IsDynamic > 0.5) ? 1.0 : 0.0;
}
    )";
}
