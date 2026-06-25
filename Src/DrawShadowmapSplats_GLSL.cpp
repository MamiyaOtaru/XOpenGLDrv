/*=============================================================================
	DrawShadowMap_GLSL.cpp: UE1 shadow cubemap generation

	Revision history:
		* Created by MamiyaOtaru
=============================================================================*/

#include <glm/glm.hpp>
#include "XOpenGLDrv.h"
#include "XOpenGL.h"

const UXOpenGLRenderDevice::ShaderProgram::DrawCallParameterInfo UXOpenGLRenderDevice::DrawShadowMapSplatsParametersInfo[] =
{
	{ "mat4",  "u_ModelMatrix",    0 },
	{ "mat4",  "u_View",           0 },
	{ "mat4",  "u_Proj",           0 },
	{ "vec4",  "u_LightWorldPos",  0 },
	{ "float", "u_LightRadius",    0 },
	{ "uint",  "u_IsDynamicActor", 0 },
	{ nullptr, nullptr,            0 } // Termination guard sentinel
};

void UXOpenGLRenderDevice::DrawShadowMapSplatsProgram::BuildVertexShader(
    GLuint ShaderType, UXOpenGLRenderDevice* GL, FShaderWriterX& Out)
{
    Out << R"(
layout(location = 0) in vec3 InP0;
layout(location = 1) in vec3 InP1;
layout(location = 2) in float InRadius;
layout(location = 3) in uint InDrawID;

out VS_OUT {
    vec3 P0;
    vec3 P1;
    float Radius;
} vs_out;

void main()
{
    vs_out.P0 = InP0;
    vs_out.P1 = InP1;
    vs_out.Radius = InRadius;

    vDrawID = InDrawID; // injected by framework
}
    )";
}

void UXOpenGLRenderDevice::DrawShadowMapSplatsProgram::BuildGeometryShader(
    GLuint ShaderType, UXOpenGLRenderDevice* GL, FShaderWriterX& Out)
{
    Out << R"(
layout(points) in;
layout(triangle_strip, max_vertices = 10) out;

in VS_OUT {
    vec3 P0;
    vec3 P1;
    float Radius;
} gs_in[];

out vec3 v_WorldPos;

void EmitVS(vec3 posWS, vec3 posVS, mat4 P)
{
    v_WorldPos = posWS;
    gl_Position = P * vec4(posVS, 1.0);
    EmitVertex();
}

void main()
{
    uint id = vDrawID[0];
    gDrawID = id;

    vec3 p0 = gs_in[0].P0;   // world space
    vec3 p1 = gs_in[0].P1;   // world space
    float r = gs_in[0].Radius;

    mat4 V = DrawDrawShadowMapSplatsParams[id].u_View;
    mat4 P = DrawDrawShadowMapSplatsParams[id].u_Proj;

    // 1. Transform endpoints to VIEW SPACE
    vec3 vP0 = (V * vec4(p0, 1.0)).xyz;
    vec3 vP1 = (V * vec4(p1, 1.0)).xyz;

    vec3 seg = vP1 - vP0;
    float segLen = length(seg);
    if (segLen < 0.0001) segLen = 0.0001;

    vec3 segDir = seg / segLen;

    // --- Stabilize near-view-direction segments ---
    if (abs(segDir.z) > 0.85) {
        float xyLen = length(segDir.xy);
        vec2 xy = (xyLen > 0.0001) ? segDir.xy / xyLen : vec2(1.0, 0.0);
        segDir = vec3(xy, 0.0);
    }

    // 2. Build 2D basis in VIEW SPACE
    vec3 viewForward = segDir;
    vec3 viewRight   = vec3(-viewForward.y, viewForward.x, 0.0);
    
    float rightLen = length(viewRight);
    if (rightLen < 0.0001)
        viewRight = vec3(1.0, 0.0, 0.0);
    else
        viewRight /= rightLen;

    // 3. Convert basis to WORLD SPACE (rotation only)
    mat3 invRot = mat3(
        V[0][0], V[1][0], V[2][0],
        V[0][1], V[1][1], V[2][1],
        V[0][2], V[1][2], V[2][2]
    );
    vec3 rightWS   = invRot * viewRight;
    vec3 forwardWS = invRot * viewForward;

    float h = 0.70710678;

    // --- P0 ENDCAP POINTS ---
    vec3 v0_back   = vP0 - viewForward * r;
    vec3 w0_back   = p0  - forwardWS   * r;

    vec3 v0_topDia = vP0 - viewRight * (r * h) - viewForward * (r * h);
    vec3 w0_topDia = p0  - rightWS   * (r * h) - forwardWS   * (r * h);

    vec3 v0_botDia = vP0 + viewRight * (r * h) - viewForward * (r * h);
    vec3 w0_botDia = p0  + rightWS   * (r * h) - forwardWS   * (r * h);

    vec3 v0_topFlk = vP0 - viewRight * r;
    vec3 w0_topFlk = p0  - rightWS   * r;

    vec3 v0_botFlk = vP0 + viewRight * r;
    vec3 w0_botFlk = p0  + rightWS   * r;

    // --- P1 ENDCAP POINTS ---
    vec3 v1_topFlk = vP1 - viewRight * r;
    vec3 w1_topFlk = p1  - rightWS   * r;

    vec3 v1_botFlk = vP1 + viewRight * r;
    vec3 w1_botFlk = p1  + rightWS   * r;

    vec3 v1_topDia = vP1 - viewRight * (r * h) + viewForward * (r * h);
    vec3 w1_topDia = p1  - rightWS   * (r * h) + forwardWS   * (r * h);

    vec3 v1_botDia = vP1 + viewRight * (r * h) + viewForward * (r * h);
    vec3 w1_botDia = p1  + rightWS   * (r * h) + forwardWS   * (r * h);

    vec3 v1_front  = vP1 + viewForward * r;
    vec3 w1_front  = p1  + forwardWS   * r;

    // 5. Emit strip
    EmitVS(w0_back,   v0_back,   P);
    EmitVS(w0_topDia, v0_topDia, P);
    EmitVS(w0_botDia, v0_botDia, P);
    EmitVS(w0_topFlk, v0_topFlk, P);
    EmitVS(w0_botFlk, v0_botFlk, P);
    EmitVS(w1_topFlk, v1_topFlk, P);
    EmitVS(w1_botFlk, v1_botFlk, P);
    EmitVS(w1_topDia, v1_topDia, P);
    EmitVS(w1_botDia, v1_botDia, P);
    EmitVS(w1_front,  v1_front,  P);

    EndPrimitive();
}
    )";
}

void UXOpenGLRenderDevice::DrawShadowMapSplatsProgram::BuildFragmentShader(
    GLuint ShaderType, UXOpenGLRenderDevice* GL, FShaderWriterX& Out)
{
    Out << R"(
layout(location = 0) out vec4 out_ShadowData;

in vec3 v_WorldPos;
in float v_Radius;

void main()
{
    // Compute linear depth
    //vec3 lightPos = DrawDrawShadowMapSplatsParams[gDrawID].u_LightWorldPos.xyz;
    //float radius  = DrawDrawShadowMapSplatsParams[gDrawID].u_LightRadius;

    //float d = length(v_WorldPos - lightPos);
    //out_ShadowData.r = clamp(d / radius, 0.0, 1.0);

    out_ShadowData.r = 1.0;
    out_ShadowData.a = 1.0;
}
    )";
}


