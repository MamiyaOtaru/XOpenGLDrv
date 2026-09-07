#version 330 core

out vec4 FragColor;
in vec2 TexCoords;

// G-buffer inputs
uniform sampler2D gDepth;
uniform sampler2D gNormal;

// SSAO kernel + params
uniform vec3  samples[64];
uniform int   kernelSize;
uniform float nearPlane;
uniform float farPlane;

// Noise (optional)
uniform sampler2D texNoise;
uniform vec2      noiseScale;

// Must match engine's FrameState exactly
layout(std140) uniform FrameState {
    mat4 projMat;
    mat4 viewMat;
    mat4 modelMat;
    mat4 modelviewMat;
    mat4 modelviewprojMat;
    mat4 lightSpaceMat;
    mat4 FrameCoords;
    mat4 FrameUncoords;
    float Gamma;
    float LightMapIntensity;
    float LightColorIntensity;
};

float when_gt(float x, float y) { return max(sign(x - y), 0.0); }
float when_lt(float x, float y) { return max(sign(y - x), 0.0); }

#define LOG_MAX_OFFSET 3
#define MAX_MIP_LEVEL  10

// ------------------------------------------------------------
// Depth ? linear view-space Z
// ------------------------------------------------------------
float LinearizeDepth(float depth)
{
    float z_ndc  = depth * 2.0 - 1.0;
    float z_view = (2.0 * nearPlane * farPlane) /
                   (farPlane + nearPlane - z_ndc * (farPlane - nearPlane));
    return z_view; // +Z forward
}

// ------------------------------------------------------------
// Reconstruct view-space position from depth + UV
// ------------------------------------------------------------
vec3 ReconstructViewPos(vec2 uv, float depth, float fx, float fy)
{
    float z = LinearizeDepth(depth);

    float x = (uv.x * 2.0 - 1.0) * z / fx;
    float y = (uv.y * 2.0 - 1.0) * z / fy;

    return vec3(-x, -y, z); // view space, +Z forward
}

// ------------------------------------------------------------
// Main SSAO
// ------------------------------------------------------------
void main()
{
    float depth = texture(gDepth, TexCoords).r;

    // Sky / no geometry
    if (depth == 0.0) {
        FragColor = vec4(1,0,0,1);
        return;
    }

    // Extract projection scale from projMat (same one used for depth)
    float fx = projMat[0][0];
    float fy = projMat[1][1];

    // View-space position
    vec3 fragPos = ReconstructViewPos(TexCoords, depth, fx, fy);
      
    // View‑space normal from G‑buffer
    vec3 normal = normalize(texture(gNormal, TexCoords).rgb);
    // Random vector from noise texture (view‑space)
    vec3 randomVec = normalize(texture(texNoise, TexCoords * noiseScale).xyz);
    // Project randomVec into tangent plane to get a stable tangent
    vec3 tangent = randomVec - normal * dot(randomVec, normal);
    float tLen = dot(tangent, tangent);
    if (tLen < 1e-4) {
        // Fallback if randomVec was nearly parallel to normal
        tangent = normalize(cross(normal, vec3(1.0, 0.0, 0.0)));
    } else {
        tangent *= inversesqrt(tLen);   // normalize without full sqrt
    }
    // Bitangent is orthogonal by construction
    vec3 bitangent = cross(normal, tangent);
    // Final TBN (per‑fragment, already randomized)
    mat3 TBN = mat3(tangent, bitangent, normal);

    float radius    = 30.0;
    float bias      = 10; // 3
    float occlusion = 0.0;
    
    ivec2 texSize = textureSize(gDepth, 0).xy;
		float projScale = texSize.y / (2.0 * tan(1 * 0.5)); // vertical fov is about one rad. 
		// Projected radius in pixels
    float rad = projScale * radius / fragPos.z; // fragPos.z is positive distance
    // Keep rad in a sane range to avoid crazy mip levels on very near/far geometry
    rad = clamp(rad, 1.0, 512.0);
    int mipLevel = clamp(int(floor(log2(rad))) - LOG_MAX_OFFSET, 0, MAX_MIP_LEVEL);

    int maxKernel = kernelSize;
    int minKernel = 8;
    int ks = int(mix(minKernel, maxKernel, clamp(rad / 64.0, 0.0, 1.0)));
    float validSamples = 0.0001;
    
    for (int i = 0; i < ks; i++)
    {
        vec3 baseSample = samples[i];
        baseSample.z *= -1;
        vec3 sampleVec = TBN * normalize(baseSample);
        vec3 samplePos = fragPos + sampleVec * radius;

        // Project samplePos using the SAME projection as geometry pass
        vec4 clip = projMat * vec4(samplePos, 1.0);
        vec3 ndc  = clip.xyz / clip.w;
        vec2 uv   = ndc.xy * 0.5 + 0.5;

        uv = clamp(uv, 0.0, 1.0);
        //if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {
        //  continue; // skip this sample
        //}
        validSamples++;
        
        ivec2 iOffset      = ivec2(uv.x * texSize.x, uv.y * texSize.y);
        ivec2 mippedOffset = clamp(iOffset >> mipLevel,
                                   ivec2(0),
                                   textureSize(gDepth, mipLevel) - ivec2(1));

        // sampleDepth is linear view-space Z, same convention as fragPos.z, samplePos.z
        float sampleDepth = LinearizeDepth(
            texelFetch(gDepth, mippedOffset, mipLevel).r
        );
        //float sampleDepth = LinearizeDepth(texture(gDepth, uv).r);

        //float dist  = abs(samplePos.z - sampleDepth);
        //float range = when_lt(dist, radius);
        float range = smoothstep(0.0, 1.0, radius / abs(samplePos.z - sampleDepth));

        // +Z forward: smaller z = closer to camera
        float hit = max(sign((samplePos.z - bias) - sampleDepth), 0.0);
        occlusion += hit * range;
        //if (occlusion > 0.8/(i+1) && i > 4) { // not much of an improvement usually.  might be in particular places
        //  ks = i+1;
        //  break;
        //}
    }
    occlusion /= validSamples;
    float ao = 1.0 - occlusion;

    ao = clamp(ao, 0.0, 1.0);
    FragColor = vec4(vec3(ao), 1.0);
}