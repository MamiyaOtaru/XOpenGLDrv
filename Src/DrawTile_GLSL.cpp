/*=============================================================================
	DrawTile_GLSL.cpp: UE1 Tile Rendering Shaders

	Copyright 2014-2023 OldUnreal

	Revision history:
		* Created by Smirftsch
=============================================================================*/

#include <glm/glm.hpp>
#include "XOpenGLDrv.h"
#include "XOpenGL.h"

/*-----------------------------------------------------------------------------
	Globals
-----------------------------------------------------------------------------*/
const UXOpenGLRenderDevice::ShaderProgram::DrawCallParameterInfo UXOpenGLRenderDevice::DrawTileParametersInfo[]
=
{
	{"vec4", "DrawColor", 0},
	{"uvec4", "TexHandles", 5},
	{"uint", "DrawFlags", 0},
	{"uint", "SceneWidth", 0},
	{"uint", "SceneHeight", 0},
	{"uint", "Dummy0", 0},
	{ nullptr, nullptr, 0}
};

static const char* InterfaceBlockData = R"(
  vec3 Coords;
  vec4 TexCoords;
  vec4 EyeSpacePos;

  // Core only
  vec4 TexCoords1;
  vec4 TexCoords2;
)";

/*-----------------------------------------------------------------------------
	OpenGL ES Tile Shader
-----------------------------------------------------------------------------*/

void UXOpenGLRenderDevice::DrawTileESProgram::BuildVertexShader(GLuint ShaderType, UXOpenGLRenderDevice* GL, FShaderWriterX& Out)
{
	Out << R"(
out VertexData
{
)";
	Out << InterfaceBlockData;
	Out << R"(
} Out;

layout(location = 0) in vec3 Coords; // ==gl_Vertex
layout(location = 1) in uint DrawID; // emulated gl_DrawID
layout(location = 2) in vec2 TexCoords;

void main(void)
{
  Out.EyeSpacePos = modelviewMat * vec4(Coords, 1.0);
  Out.TexCoords = vec4(TexCoords, 0.f, 0.f);
  gl_Position = modelviewprojMat * vec4(Coords, 1.0);
  vDrawID = DrawID;
}
)";
}

void UXOpenGLRenderDevice::DrawTileESProgram::BuildFragmentShader(GLuint ShaderType, UXOpenGLRenderDevice* GL, FShaderWriterX& Out)
{
	Out << R"(
layout(location = 0) out vec4 FragColor;

in VertexData
{
)";
	Out << InterfaceBlockData;
	Out << R"(
} In;
void main(void)
{	
  vec4 TotalColor;
  vec4 Color = GetTexel(GetTexHandle(vDrawID).xy, TMUDiffuse, In.TexCoords.xy);
  uint DrawFlags = GetDrawFlags(vDrawID);

  TotalColor = ApplyPolyFlags(Color, DrawFlags) * GetDrawColor(vDrawID);

  if ((DrawFlags & DF_Modulated) != DF_Modulated)
    TotalColor = GammaCorrect(Gamma, TotalColor);

  FragColor = TotalColor;
}
)";
}

/*-----------------------------------------------------------------------------
	OpenGL Core Tile Shader
-----------------------------------------------------------------------------*/

void UXOpenGLRenderDevice::DrawTileCoreProgram::BuildVertexShader(GLuint ShaderType, UXOpenGLRenderDevice* GL, FShaderWriterX& Out)
{
	Out << R"(
out VertexData
{
)";
	Out << InterfaceBlockData;
	Out << R"(
} Out;

layout(location = 0) in vec3 Coords; // ==gl_Vertex
layout(location = 1) in uint DrawID; // emulated gl_DrawID
layout(location = 2) in vec4 TexCoords0;
layout(location = 3) in vec4 TexCoords1;
layout(location = 4) in vec4 TexCoords2;

void main(void)
{
  Out.EyeSpacePos = modelviewMat * vec4(Coords, 1.0);
  Out.Coords = Coords;
  Out.TexCoords = TexCoords0;
  Out.TexCoords1 = TexCoords1;
  Out.TexCoords2 = TexCoords2;
  gl_Position = vec4(Coords, 1.0);
  vDrawID = DrawID;
}
)";
}

void UXOpenGLRenderDevice::DrawTileCoreProgram::BuildGeometryShader(GLuint ShaderType, UXOpenGLRenderDevice* GL, FShaderWriterX& Out)
{
	Out << R"(
layout(triangles) in;
layout(triangle_strip, max_vertices = 6) out;

in VertexData
{
)";
	Out << InterfaceBlockData;
	Out << R"(
} In[];

out GeometryData
{
)";
	Out << InterfaceBlockData;
	Out << R"(
} Out;

#if OPT_ClipDistance
out float gl_ClipDistance[OPT_MaxClippingPlanes];
#endif

void main()
{
  uint ClipIndex = uint(ClipParams.x);

  float RFX2 = In[0].TexCoords.x;
  float RFY2 = In[0].TexCoords.y;
  float FX2 = In[0].TexCoords.z;
  float FY2 = In[0].TexCoords.w;

  float U = In[0].TexCoords1.x;
  float V = In[0].TexCoords1.y;
  float UL = In[0].TexCoords1.z;
  float VL = In[0].TexCoords1.w;

  float XL = In[0].TexCoords2.x;
  float YL = In[0].TexCoords2.y;
  float UMult = In[0].TexCoords2.z;
  float VMult = In[0].TexCoords2.w;

  float X = gl_in[0].gl_Position.x;
  float Y = gl_in[0].gl_Position.y;
  float Z = gl_in[0].gl_Position.z;

  vec3 Position;

  gDrawID = vDrawID[0];

  Out.TexCoords.zw = vec2(0.f, 0.f);

  // 0
  Position.x = RFX2 * Z * (X - FX2);
  Position.y = RFY2 * Z * (Y - FY2);
  Position.z = Z;
  Out.TexCoords.x = (U)*UMult;
  Out.TexCoords.y = (V)*VMult;
  gl_Position = modelviewprojMat * vec4(Position, 1.0);
#if OPT_ClipDistance
  gl_ClipDistance[ClipIndex] = PlaneDot(ClipPlane, In[0].Coords);
#endif
  EmitVertex();

  // 1
  Position.x = RFX2 * Z * (X + XL - FX2);
  Position.y = RFY2 * Z * (Y - FY2);
  Position.z = Z;
  Out.TexCoords.x = (U + UL) * UMult;
  Out.TexCoords.y = (V)*VMult;
  gl_Position = modelviewprojMat * vec4(Position, 1.0);
#if OPT_ClipDistance
  gl_ClipDistance[ClipIndex] = PlaneDot(ClipPlane, In[1].Coords);
#endif
  EmitVertex();

  // 2
  Position.x = RFX2 * Z * (X + XL - FX2);
  Position.y = RFY2 * Z * (Y + YL - FY2);
  Position.z = Z;
  Out.TexCoords.x = (U + UL) * UMult;
  Out.TexCoords.y = (V + VL) * VMult;
  gl_Position = modelviewprojMat * vec4(Position, 1.0);
#if OPT_ClipDistance
  gl_ClipDistance[ClipIndex] = PlaneDot(ClipPlane, In[2].Coords);
#endif
  EmitVertex();
  EndPrimitive();

  // 0
  Position.x = RFX2 * Z * (X - FX2);
  Position.y = RFY2 * Z * (Y - FY2);
  Position.z = Z;
  Out.TexCoords.x = (U)*UMult;
  Out.TexCoords.y = (V)*VMult;
  gl_Position = modelviewprojMat * vec4(Position, 1.0);
#if OPT_ClipDistance
  gl_ClipDistance[ClipIndex] = PlaneDot(ClipPlane, In[0].Coords);
#endif
  EmitVertex();

  // 2
  Position.x = RFX2 * Z * (X + XL - FX2);
  Position.y = RFY2 * Z * (Y + YL - FY2);
  Position.z = Z;
  Out.TexCoords.x = (U + UL) * UMult;
  Out.TexCoords.y = (V + VL) * VMult;
  gl_Position = modelviewprojMat * vec4(Position, 1.0);
#if OPT_ClipDistance
  gl_ClipDistance[ClipIndex] = PlaneDot(ClipPlane, In[1].Coords);
#endif
  EmitVertex();

  // 3
  Position.x = RFX2 * Z * (X - FX2);
  Position.y = RFY2 * Z * (Y + YL - FY2);
  Position.z = Z;
  Out.TexCoords.x = (U)*UMult;
  Out.TexCoords.y = (V + VL) * VMult;
  gl_Position = modelviewprojMat * vec4(Position, 1.0);
#if OPT_ClipDistance
  gl_ClipDistance[ClipIndex] = PlaneDot(ClipPlane, In[2].Coords);
#endif
  EmitVertex();  

  EndPrimitive();
}
)";
}

void UXOpenGLRenderDevice::DrawTileCoreProgram::BuildFragmentShader(GLuint ShaderType, UXOpenGLRenderDevice* GL, FShaderWriterX& Out)
{
	Out << R"(
# if OPT_ScreenSpaceReflections
// draw to our own color attachment to composite in after reflections etc are resolved
layout(location = 3) out vec4 FragColor;
# else
layout(location = 0) out vec4 FragColor;
# endif

in GeometryData
{
)";
	Out << InterfaceBlockData;
	Out << R"(
} In;

uvec2 GetTexHandleHelper(uint DrawID, uint Index)
{
	uvec4 Handles = GetTexHandles(DrawID, Index / 2u);
	return (Index % 2u == 0u) ? Handles.xy : Handles.zw;
}

float Linearize(float z)
{
    float n = 1.0;  // pass from CPU
    float f = 65336.0;   // pass from CPU
    return (2.0 * n) / (f + n - z * (f - n));
}

void main(void)
{
#if OPT_GeometryShaders
  uint DrawID = gDrawID;
#else
  uint DrawID = vDrawID;
#endif

  uint DrawFlags = GetDrawFlags(DrawID);

  vec4 TotalColor;
  //vec4 Color = GetTexel(GetTexHandle(DrawID).xy, TMUDiffuse, In.TexCoords.xy);
  vec4 Color = GetTexel(GetTexHandleHelper(DrawID, DiffuseTextureIndex), TMUDiffuse, In.TexCoords.xy);

  TotalColor = ApplyPolyFlags(Color, DrawFlags) * GetDrawColor(DrawID);

  if ((DrawFlags & DF_Modulated) != DF_Modulated)
    TotalColor = GammaCorrect(Gamma, TotalColor);

  if ((DrawFlags & DF_ReadDepth) == DF_ReadDepth)
  {
    // depth sampling
    vec2 screenUV = gl_FragCoord.xy /
                    vec2(DrawDrawTileParams[DrawID].SceneWidth,
                         DrawDrawTileParams[DrawID].SceneHeight);

    float sceneZ = GetDepthTexel(GetTexHandleHelper(DrawID, SceneDepthIndex),
                            TMUDepthMap, screenUV).r;
    float spriteZ = gl_FragCoord.z;

    // linearization
    float n = 1.0;
    float f = 65336.0;
    float sceneL  = (2.0 * n) / (f + n - sceneZ  * (f - n));
    float spriteL = (2.0 * n) / (f + n - spriteZ * (f - n));

    float diff = sceneL - spriteL;

    // radial distance
    vec2 uv = In.TexCoords.xy;
    vec2 centered = uv * 2.0 - 1.0;
    float r = length(centered);
    float radialFade = clamp(r / 1.4142, 0.0, 1.0);

    // fade zone widens toward edges
    float minWidth = 0.002;   // narrow at center
    float maxWidth = 0.010;   // wide at edges
    float fadeWidth = mix(minWidth, maxWidth, radialFade);

    // proximity fade with variable width
    float proximityFade = smoothstep(0.0, fadeWidth, diff);

    // additive fade
    if ((DrawFlags & DF_Modulated) != DF_Modulated)
    {
      TotalColor.rgb *= proximityFade;
    }
    else
    {    
        // pretty much only rocket secondary smoke trail
        TotalColor.a *= proximityFade;
    }
  }

#if OPT_ScreenSpaceReflections
// Compute unified visibility
float vis = 1;
if ((DrawFlags & DF_AlphaBlended) == DF_AlphaBlended)
{
    // true alpha-blend mode (UI, at least text)
    vis = TotalColor.a;
}
else if ((DrawFlags & DF_Modulated) == DF_Modulated)
{
    // modulated smoke (decals are in DrawGouraud)
    // TODO eventually need to separate out alpha/modulated from additive (explosions etc) and blend both separately.  
    // No way to have modulated and additive coexist in the same buffer without making the modulated stuff all additive itself 
    // made secondary rocket smoke white for now pending that refactor
    /* // into buffer to be alpha blended later (add if to draw to different output)
    float intensity = dot(TotalColor.rgb, vec3(0.3333));
    vis = abs(intensity - 0.5) * 2.0;
    vis = 1 - vis;
    vis *= vis*vis;
    vis = 1 - vis;
    vis *= TotalColor.a;
    if (intensity < .5) {
        TotalColor.rgb = vec3(0,0,0);
    }
    else {
        TotalColor.rgb = vec3(1,1,1);
    }
    */
    // into buffer that will be blended additively
    float intensity = dot(TotalColor.rgb, vec3(0.3333));
    vis = abs(intensity - 0.5) * 2.0;
    TotalColor.rgb = vec3(vis, vis, vis);
}
TotalColor.a = vis;
#endif

#if OPT_Editor
  if ((DrawFlags & DF_Selected) == DF_Selected)
  {
    TotalColor.g = TotalColor.g - 0.04;
    TotalColor = clamp(TotalColor, 0.0, 1.0);
  }

  if (bool(HitTesting))
    TotalColor = GetDrawColor(DrawID);
#endif

  FragColor = TotalColor;
}
)";
}