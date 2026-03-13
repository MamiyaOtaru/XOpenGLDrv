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
    {"uvec4", "TexHandles", 4},
	{"uint", "DrawFlags", 0},
    {"float", "Roughness", 0},
    {"uint", "Dummy0", 0},
    {"uint", "Dummy1", 0},
	{ nullptr, nullptr, 0}
};

// Vertexshader for DrawComplexSurface, single pass.
void UXOpenGLRenderDevice::DrawComplexProgram::BuildVertexShader(GLuint ShaderType, UXOpenGLRenderDevice * GL, FShaderWriterX & Out)
{
    Out << R"(
layout(location = 0) in vec3 Coords; // == gl_Vertex
layout(location = 1) in uint DrawID; // emulated gl_DrawID
layout(location = 2) in vec4 Normal;
layout(location = 3) in uint FacetID;

out vec3 vCoords;
out vec2 vTexCoords;
out vec2 vLightMapCoords;
out vec2 vFogMapCoords;
flat out uint vFacetID;

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
flat out mat3 vTBNMat;
out vec3 vTangentViewPos;
out vec3 vTangentFragPos;
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

#if OPT_BumpMaps || OPT_HWLighting || OPT_HeightMaps

  vec3 CameraPosWS_Normalized = normalize(FrameCoords[0].xyz); // despite pretty perfect results (so far) this still seems somewhat wrong to me.
  vec3 T = normalize(vec3(MapCoordsXAxis.x, MapCoordsXAxis.y, MapCoordsXAxis.z));
  vec3 B = normalize(vec3(MapCoordsYAxis.x, MapCoordsYAxis.y, MapCoordsYAxis.z));
  vec3 N = normalize(vec3(MapCoordsZAxis.x, MapCoordsZAxis.y, MapCoordsZAxis.z)); //SurfNormals.

  // TBN must have right handed coord system.
  //if (dot(cross(N, T), B) < 0.0)
  //   T = T * -1.0;
  vTBNMat = transpose(mat3(T, B, N));

  // what... seems to work accidentally as a normalized position (what even is that) is only 1 unit away from 0,0,0, the actual viewspace position of the camera
  // would have been better off not bothering and just calculating vTangentViewPos as vTBNMat * vec3(0.0,0.0,0.0);
  vTangentViewPos = vTBNMat * CameraPosWS_Normalized.xyz; 
  vTangentFragPos = vTBNMat * Coords.xyz;
/*
  // Build world-space TBN
  vec3 Nw = normalize(MapCoordsZAxis.xyz);
  vec3 Tw = normalize(MapCoordsXAxis.xyz);

  // Re-orthogonalize tangent to normal
  Tw = normalize(Tw - Nw * dot(Nw, Tw));

  // Bitangent
  vec3 Bw = normalize(cross(Nw, Tw));

  // Handedness correction
  if (dot(cross(Tw, Bw), Nw) < 0.0)
  {
    Tw = -Tw;
    Bw = cross(Nw, Tw);
  }

  // v axis orientation correction
  vec3 MapV = normalize(GetYAxis(DrawID).xyz);
  if (dot(Bw, MapV) < 0.0)
  {
    Bw = -Bw;
  }

  // Re-orthogonalize after V-axis flip
  Bw = normalize(Bw - Nw * dot(Nw, Bw));
  Tw = normalize(cross(Bw, Nw));  // rebuild T from B×N to keep basis tight

  // Final world-space TBN -> tangent-space transform
  vTBNMat = transpose(mat3(Tw, Bw, Nw));

  // Compute view-space positions
  // Coords is already view-space
  vec3 FragPosVS = Coords.xyz;

  // Camera position in view space is always (0,0,0)
  vec3 ViewPosVS = vec3(0.0);

  // Transform both into tangent space
  vTangentFragPos = vTBNMat * FragPosVS;
  vTangentViewPos = vTBNMat * ViewPosVS;*/
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
            constexpr FLOAT minLayers = 10.f;
            constexpr FLOAT maxLayers = 15.f;
            constexpr INT numSearches = 5;
            Out << "  float numLayers = mix(" << maxLayers << ", " << minLayers << ", abs(dot(vec3(0, 0, 1), viewDir)));" END_LINE;
            Out << R"(
  float layerHeight = 1.0 / numLayers; // height of each layer
  float currentLayerHeight = 0.0; // depth of current layer
  float vz = max(abs(viewDir.z), 0.02);
  vec2 dtex = vParallaxScale * viewDir.xy / vz / numLayers; // shift of texture coordinates for each iteration
  vec2 currentTexCoords = ptexCoords; // current texture coordinates

  float heightFromTexture = 1.0 - GetTexel(TexHandle, TMUHeightMap, currentTexCoords).r; // depth from heightmap

  // while point is above surface
  while (heightFromTexture > currentLayerHeight)
  {
    currentLayerHeight += layerHeight; // go to the next layer
    currentTexCoords -= dtex; // shift texture coordinates along V
    heightFromTexture = 1.0 - GetTexel(TexHandle, TMUHeightMap, currentTexCoords).r; // new depth from heightmap
  }

  ///////////////////////////////////////////////////////////
  // Start of Relief Parallax Mapping
  // decrease shift and height of layer by half
  vec2 deltaTexCoord = dtex / 2.0;
  float deltaHeight = layerHeight / 2.0;

  // return to the mid point of previous layer
  currentTexCoords += deltaTexCoord;
  currentLayerHeight -= deltaHeight;

  // binary search to increase precision of Steep Paralax Mapping
  for (int i = 0; i < )" << numSearches << R"(; i++)
  {
    // decrease shift and height of layer by half
    deltaTexCoord /= 2.0;
    deltaHeight /= 2.0;
 
    // new depth from heightmap
    heightFromTexture = 1.0 - GetTexel(TexHandle, TMUHeightMap, currentTexCoords).r;

    // shift along or agains vector V
    if (heightFromTexture > currentLayerHeight) // below the surface
    {
      currentTexCoords -= deltaTexCoord;
      currentLayerHeight += deltaHeight;
    }
    else // above the surface
    {
      currentTexCoords += deltaTexCoord;
      currentLayerHeight -= deltaHeight;
    }
  }

  // return results
  parallaxHeight = currentLayerHeight;
  if (any_nonfinite(currentTexCoords))
        return ptexCoords;
  return currentTexCoords;
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
flat in mat3 vTBNMat;
in vec3 vTangentViewPos;
in vec3 vTangentFragPos;
#endif

#if OPT_BumpMaps || OPT_DistanceFog
in vec4 vEyeSpacePos;
#endif

#if OPT_GLES
layout(location = 0) out vec4 FragColor;
# if OPT_SimulateMultiPass
layout(location = 1) out vec4 FragColor1;
# endif
#else
# if OPT_SimulateMultiPass
layout(location = 0, index = 1) out vec4 FragColor1;
# endif
layout(location = 0, index = 0) out vec4 FragColor;
#endif
)";

    EmitParallaxFunction(GL, Out);

    Out << R"(
uvec2 GetTexHandleHelper(uint DrawID, uint Index)
{
	uvec4 Handles = GetTexHandles(DrawID, Index / 2u);
	return (Index % 2u == 0u) ? Handles.xy : Handles.zw;
}

void main(void)
{
  uint DrawFlags = GetDrawFlags(vDrawID);
  mat3 InFrameCoords = mat3(FrameCoords[1].xyz, FrameCoords[2].xyz, FrameCoords[3].xyz); // TransformPointBy...
  mat3 InFrameUncoords = mat3(FrameUncoords[1].xyz, FrameUncoords[2].xyz, FrameUncoords[3].xyz);

  vec4 TotalColor = vec4(1.0);
  vec2 texCoords = vTexCoords;

#if OPT_BumpMaps || OPT_HWLighting || OPT_HeightMaps
  //vec3 TangentViewDir = normalize(vTangentViewPos - vTangentFragPos);
  vec3 TangentViewDir = normalize(-vTBNMat *  vCoords.xyz);
#endif
#if OPT_HWLighting
  int NumLights = int(LightData4[0].y);
#endif

#if OPT_HeightMaps
  if ((DrawFlags & DF_HeightMap) == DF_HeightMap)
  {
    float parallaxHeight = 1.0;
    // get new texture coordinates from Parallax Mapping
    texCoords = ParallaxMapping(vTexCoords, TangentViewDir, GetTexHandleHelper(vDrawID, HeightMapIndex), parallaxHeight);
    //if(texCoords.x > 1.0 || texCoords.y > 1.0 || texCoords.x < 0.0 || texCoords.y < 0.0)
    //discard; // texCoords = vTexCoords;
  }
#endif

  vec4 Color = GetTexel(GetTexHandleHelper(vDrawID, DiffuseTextureIndex), TMUDiffuse, texCoords.xy);
  Color *= GetDiffuseInfo(vDrawID).x; // Diffuse factor.
  Color.a *= GetDiffuseInfo(vDrawID).z; // Alpha.
	
  TotalColor = ApplyPolyFlags(Color, DrawFlags);
  vec4 LightColor = vec4(1.0);

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
  if ((DrawFlags & DF_LightMap) == DF_LightMap)
  {
    LightColor = GetTexel(GetTexHandleHelper(vDrawID, LightMapIndex), TMULightMap, vLightMapCoords);
    // Fetch lightmap texel. Data in LightMap is in 0..127/255 range, which needs to be scaled to 0..2 range.
    LightColor.rgb =
# if OPT_GLES
  	  LightColor.bgr
# else
	  LightColor.rgb
# endif
	  * (LightMapIntensity * 255.0 / 127.0);
    LightColor.a = 1.0;
  }
#endif

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
      TextureNormal = vec3(0.0, 0.0, 1.0);

    float rough = DrawDrawComplexParams[vDrawID].Roughness;

    //vec3 TotalBumpColor = vec3(0.0);
    vec3 totalLight = vec3(0.0);
    //int contributingLights = 0;

    uvec2 meta = FacetMetaArr[vFacetID];
    uint start = meta.x;
    numSurfaceLights = clamp(meta.y, uint(0), uint(MAX_SURFACE_LIGHTS));
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

      //float NormalLightRadius  = LightData5[i].x;
      // attenuation that fades out by radius.  worldLightRadius looks better here
      float x = clamp(dist / WorldLightRadius, 0.0, 1.0);
      float attenuation = (1.0 - x) / (1.0 + 4.0 * x*x);

      // HWLighting style attenuation
      //float RWorldLightRadius = WorldLightRadius * WorldLightRadius;
      //float b = WorldLightRadius / (RWorldLightRadius * MinLight);
      //float attenuation = WorldLightRadius / (dist + b * dist * dist);

      // Light color + brightness
      vec3 rawColor = clamp(vec3(LightData1[i].x, LightData1[i].y, LightData1[i].z), 0.0, 1.0);
      float lum = dot(rawColor, vec3(0.299, 0.587, 0.114));
      //vec3 desatColor = mix(rawColor, vec3(lum), 0.15); // not desaturating looks better in most cases, and it also makes the specular term look better without tweaking the exponent and intensity.

      float brightness = LightData5[i].z / 255.0;
      float brightnessFactor = max(lum, brightness);
        
      // Tangent-space direction
      vec3 TangentLightDir = normalize(vTBNMat * (InLightPos - vCoords));

      // Lambert for ranking
      //float lambert = max(dot(TangentLightDir, TextureNormal), 0.0);

      // Ranking strength
      //float strength = attenuation * brightnessFactor * lambert;

      vec3 N = TextureNormal;
      vec3 L = normalize(TangentLightDir);
      vec3 V = TangentViewDir;

      float diff = max(dot(N, L), 0.0);

      totalLight += rawColor * diff * attenuation;

      // --- SPECULAR
      float shininess    = mix(4.0, 64.0, 1.0 - rough);
      float specStrength = mix(0.1, 1.0, 1.0 - rough);

      vec3 H = normalize(L + V);

      float spec = pow(max(dot(N, H), 0.0), shininess)
                     * specStrength
                     * brightnessFactor
                     * attenuation;

      vec3 specular = spec * rawColor;   // colored specular, matches UT99 lights
      totalSpec += specular;

      //contributingLights++;
    }
    // needs to be numSurfaceLights here not contributingLights.  Trying to weed out facets with no lights (that shouldn't be part of the per-pixel lighting path)
    // not *fragments* where there might legitimately be no contributing lights due to attenuation
    if (numSurfaceLights > 0) {
      LightColor.rgb *= clamp(totalLight, 0.0, 1.0);
    }
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
    TotalColor = clamp(TotalColor * LightColor + vec4(totalSpec.rgb, 1.0), 0.0, 1.0);

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
