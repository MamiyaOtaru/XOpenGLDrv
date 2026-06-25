/*=============================================================================
	DrawComplex_GLSL.cpp: UE1 BSP Rendering Shaders

	Copyright 2014-2023 OldUnreal

	Revision history:
		* Created by Smirftsch
=============================================================================*/

#include <glm/glm.hpp>
#include "XOpenGLDrv.h"
#include "XOpenGL.h"

/*-----------------------------------------------------------------------------
	GLSL Code
-----------------------------------------------------------------------------*/

const UXOpenGLRenderDevice::ShaderProgram::DrawCallParameterInfo UXOpenGLRenderDevice::DrawComplexParametersInfo[]
=
{
	{"vec4", "DiffuseUV", 0},
	{"vec4", "LightMapUV", 0},
	{"vec4", "FogMapUV", 0},
	{"vec4", "DetailUV", 0},
	{"vec4", "MacroUV", 0},
	{"vec4", "EnviroMapUV", 0},
	{"vec4", "DiffuseInfo", 0},
	{"vec4", "MacroInfo", 0},
	{"vec4", "BumpMapInfo", 0},
	{"vec4", "HeightMapInfo", 0},
	{"vec4", "XAxis", 0},
	{"vec4", "YAxis", 0},
	{"vec4", "ZAxis", 0},
	{"vec4", "DrawColor", 0},
    {"uvec4", "TexHandles", 7},
	{"uint", "DrawFlags", 0},
    {"float", "Roughness", 0},
    {"uint", "SceneWidth", 0},
    {"uint", "SceneHeight", 0},
	{ nullptr, nullptr, 0}
};

// Vertexshader for DrawComplexSurface, single pass.
void UXOpenGLRenderDevice::DrawComplexProgram::BuildVertexShader(GLuint ShaderType, UXOpenGLRenderDevice * GL, FShaderWriterX & Out)
{
    Out << R"(
layout(location = 0) in vec3 Coords; // == gl_Vertex
layout(location = 1) in uint DrawID; // emulated gl_DrawID
layout(location = 2) in vec4 Normal;
layout(location = 3) in vec4 Tangent;
layout(location = 4) in vec4 Bitangent;
layout(location = 5) in uint FacetID;
layout(location = 6) in vec2 StaticLightmapCoords;

out vec3 vCoords;
out vec2 vTexCoords;
out vec2 vLightMapCoords;
out vec2 vStaticLightmapCoords;
out vec2 vFogMapCoords;
flat out uint vFacetID;
out vec3 vNormal; // interpolated per-vertex normal (view-space)

#if OPT_DetailTextures
out vec2 vDetailTexCoords;
#endif

#if OPT_MacroTextures
out vec2 vMacroTexCoords;
#endif

#if OPT_EnvironmentMaps
out vec2 vEnvironmentTexCoords;
#endif

#if OPT_BumpMaps
out vec2 vBumpTexCoords;
#endif

#if OPT_BumpMaps || OPT_HWLighting || OPT_HeightMaps
out mat3 vTBNMat_geo;
out vec3 vN;
out vec3 vT;
out vec3 vB;
#endif

#if OPT_BumpMaps || OPT_DistanceFog || OPT_HWLighting
out vec4 vEyeSpacePos;
#endif

#if OPT_ClipDistance
out float gl_ClipDistance[OPT_MaxClippingPlanes];
#endif

void main(void)
{
  // Point Coords
  vCoords = Coords.xyz;
  vStaticLightmapCoords = StaticLightmapCoords;

  // UDot/VDot calculation.
  vec3 MapCoordsXAxis = GetXAxis(DrawID).xyz;
  vec3 MapCoordsYAxis = GetYAxis(DrawID).xyz;
#if OPT_Editor || OPT_BumpMaps || OPT_HWLighting || OPT_HeightMaps
  vec3 MapCoordsZAxis = GetZAxis(DrawID).xyz;
#endif

  uint DrawFlags = GetDrawFlags(DrawID);

  float UDot = GetXAxis(DrawID).w;
  float VDot = GetYAxis(DrawID).w;
  
  float MapDotU = dot(MapCoordsXAxis, Coords.xyz) - UDot;
  float MapDotV = dot(MapCoordsYAxis, Coords.xyz) - VDot;
  vec2  MapDot = vec2(MapDotU, MapDotV);

  // Texture UV to fragment
  vec2 TexMapMult = GetDiffuseUV(DrawID).xy;
  vec2 TexMapPan = GetDiffuseUV(DrawID).zw;
  vTexCoords = (MapDot - TexMapPan) * TexMapMult;

  // Texture UV Lightmap to fragment
  if ((DrawFlags & DF_LightMap) == DF_LightMap)
  {
    vec2 LightMapMult = GetLightMapUV(DrawID).xy;
    vec2 LightMapPan = GetLightMapUV(DrawID).zw;
    vLightMapCoords = (MapDot - LightMapPan) * LightMapMult;
  }

  // Texture UV FogMap
  if ((DrawFlags & DF_FogMap) == DF_FogMap)
  {
    vec2 FogMapMult = GetFogMapUV(DrawID).xy;
    vec2 FogMapPan = GetFogMapUV(DrawID).zw;
    vFogMapCoords = (MapDot - FogMapPan) * FogMapMult;
  }

  // Texture UV DetailTexture
#if OPT_DetailTextures
  if ((DrawFlags & DF_DetailTexture) == DF_DetailTexture)
  {
    vec2 DetailMult = GetDetailUV(DrawID).xy;
    vec2 DetailPan = GetDetailUV(DrawID).zw;
    vDetailTexCoords = (MapDot - DetailPan) * DetailMult;
  }
#endif

  // Texture UV Macrotexture
#if OPT_MacroTextures
  if ((DrawFlags & DF_MacroTexture) == DF_MacroTexture)
  {
    vec2 MacroMult = GetMacroUV(DrawID).xy;
    vec2 MacroPan = GetMacroUV(DrawID).zw;
    vMacroTexCoords = (MapDot - MacroPan) * MacroMult;
  }
#endif

  // Texture UV EnvironmentMap
#if OPT_EnvironmentMaps
  if ((DrawFlags & DF_EnvironmentMap) == DF_EnvironmentMap)
  {
    vec2 EnvironmentMapMult = GetEnviroMapUV(DrawID).xy;
    vec2 EnvironmentMapPan = GetEnviroMapUV(DrawID).zw;
    vEnvironmentTexCoords = (MapDot - EnvironmentMapPan) * EnvironmentMapMult;
  }
#endif

#if OPT_BumpMaps || OPT_HWLighting || OPT_DistanceFog
  vEyeSpacePos = modelviewMat * vec4(Coords.xyz, 1.0);
#endif

  // --- Read and normalize attribute normal first (input is expected in view-space) ---
  vec3 attrNormal = normalize(Normal.xyz);
  if (length(attrNormal) < 1e-6)
  {
#if OPT_Editor || OPT_BumpMaps || OPT_HWLighting || OPT_HeightMaps
    attrNormal = normalize(MapCoordsZAxis);
#else
    attrNormal = vec3(0.0, 0.0, 1.0);
#endif
  }
  vNormal = attrNormal; // pass to fragment shader (interpolated)

#if OPT_BumpMaps || OPT_HWLighting || OPT_HeightMaps
  // Inputs:
  //   attrNormal         : smoothed per-vertex normal (view-space)
  //   Tangent.xyz        : tangent direction (view-space)
  //   Tangent.w          : handedness sign (+1/-1)
  //   modelviewMat       : world->view matrix
  //   vCoords            : view-space position

  vec3 T_geo = normalize(vec3(MapCoordsXAxis.x, MapCoordsXAxis.y, MapCoordsXAxis.z));
  vec3 B_geo = normalize(vec3(MapCoordsYAxis.x, MapCoordsYAxis.y, MapCoordsYAxis.z));
  vec3 N_geo = normalize(vec3(MapCoordsZAxis.x, MapCoordsZAxis.y, MapCoordsZAxis.z));

  vTBNMat_geo = transpose(mat3(T_geo, B_geo, N_geo));

  vec3 T_smooth = T_geo;
  vec3 B_smooth = B_geo;
  vec3 N_smooth = N_geo;
#if OPT_PhongShading
  if ((DrawFlags & DF_PhongShading) == DF_PhongShading) {
      T_smooth = Tangent.xyz;
      B_smooth = Bitangent.xyz;
      N_smooth = Normal.xyz;
  }
#endif

  vN = N_smooth;
  vT = T_smooth;
  vB = B_smooth;
#endif

  gl_Position = modelviewprojMat * vec4(Coords.xyz, 1.0);
  vDrawID = DrawID;
  vFacetID = FacetID;

#if OPT_ClipDistance
  uint ClipIndex = uint(ClipParams.x);
  gl_ClipDistance[ClipIndex] = PlaneDot(ClipPlane, Coords.xyz);
#endif
}
)";
}

static void EmitParallaxFunction(UXOpenGLRenderDevice* GL, FShaderWriterX& Out)
{
    Out << R"(
#if OPT_HeightMaps
bool is_nan(float v) {
    return v != v;
}
bool is_inf(float v) {
    return abs(v) > 1e20;   // any huge threshold works
}
bool is_finite(float v) {
    return !is_nan(v) && !is_inf(v);
}
bool any_nan(vec2 v) { return is_nan(v.x) || is_nan(v.y); }
bool any_nan(vec3 v) { return is_nan(v.x) || is_nan(v.y) || is_nan(v.z); }

bool any_inf(vec2 v) { return is_inf(v.x) || is_inf(v.y); }
bool any_inf(vec3 v) { return is_inf(v.x) || is_inf(v.y) || is_inf(v.z); }

bool any_nonfinite(vec2 v) { return any_nan(v) || any_inf(v); }
bool any_nonfinite(vec3 v) { return any_nan(v) || any_inf(v); }

vec2 ParallaxMapping(vec2 ptexCoords, vec3 viewDir, uvec2 TexHandle, out float parallaxHeight)
{
    if (any_nonfinite(viewDir))
        return ptexCoords;
    float vParallaxScale = GetHeightMapInfo(vDrawID).z * 0.025;
    float vTimeSeconds = GetHeightMapInfo(vDrawID).w; // Surface.Level->TimeSeconds
        )";
        if (GL->ParallaxVersion == Parallax_Basic) // very basic implementation
        {
            Out << R"(
  float height = 1.0 - GetTexel(TexHandle, TMUHeightMap, ptexCoords).r;
  return ptexCoords - viewDir.xy * (height * 0.1);
}
#endif
)";
        }
        else if (GL->ParallaxVersion == Parallax_Occlusion) // parallax occlusion mapping
        {
            constexpr FLOAT minLayers = 8.f;
            constexpr FLOAT maxLayers = 32.f;

            Out << "  float numLayers = mix(" << maxLayers << ", " << minLayers << ", abs(dot(vec3(0.0, 0.0, 1.0), viewDir)));" END_LINE;
            Out << R"(  
  //vParallaxScale += 8.0f * sin(vTimeSeconds) + 4.0 * cos(2.3f * vTimeSeconds);
  // number of depth layers		
  float layerDepth = 1.0 / numLayers; // calculate the size of each layer
  float currentLayerDepth = 0.0; // depth of current layer

  // the amount to shift the texture coordinates per layer (from vector P)
  float vz = max(abs(viewDir.z), 0.02);
  vec2 P = viewDir.xy / vz * vParallaxScale;
  vec2 deltaTexCoords = P / numLayers;

  // get initial values
  vec2  currentTexCoords = ptexCoords;
  float currentDepthMapValue = 0.0;
  currentDepthMapValue = 1.0 - GetTexel(TexHandle, TMUHeightMap, currentTexCoords).r;

  while (currentLayerDepth < currentDepthMapValue)
  {
    currentTexCoords -= deltaTexCoords; // shift texture coordinates along direction of P
    currentDepthMapValue = 1.0 - GetTexel(TexHandle, TMUHeightMap, currentTexCoords).r; // get depthmap value at current texture coordinates
    currentLayerDepth += layerDepth; // get depth of next layer
  }

  vec2 prevTexCoords = currentTexCoords + deltaTexCoords; // get texture coordinates before collision (reverse operations)

  // get depth after and before collision for linear interpolation
  float afterDepth = currentDepthMapValue - currentLayerDepth;
  float beforeDepth = 1.0 - GetTexel(TexHandle, TMUHeightMap, currentTexCoords).r - currentLayerDepth + layerDepth;

  // interpolation of texture coordinates
  float denom = afterDepth - beforeDepth;
  // If denom is too small, skip interpolation entirely
  if (abs(denom) < 1e-5)
    return currentTexCoords;
  float weight = afterDepth / denom;
  vec2 finalTexCoords = prevTexCoords * weight + currentTexCoords * (1.0 - weight);
  if (any_nonfinite(finalTexCoords))
        return ptexCoords;

    return finalTexCoords;
}
#endif
)";
        }
        else if (GL->ParallaxVersion == Parallax_Relief) // Relief Parallax Mapping
        {
            // determine required number of layers
            constexpr FLOAT minLayers   = 4.f;
            constexpr FLOAT maxLayers   = 20.f;
            constexpr INT   numSearches = 5;
    Out << R"(
  int mipLevel = int(floor(textureQueryLOD(Texture7, ptexCoords).y));
  //if (mipLevel > 3) return ptexCoords;

  // BasicRace-style angle factor
  float ndotv = abs(dot(vec3(0,0,1), viewDir));  // 1 = head-on, 0 = grazing
  // Fade region
  float fadeStart = 0.45;   // 0.25 ~75°
  float fadeEnd   = 0.05;   // 0.05 ~87°
  // Normalize ndotv into [0,1] fade space
  float t = clamp((ndotv - fadeEnd) / (fadeStart - fadeEnd), 0.0, 1.0);
  // Ease-in at the start, linear at the end
  float eased = t * t * (3.0 - 2.0 * t);  // smoothstep
  float linear = t;
  // Blend between smoothstep (early) and linear (late)
  float angleFactor = mix(eased, linear, t);

  float minLayers = )" << minLayers << R"(;
  float maxLayers = )" << maxLayers << R"(;
  float maxLayersMipped = max(minLayers, maxLayers / pow(2, max(0,mipLevel)));
  float numLayers = mix(maxLayersMipped, minLayers, angleFactor );  
  float layerHeight = 1.0 / numLayers;
  float currentLayerHeight = 0.0;

  // BasicRace-style P vector (angleFactor applied here)
  float vz = max(abs(viewDir.z), 0.02);
  vec2 P = (vParallaxScale * viewDir.xy * angleFactor) / vz;
  vec2 delta = P / numLayers;

  vec2 currentTexCoords = ptexCoords;
  float height = 1.0 - GetTexel(TexHandle, Texture7, currentTexCoords).r;

  // Coarse Relief search
  while (height > currentLayerHeight)
  {
    currentLayerHeight += layerHeight;
    currentTexCoords -= delta;
    height = 1.0 - GetTexel(TexHandle, Texture7, currentTexCoords).r;
  }

  // Binary search refinement
  vec2 prevTexCoords = currentTexCoords + delta;
  float prevLayerHeight = currentLayerHeight - layerHeight;

  vec2 a = prevTexCoords;
  vec2 b = currentTexCoords;
  float aH = prevLayerHeight;
  float bH = currentLayerHeight;

  for (int i = 0; i < )" << numSearches << R"(; i++)
  {
    vec2 mid = (a + b) * 0.5;
    float midH = (aH + bH) * 0.5;
    float midTexH = 1.0 - GetTexel(TexHandle, Texture7, mid).r;

    if (midTexH > midH)
    {
      a = mid;
      aH = midH;
    }
    else
    {
      b = mid;
      bH = midH;
    }
  }

  parallaxHeight = bH;
  return b;
}
#endif
)";

        }
        else
        {
            Out << R"(
  return ptexCoords;
}
#endif
)";
        }
}

void UXOpenGLRenderDevice::DrawComplexProgram::BuildFragmentShader(GLuint ShaderType, UXOpenGLRenderDevice* GL, FShaderWriterX& Out)
{
    Out << R"(
in vec3 vCoords;
in vec2 vTexCoords;
in vec2 vLightMapCoords;
in vec2 vFogMapCoords;
flat in uint vFacetID;
in vec3 vNormal; // interpolated per-vertex normal (view-space)
in vec2 vStaticLightmapCoords;

#if OPT_DetailTextures
in vec2 vDetailTexCoords;
#endif

#if OPT_MacroTextures
in vec2 vMacroTexCoords;
#endif

#if OPT_EnvironmentMaps
in vec2 vEnvironmentTexCoords;
#endif

#if OPT_BumpMaps
in vec2 vBumpTexCoords;
#endif

#if OPT_BumpMaps || OPT_HWLighting || OPT_HeightMaps
in mat3 vTBNMat_geo;
in vec3 vN;
in vec3 vT;
in vec3 vB;
#endif

#if OPT_BumpMaps || OPT_DistanceFog
in vec4 vEyeSpacePos;
#endif

#if OPT_GLES
// Primary color output
  layout(location = 0) out vec4 FragColor;

  #if OPT_SimulateMultiPass
    layout(location = 1) out vec4 FragColor1;
  #endif

  #if OPT_IndirectIllumination
    // Albedo output for SSDO/SSGI
    layout(location = 2) out vec4 Albedo;
  #endif
#else // Desktop GL
  #if OPT_SimulateMultiPass
    layout(location = 0, index = 1) out vec4 FragColor1;
  #endif

  layout(location = 0, index = 0) out vec4 FragColor;

  #if OPT_IndirectIllumination
    // Albedo output for SSDO/SSGI
    layout(location = 2) out vec4 Albedo;
  #endif
#endif
    )";

    EmitParallaxFunction(GL, Out);

    Out << R"(
uvec2 GetTexHandleHelper(uint DrawID, uint Index)
{
	uvec4 Handles = GetTexHandles(DrawID, Index / 2u);
	return (Index % 2u == 0u) ? Handles.xy : Handles.zw;
}

float LinearizeDepth(float depth, float nearZ, float farZ)
{
    float z = depth * 2.0 - 1.0;        // back to NDC
    return (2.0 * nearZ * farZ) /
           (farZ + nearZ - z * (farZ - nearZ));
}

vec2 ViewToUV(vec3 viewPos) {
    vec4 clip = projMat * vec4(viewPos, 1.0);
    vec3 ndc  = clip.xyz / clip.w;
    vec2 uv   = ndc.xy * 0.5 + 0.5;
    
    uv.x = 1.0 - uv.x; // The "Inversion" Fix
    return uv;
}

// to continue to use this will need to tweak GetDepthTexel to handle AA (or rather lack of it from prepass), like a new GetPrepassDepthTexel
/*float ShadowForLight(vec3 fragPosVS, vec3 lightPosVS)
{
    const float bias = 5.0;
    const int   maxSteps = 48;

    // View-space normal
    vec3 normalVS = normalize(vNormal);

    // Push origin
    vec3 rayOrigin = fragPosVS + normalVS * bias;

    // Direction and distance
    vec3 L = lightPosVS - rayOrigin;
    float distToLight = length(L);
    vec3 lightDirVS = L / distToLight;

    // Depth-relative step size
    float baseStep = max(2.0, fragPosVS.z * 0.02);

    // Clamp number of steps based on distance
    int steps = clamp(int(distToLight / baseStep), 1, maxSteps);

    // Jitter to break up banding
    float jitter = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453);

    // Final step size
    float stepSize = distToLight / float(steps);

    // Thickness scales with step size
    float thickness = stepSize * 4.0;

    for (int i = 0; i < steps; i++)
    {
        float t = (float(i) + jitter) * stepSize;
        vec3 currentPos = rayOrigin + lightDirVS * t;

        vec2 uv = ViewToUV(currentPos);
        if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
            return 0.0;

        float depth = GetDepthTexel(GetTexHandleHelper(vDrawID, PrepassDepthIndex),
                                    TMUPrepassDepthMap, uv).r;
        float sceneZ = LinearizeDepth(depth, 0.5, 65336.0);

        float dz = currentPos.z - sceneZ;
        if (dz > 0.0 && dz < thickness)
            return 1.0;
    }

    return 0.0;
}*/
#if OPT_ShadowMaps
// Controls the blur width (softness) of the shadow edges on the wall surfaces
const float SHADOW_FILTER_RADIUS = 0.0035f; 

float GetSoftActorShadow(samplerCube shadowMap, vec3 sampleDir)
{
    vec3 N_Dir = normalize(sampleDir);

    // 1. Establish a fast tangent plane perpendicular to the lookup ray
    // This ensures our PCF offsets expand flatly along the cubemap face plane
    vec3 up = (abs(N_Dir.z) < 0.999) ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 tangent = normalize(cross(up, N_Dir));
    vec3 bitangent = cross(N_Dir, tangent);

    // 2. Define a standard 4-tap diamond offset pattern
    vec3 offset0 = ( tangent *  1.0 + bitangent *  1.0) * SHADOW_FILTER_RADIUS;
    vec3 offset1 = (-tangent *  1.0 + bitangent *  1.0) * SHADOW_FILTER_RADIUS;
    vec3 offset2 = ( tangent *  1.0 - bitangent *  1.0) * SHADOW_FILTER_RADIUS;
    vec3 offset3 = (-tangent *  1.0 - bitangent *  1.0) * SHADOW_FILTER_RADIUS;

    // 3. Accumulate classification values (alpha channel contains capsule splats and static meshes)
    float shadowSample = 0.0;
    shadowSample += texture(shadowMap, N_Dir + offset0).a;
    shadowSample += texture(shadowMap, N_Dir + offset1).a;
    shadowSample += texture(shadowMap, N_Dir + offset2).a;
    shadowSample += texture(shadowMap, N_Dir + offset3).a;

    // 4. Return the averaged shadow factor (0.0 = fully occluded, 1.0 = fully unshadowed)
    return 1.0 - (shadowSample * 0.25);
}
#endif
vec3 applyReinhard(vec3 color, float threshold) {
    // Normalize the range down to a 0-1 baseline
    vec3 normalized = color / vec3(threshold);
    
    // Apply the per-channel Reinhard curve: x / (x + 1)
    vec3 curved = normalized / (normalized + vec3(1.0));
    
    // Scale back up to target ceiling
    return curved * vec3(threshold);
}
    )";
    Out << R"(
void main(void)
{
  // normals debug
  /*if (true) {
    FragColor = vec4(vNormal * 0.5 + 0.5, 1.0);
    return;
  }*/

  uint DrawFlags = GetDrawFlags(vDrawID);
  mat3 InFrameCoords = mat3(FrameCoords[1].xyz, FrameCoords[2].xyz, FrameCoords[3].xyz); // TransformPointBy...
  mat3 InFrameUncoords = mat3(FrameUncoords[1].xyz, FrameUncoords[2].xyz, FrameUncoords[3].xyz);

  vec4 TotalColor = vec4(1.0);
  vec2 texCoords = vTexCoords;

#if OPT_BumpMaps || OPT_HWLighting || OPT_HeightMaps
#if OPT_PhongShading
  mat3 TBNMat;
  if ((DrawFlags & DF_PhongShading) == DF_PhongShading) {
    vec3 N = normalize(vN);
    vec3 T = normalize(vT);
    vec3 B = normalize(vB);

    // Optional safety: re-orthogonalize T and B
    T = normalize(T - N * dot(N, T));
    B = normalize(B - N * dot(N, B) - T * dot(T, B));
    // Build TBN (view space -> tangent space)
    TBNMat = transpose(mat3(T, B, N));
  }
  else {
    TBNMat = vTBNMat_geo;
  }
#else
  mat3 TBNMat = vTBNMat_geo;
#endif

#endif
#if OPT_HWLighting
  int NumLights = int(LightData4[0].y);
#endif

#if OPT_HeightMaps
  float baseAlpha;
  float applyConservative;
  if ((DrawFlags & DF_HeightMap) == DF_HeightMap) {
    float parallaxHeight = 1.0;
    // get new texture coordinates from Parallax Mapping
    vec3 TangentViewDir = normalize(vTBNMat_geo * -vCoords.xyz);
    texCoords = ParallaxMapping(vTexCoords, TangentViewDir, GetTexHandleHelper(vDrawID, HeightMapIndex), parallaxHeight);
  
    vec2 f = fract(vTexCoords);
    float borderX = min(f.x, 1.0 - f.x);
    float borderY = min(f.y, 1.0 - f.y);
    float borderZone = 0.10;
    float nearVerticalBorder   = step(borderX, borderZone);   // left/right
    float nearHorizontalBorder = step(borderY, borderZone);   // top/bottom

    // Parallaxed sample
    vec4 Color = GetTexel(GetTexHandleHelper(vDrawID, DiffuseTextureIndex), TMUDiffuse, texCoords.xy);
    // Build safe UV that protects only the needed axis
    vec2 safeUV = texCoords.xy;
    safeUV.x = mix(safeUV.x, vTexCoords.x, nearVerticalBorder);
    safeUV.y = mix(safeUV.y, vTexCoords.y, nearHorizontalBorder);
    // Conservative alpha only on protected axes 
    vec4 safeSample = GetTexel(GetTexHandleHelper(vDrawID, DiffuseTextureIndex), TMUDiffuse, safeUV);
    baseAlpha = safeSample.a;
    applyConservative = max(nearVerticalBorder, nearHorizontalBorder);
  }
#endif
    
  // (possibly) Parallaxed sample
  vec4 Color = GetTexel(GetTexHandleHelper(vDrawID, DiffuseTextureIndex), TMUDiffuse, texCoords.xy);
#if OPT_HeightMaps
  if ((DrawFlags & DF_HeightMap) == DF_HeightMap) {
    // Conservative alpha only near borders
    Color.a = mix(Color.a, baseAlpha, applyConservative);
  }
#endif

  // Apply diffuse factors
  Color *= GetDiffuseInfo(vDrawID).x;
  Color.a *= GetDiffuseInfo(vDrawID).z;

  TotalColor = ApplyPolyFlags(Color, DrawFlags);
#if OPT_IndirectIllumination
  Albedo = vec4(TotalColor.rgb, 1.0);
#endif
  vec4 LightColor = vec4(1.0);
  vec4 Occlusion = vec4(1.0);

#if OPT_HWLighting
  float MinLight = 0.05f;
  float LightAdd = 0.0f;
  LightColor = vec4(0.0);

  for (int i = 0; i < NumLights; i++)
  {
    float WorldLightRadius = LightData4[i].x;
    float LightRadius = LightData2[i].w;
    float RWorldLightRadius = WorldLightRadius * WorldLightRadius;

    vec3 InLightPos = ((LightPos[i].xyz - FrameCoords[0].xyz) * InFrameCoords); // Frame->Coords.
    float dist = distance(vCoords, InLightPos);

    if (dist < RWorldLightRadius)
    {
      // Light color
      vec4 CurrentLightColor = vec4(LightData1[i].x, LightData1[i].y, LightData1[i].z, 1.0);
      float b = WorldLightRadius / (RWorldLightRadius * MinLight);
      float attenuation = WorldLightRadius / (dist + b * dist * dist);
      LightColor += CurrentLightColor * attenuation;
    }
  }
  
  TotalColor *= LightColor;

#else
  if ((DrawFlags & DF_LightMap) == DF_LightMap) {
#if OPT_HDLightMap
    if ((DrawFlags & DF_HDLightMap) == DF_HDLightMap) {

      // Fetch facet metadata (atlas rect + light list)
      FacetData fd = FacetMetaArr[vFacetID];
      
      // Atlas-space clamp rectangle
      float MinU = fd.StaticUVMinMax.x;
      float MaxU = fd.StaticUVMinMax.y;
      float MinV = fd.StaticUVMinMax.z;
      float MaxV = fd.StaticUVMinMax.w;

      // Final atlas-space UV from vertex
      vec2 uv = vStaticLightmapCoords;

      // Texture size for blur kernel
      uvec2 handle = GetTexHandleHelper(vDrawID, StaticLightmapIndex);
      ivec2 size = GetTexSize(handle, TMUStaticLightmap);
      vec2 texelSize = 1.0 / vec2(size);

      vec4 accum = vec4(0.0);

      for (int x = -1; x <= 1; x++)
      for (int y = -1; y <= 1; y++)
      {
        vec2 offsetUV = uv + vec2(x, y) * texelSize;

        // Clamp to atlas rect (prevents sampling padded borders or neighbors)
        offsetUV.x = clamp(offsetUV.x, MinU, MaxU);
        offsetUV.y = clamp(offsetUV.y, MinV, MaxV);

        accum += GetTexel(handle, TMUStaticLightmap, offsetUV);
      }

      Occlusion = clamp(accum / 9.0, 0.0, 1.0);

      //Occlusion = GetTexel(handle, TMUStaticLightmap, StaticLightCoords);
      //Occlusion = textureLod(sampler2D(handle), StaticLightCoords, 1.0); // mip to blur
    }
#endif
    vec3 OldBakedLight = GetTexel(GetTexHandleHelper(vDrawID, LightMapIndex), TMULightMap, vLightMapCoords).rgb;
    #if OPT_GLES
    OldBakedLight = OldBakedLight.bgr;
    #endif
    OldBakedLight *= (LightMapIntensity * 255.0 / 127.0);
    LightColor = vec4(OldBakedLight, 1.0);
/*if (true) {
//FragColor = Occlusion;
FragColor = vec4(vStaticLightmapCoords.x,vStaticLightmapCoords.y,0.0,1.0);
return;
}*/
  }
#endif
    )";
    Out << R"(
#if OPT_DetailTextures
  if ((DrawFlags & DF_DetailTexture) == DF_DetailTexture)
  {
    float NearZ = vCoords.z / 512.0;
    float DetailScale = 1.0;
    float bNear;
    vec4 DetailTexColor;
    vec3 hsvDetailTex;

    for (int i = 0; i < OPT_DetailMax; ++i)
    {
      if (i > 0)
      {
        NearZ *= 4.223f;
        DetailScale *= 4.223f;
      }
      bNear = clamp(0.65 - NearZ, 0.0, 1.0);
      if (bNear > 0.0)
      {
        DetailTexColor = GetTexel(GetTexHandleHelper(vDrawID, DetailTextureIndex), TMUDetail, vDetailTexCoords * DetailScale);
        vec3 hsvDetailTex = rgb2hsv(DetailTexColor.rgb); // cool idea Han :)
        hsvDetailTex.b += (DetailTexColor.r - 0.1);
        hsvDetailTex = hsv2rgb(hsvDetailTex);
        DetailTexColor = vec4(hsvDetailTex, 0.0);
        DetailTexColor = mix(vec4(1.0, 1.0, 1.0, 1.0), DetailTexColor, bNear); //fading out.
        TotalColor.rgb *= DetailTexColor.rgb;
      }
    }
  }
#endif

#if OPT_MacroTextures
  if ((DrawFlags & DF_MacroTexture) == DF_MacroTexture)
  {    
    vec4 MacrotexColor = GetTexel(GetTexHandleHelper(vDrawID, MacroTextureIndex), TMUMacro, vMacroTexCoords);
    if ((DrawFlags & DF_Masked) == DF_Masked)
    {
      if (MacrotexColor.a < 0.5)
        discard;
      else MacrotexColor.rgb /= MacrotexColor.a;
    }
	else if ((DrawFlags & DF_AlphaBlended) == DF_AlphaBlended)
    {
      if (MacrotexColor.a < 0.01)
        discard;
    }

    vec3 hsvMacroTex = rgb2hsv(MacrotexColor.rgb);
    hsvMacroTex.b += (MacrotexColor.r - 0.1);
    hsvMacroTex = hsv2rgb(hsvMacroTex);
    MacrotexColor = vec4(hsvMacroTex, 1.0);
    TotalColor *= MacrotexColor;
  }
#endif

  vec2 screenUV = vec2(0,0);
  if ((DrawFlags & (DF_ReadDepth | DF_AmbientOcclusion)) != 0)
  {
    screenUV = gl_FragCoord.xy / vec2(DrawDrawComplexParams[vDrawID].SceneWidth, DrawDrawComplexParams[vDrawID].SceneHeight);
  }


  // BumpMap (Normal Map)
  vec3 totalSpec  = vec3(0.0);
  uint numSurfaceLights = 0;
#if OPT_BumpMaps
  {
    float MinLight = 0.05f;

    vec3 TextureNormal;
    if ((DrawFlags & DF_BumpMap) == DF_BumpMap)
      TextureNormal = normalize(GetTexel(GetTexHandles(vDrawID, 2).zw, Texture5, texCoords).rgb * 2.0 - 1.0); // has to be texCoords instead of vBumpTexCoords, otherwise alignment won't work on bumps.
    else
      TextureNormal = TBNMat * vNormal;

    float rough = DrawDrawComplexParams[vDrawID].Roughness;

    //vec3 TotalBumpColor = vec3(0.0);
    vec3 totalStaticLight = vec3(0.0);
    vec3 totalSubtractedLight = vec3(0.0);
    vec3 totalDynamicLight = vec3(0.0);
    //int contributingLights = 0;

    uvec4 meta = FacetMetaArr[vFacetID].LightMeta;
    uint start = meta.x;
    uint numStaticLights = meta.y;
    uint numDynamicLights = meta.z;
    numSurfaceLights = clamp(numStaticLights + numDynamicLights, uint(0), uint(MAX_SURFACE_LIGHTS));

    for (uint li = 0u; li < numSurfaceLights; ++li)
    {
      uint i = FacetIndicesArr[start + li];

      float WorldLightRadius  = LightData4[i].x;

      if (WorldLightRadius == 0.0)
        continue; // skip lights with zero radius, which are used for non-lighting purposes (e.g. zone restriction) 

      vec3 InLightPos = ((LightPos[i].xyz - FrameCoords[0].xyz) * InFrameCoords);
      float dist = distance(vCoords, InLightPos);

      // Distance early out test
      if (dist > WorldLightRadius)
        continue;

      vec3 originVS = vec3(vCoords.x, vCoords.y, vCoords.z);
      vec3 lightPosVS = vec3(InLightPos.x, InLightPos.y, InLightPos.z);
      
      // screenspace shadows (boo)
      //if (ShadowForLight(vCoords, lightPosVS) != 0)
      //  continue;


      float shadowFactor = 1.0f;
 #if OPT_ShadowMaps   
      // ====================================================================
      // --- UNPACK BINDLESS HERO SHADOW MAP HANDLE (Z and W of LightData5) ---
      // ====================================================================
      // unpack the type-punned float bits back into raw 32-bit unsigned ints
      uvec2 handleBits = uvec2(
        floatBitsToUint(LightData5[i].z), // Lower 32 bits
        floatBitsToUint(LightData5[i].w)  // Upper 32 bits
      );

      // If the 64-bit handle is non-zero, this light has an active, resident shadow map!
      if (handleBits.x != 0u || handleBits.y != 0u)
      {
        samplerCube shadowMap = samplerCube(handleBits);

        // Reconstruct true world coordinates (Your stable, locked-down math!)
        vec3 X = FrameCoords[1].xyz; vec3 Y = FrameCoords[2].xyz; vec3 Z = FrameCoords[3].xyz;
        mat3 ViewToWorld = mat3(X, Y, Z);
        vec3 pixelWorldPos = FrameCoords[0].xyz + ViewToWorld * vCoords;

        //Compute absolute world distance metrics
        vec3 rawLookupWS = LightPos[i].xyz - pixelWorldPos;
        float currentPixelDist = length(rawLookupWS); 
        vec3 shadowLookupDirWS = vec3(rawLookupWS.x, -rawLookupWS.y, rawLookupWS.z);

        // do a single texture tap here to see if the pixel is behind a wall.
        vec4 shadowData = texture(shadowMap, shadowLookupDirWS);
        float bspDepthNormalized = shadowData.r;
        float staticBspWorldDist = bspDepthNormalized * WorldLightRadius;
        float depthBias = 125.0f; 

        // If the current pixel sits DEEPER in the level than the first solid wall,
        // it is inside a pre-occluded back room. We bypass the shadow completely!
        if (currentPixelDist <= staticBspWorldDist + depthBias)// && shadowData.a > 0.5) // doesn't seem to speed it up, and looks worse
        {
            // The pixel is in front of the wall! Run a 4-tap PCF kernel.
            shadowFactor = GetSoftActorShadow(shadowMap, shadowLookupDirWS);
        }
      }
#endif    

      //float NormalLightRadius  = LightData5[i].x;
      // attenuation that fades out by radius.  worldLightRadius looks better here
      //float x = clamp(dist / WorldLightRadius, 0.0, 1.0);
      //float attenuation = (1.0 - x) / (1.0 + 4.0 * x*x);

      // literally linear
      float x = clamp(dist / WorldLightRadius, 0.0, 1.0);
      float attenuation = 1.0 - x;

      // mix in shadowmap result, if any
      attenuation;

      // Light color + brightness
      vec3 rawColor = clamp(vec3(LightData1[i].x, LightData1[i].y, LightData1[i].z), 0.0, 1.0);
      float lum = dot(rawColor, vec3(0.299, 0.587, 0.114));
    
      float brightness = LightData5[i].z / 255.0; // this could be something in Unreal.  in UT it is 0.  (unless I pass Actor->LightBrightness like Unreal path does.  Why not?)
      float brightnessFactor = max(lum, brightness); // so in UT this == lum, but in Unreal it allows the light's brightness to boost the diffuse and specular even if the color is dark

      // Choose normal / coordinate space based on whether we have a normal map
      vec3 N;
      vec3 L;
      vec3 V;

      // Tangent-space lighting using normal map
      vec3 TexN = TextureNormal; // already normalized
      N = TexN;
      vec3 TangentLightDir = normalize(TBNMat * (InLightPos - vCoords));
      L = normalize(TangentLightDir);
      vec3 TangentViewDir = normalize(TBNMat * -vCoords.xyz);
      V = TangentViewDir;

      float diff;
      if ((DrawFlags & DF_TwoSided) == DF_TwoSided)
        diff = abs(dot(N, L));
      else
        diff = max(dot(N, L), 0.0);
   
      if (li < numStaticLights) {
        totalStaticLight += rawColor * diff * attenuation;
        totalSubtractedLight += (1.0 - shadowFactor) * rawColor * diff * attenuation;
      }
      else {
        totalDynamicLight += rawColor * diff * attenuation;
      }

      // --- SPECULAR
      float shininess    = mix(4.0, 64.0, 1.0 - rough);
      float specStrength = mix(0.1, 1.0, 1.0 - rough);

      vec3 H = normalize(L + V);

      float specDot;
      if ((DrawFlags & DF_BumpMap) == DF_BumpMap)
        specDot = abs(dot(N, H));
      else
        specDot = max(dot(N, H), 0.0);

      float spec = pow(specDot, shininess)
                   * specStrength
                   * brightnessFactor
                   * attenuation
                   * shadowFactor;

      vec3 specular = spec * rawColor;   // colored specular, matches UT99 lights
      totalSpec += specular;

      //contributingLights++;
    }
    // needs to be numSurfaceLights here not contributingLights.  Trying to weed out facets with no lights (that shouldn't be part of the per-pixel lighting path)
    // not *fragments* where there might legitimately be no contributing lights due to attenuation
    if (numSurfaceLights > 0) {
      float hdLightmapIntensity = 2; // vanilla boosts 2 X LightMapIntensity.  We do a little less or it ends up too bright
      totalStaticLight *= (LightMapIntensity * hdLightmapIntensity);
      totalSubtractedLight *= (LightMapIntensity * hdLightmapIntensity);
      totalDynamicLight *= (LightMapIntensity * hdLightmapIntensity);      
      totalSpec *= (LightMapIntensity * 1.5f); // give specular less of a boost

      float specThreshold = 1.0; // for specular, we want to allow it to be as bright as the light color, but not brighter
      totalSpec = applyReinhard(totalSpec, specThreshold);

      float threshold = 1.34; // must match the GPU_Threshold in the CPU occlusion calculator
#if OPT_HDLightMap
      // preserve color from both HD and vanilla and use the darker shadow term
      float HDShadow  = max(Occlusion.r, max(Occlusion.g, Occlusion.b));
      float VanShadow = max(LightColor.r, max(LightColor.g, LightColor.b));
      vec3 HDHue     = (HDShadow  > 0) ? (Occlusion.rgb / HDShadow)  : vec3(1.0);
      vec3 VanHue    = (VanShadow > 0) ? (LightColor.rgb / VanShadow) : vec3(1.0);
      // Final hue = HD hue * vanilla hue
      vec3 finalHue = HDHue * VanHue;
      // Choose shadow term
      float finalShadow = (VanShadow < HDShadow) ? VanShadow : HDShadow;
      // Final
      vec3 blendedLM = finalHue * finalShadow;
#else
      vec3 blendedLM = LightColor.rgb;
#endif
      totalSubtractedLight *= LightColor.rgb;
#if OPT_AmbientOcclusion
      if ((DrawFlags & DF_AmbientOcclusion) == DF_AmbientOcclusion) {
        // Sample SSAO (0 = dark, 1 = no occlusion)
        float AO = GetTexel(GetTexHandleHelper(vDrawID, PostProcessIndex), TMUPostProcessMap, screenUV).r;
        blendedLM *= AO * AO * AO * AO * AO;// * AO;
      }
#endif
      float lmIntensity = dot(blendedLM, vec3(0.299, 0.587, 0.114));
      totalSpec *= lmIntensity;
      totalStaticLight = totalStaticLight * blendedLM;
      totalStaticLight -= totalSubtractedLight;
      totalStaticLight = clamp(totalStaticLight, vec3(0.0), totalStaticLight);
      totalStaticLight = applyReinhard(totalStaticLight, threshold);
      vec3 totalLight = totalStaticLight + totalDynamicLight;

      LightColor.rgb = totalLight;
      //LightColor.rgb = applyReinhard(LightColor.rgb, 1.34);
      
      // lighting debug
      /*if (true) {
        FragColor = vec4(clamp(totalLight, 0.0, 1.0), 1.0);
        //FragColor = vec4(LightColor.rgb, 1.0);
        return;
      }*/
    } // end if there are lights
  }
#endif

  vec4 FogColor = vec4(0.0);

  if ((DrawFlags & DF_FogMap) == DF_FogMap)
    FogColor = GetTexel(GetTexHandleHelper(vDrawID, FogMapIndex), TMUFogMap, vFogMapCoords) * 2.0;

#if OPT_EnvironmentMaps
  if ((DrawFlags & DF_EnvironmentMap) == DF_EnvironmentMap)
  {    
    vec4 EnvironmentColor = GetTexel(GetTexHandleHelper(vDrawID, EnvironmentMapIndex), TMUEnvironmentMap, vEnvironmentTexCoords);
    if ((DrawFlags & DF_Masked) == DF_Masked)
    {
      if (EnvironmentColor.a < 0.5)
        discard;
      else EnvironmentColor.rgb /= EnvironmentColor.a;
    }
    else if ((DrawFlags & DF_AlphaBlended) == DF_AlphaBlended)
    {
      if (EnvironmentColor.a < 0.01)
        discard;
    }

    TotalColor *= vec4(EnvironmentColor.rgb, 1.0);
  }
#endif

  if ((DrawFlags & DF_Modulated) != DF_Modulated)
  {
    //TotalColor.rgb = applyReinhard(TotalColor.rgb * LightColor.rgb + totalSpec.rgb, 1.34);
    TotalColor = clamp(TotalColor * LightColor + vec4(totalSpec.rgb, 1.0), 0.0, 1.0);
  }

  TotalColor += FogColor;

#if OPT_DistanceFog
  // Add DistanceFog, needs to be added after Light has been applied. 
  if (DistanceFogMode >= 0)
  {
    vec4 MixColor;
    if ((DrawFlags & DF_Modulated) == DF_Modulated)
      MixColor = vec4(0.5, 0.5, 0.5, 0.0);
    else if ((DrawFlags & DF_Translucent) == DF_Translucent && (DrawFlags & DF_EnvironmentMap) != DF_EnvironmentMap)
      MixColor = vec4(0.0, 0.0, 0.0, 0.0);
    else
      MixColor = DistanceFogColor;

    float FogCoord = abs(vEyeSpacePos.z / vEyeSpacePos.w);
    TotalColor = mix(TotalColor, MixColor, getFogFactor(FogCoord));
  }
#endif
	
#if OPT_Editor
  vec4 vDrawColor = GetDrawColor(vDrawID);
  if (RendMap == REN_Zones || RendMap == REN_PolyCuts || RendMap == REN_Polys || RendMap == REN_WalkableSurfs)
  {
    TotalColor += 0.5;
    TotalColor *= vDrawColor;
  }
# if 0
  else if (RendMap == REN_Normals) //Thank you han!
  {
    // Dot.
    float T = 0.5 * dot(normalize(vCoords), GetZAxis(vDrawID).xyz);

    // Selected.
    if ((DrawFlags & DF_Selected) == DF_Selected)
      TotalColor = vec4(0.0, 0.0, abs(T), 1.0);
    // Normal.
    else    
      TotalColor = vec4(max(0.0, T), max(0.0, -T), 0.0, 1.0);
  }
# endif
  else if (RendMap == REN_PlainTex)
    TotalColor = Color;

  if ((DrawFlags & DF_Selected) == DF_Selected && RendMap != REN_Normals)
  {
    TotalColor.r = (TotalColor.r * 0.75);
    TotalColor.g = (TotalColor.g * 0.75);
    TotalColor.b = (TotalColor.b * 0.75) + 0.1;
    TotalColor = clamp(TotalColor, 0.0, 1.0);
    if (TotalColor.a < 0.5)
      TotalColor.a = 0.51;
  }

  // HitSelection, Zoneview etc.
  if (bool(HitTesting))
    TotalColor = vDrawColor; // Use ONLY DrawColor.
  else if ((DrawFlags & DF_Modulated) != DF_Modulated)
    TotalColor = GammaCorrect(Gamma, TotalColor);
#endif // OPT_Editor

  if ((DrawFlags & DF_ReadDepth) == DF_ReadDepth)
  {
    // depth sampling
    float sceneZ = GetDepthTexel(GetTexHandleHelper(vDrawID, SceneDepthIndex), TMUDepthMap, screenUV).r;
    float spriteZ = gl_FragCoord.z;

    // linearization
    float n = 0.5;
    float f = 65336.0;
    float sceneL  = (2.0 * n) / (f + n - sceneZ  * (f - n));
    float spriteL = (2.0 * n) / (f + n - spriteZ * (f - n));

    float diff = sceneL - spriteL;
    float fadeWidth = .002;

    // proximity fade
    float proximityFade = smoothstep(0.0, fadeWidth, diff);
    /*
    // more "correct" tunable version?
    float sceneL = n / (f - sceneZ * (f - n));
    float spriteL = n / (f - spriteZ * (f - n));
    float diff = sceneL - spriteL;

    // Linear Scaling with a "Floor"
    // 0.001 is "Minimum Thickness" (about 65 world units)
    // + 0.01 * spriteL makes it get even THICKER as it moves away
    float fadeWidth = 0.001 + (0.01 * spriteL); 

    float proximityFade = smoothstep(0.0, fadeWidth, diff);*/

    // alpha fade
    TotalColor.rgb *= proximityFade;
  }    

#if OPT_SimulateMultiPass
  FragColor = TotalColor;
  FragColor1 = ((vec4(1.0) - TotalColor) * LightColor);
#else
  FragColor = TotalColor;
#endif

#if !OPT_Editor
  if ((DrawFlags & DF_Modulated) != DF_Modulated)
    FragColor = GammaCorrect(Gamma, FragColor);
#endif
}
)";

	/*
	//
	// EnviroMap.
	//
	// Simple GLSL implementation of the C++ code. Should be obsoleted by some fancy
	// per pixel sphere mapping implementation. But for now, just use this approach
	// as PF_Environment handling is the last missing peace on obsoleting RenderSubsurface.
	//
	vec2 EnviroMap( vec3 Point, vec3 Normal )
	{
	vec3 R = reflect(normalize(Point),Normal);
	return vec2(0.5*dot(R,Uncoords_XAxis)+0.5,0.5*dot(R,Uncoords_YAxis)+0.5);
	}
	*/
}

// unused. Maybe later.
#if 0
Out << R"(
float parallaxSoftShadowMultiplier(in vec3 L, in vec2 initialTexCoord, in float initialHeight)
{
  float shadowMultiplier = 1;
)";

constexpr FLOAT minLayers = 15;
constexpr FLOAT maxLayers = 30;

// calculate lighting only for surface oriented to the light source
Out << R"(
if(dot(vec3(0, 0, 1), L) > 0)
{
  // calculate initial parameters
  "float numSamplesUnderSurface = 0;
  "shadowMultiplier = 0;"
)";

Out << "  float numLayers = mix(" << maxLayers << ", " << minLayers << ", abs(dot(vec3(0, 0, 1), L)));" END_LINE;
Out << R"(
  float layerHeight = initialHeight / numLayers;
  vec2 texStep = vParallaxScale * L.xy / L.z / numLayers;

  // current parameters
  float currentLayerHeight = initialHeight - layerHeight;
  vec2 currentTexCoords = initialTexCoord + texStep;
  float heightFromTexture = GetTexel(GetTexHandleHelper(vDrawID, HeightMapIndex), TMUHeightMap, currentTexCoords).r;

  int stepIndex = 1;

  // while point is below depth 0.0 )
  while(currentLayerHeight > 0)
  {
    if(heightFromTexture < currentLayerHeight) // if point is under the surface
    {
      // calculate partial shadowing factor
      numSamplesUnderSurface += 1;
      float newShadowMultiplier = (currentLayerHeight - heightFromTexture) * (1.0 - stepIndex / numLayers);
      shadowMultiplier = max(shadowMultiplier, newShadowMultiplier);
    }

    // offset to the next layer
    stepIndex += 1;
    currentLayerHeight -= layerHeight;
    currentTexCoords += texStep;
    heightFromTexture = GetTexel(GetTexHandleHelper(vDrawID, HeightMapIndex), TMUHeightMap, currentTexCoords).r;
  }

  // Shadowing factor should be 1 if there were no points under the surface
  if(numSamplesUnderSurface < 1)
    shadowMultiplier = 1;
  else
    shadowMultiplier = 1.0 - shadowMultiplier;

  return shadowMultiplier;
}
)";

#endif // 0
