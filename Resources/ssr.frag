#version 330 core

in vec2 TexCoords;
out vec4 FragColor;

// -----------------------------------------------------------------------------
// Inputs
// -----------------------------------------------------------------------------
uniform sampler2D uSceneColor;          // ResolveFbo->colorTexIDs[0]
uniform sampler2D uSSRBuffer;           // ResolveFbo->colorTexIDs[1] (depth, packed ORM, packed normal)
uniform sampler2D uSSRBufferSurface;    // ResolveFbo->colorTexIDs[2] (depth, isMesh, packed normal)

uniform vec2 uScreenSize;

uniform int   uMaxSteps;
uniform float uStepSize;
uniform float uFadeDistance;

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

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

vec2 trueSign(vec2 v) {
    // step(0.0, v) returns 1.0 if v >= 0.0, else 0.0
    // Multiplying by 2 and subtracting 1 maps 1.0->1.0 and 0.0->-1.0
    return step(0.0, v) * 2.0 - 1.0;
}

vec3 unpackNormal(float f)
{
    float Nx = floor(f / 65536.0);
    float Ny = floor((f - Nx * 65536.0) / 256.0);
    float Nz = f - Nx * 65536.0 - Ny * 256.0;

    vec3 N01 = vec3(Nx, Ny, Nz) / 255.0;
    vec3 N =  N01 * 2.0 - 1.0;
    // UT does weird things
    N.y *= -1;
    N.z *= -1;
    return N;
}

float LinearizeDepth(float depth)
{
    // Match UT SSAO constants
    const float nearPlane = 0.5;
    const float farPlane  = 65336.0;

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

vec2 projectToUV(vec3 viewPos)
{
    vec4 clip = projMat * vec4(viewPos, 1.0);
    float w = clip.w;
    vec3 ndc = clip.xyz / w;
    vec2 uv   = ndc.xy * 0.5 + 0.5;
    return uv;
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------
void main()
{
    vec4 buf = texture(uSSRBuffer, TexCoords);

    float depth  = buf.r;
    float packedNormal = buf.b;
    vec3 normal = unpackNormal(packedNormal);
    float packedRM = buf.g;
    float roughness = floor(packedRM / 256.0);
    float metalness = packedRM - roughness * 256.0;
    roughness = roughness / 255.0;
    metalness = metalness / 255.0;

    // Extract projection scale from projMat (same one used for depth)
    float fx = projMat[0][0];
    float fy = projMat[1][1];

    // View-space position
    vec3 viewPos = ReconstructViewPos(TexCoords, depth, fx, fy);
    
    // View direction is simply normalized view-space position
    vec3 V = normalize(viewPos);
    // Two-sided translucent surfaces: flip normal if facing away
    // This is essential for glass, water sheets, masked sheets, etc.
    if (dot(normal, -V) < 0.0)
        normal = -normal;
    vec3 R = reflect(V, normal);

    vec3 rayPos = viewPos;

    vec4 result = vec4(0.0);

    for (int i = 0; i < uMaxSteps; i++)
    {
        rayPos += R * uStepSize;

        vec2 suv = projectToUV(rayPos);

        if (suv.x < 0.0 || suv.x > 1.0 ||
            suv.y < 0.0 || suv.y > 1.0)
            break;

        vec4 hitBuf = texture(uSSRBufferSurface, suv);
        float sceneDepth = hitBuf.r;
        sceneDepth = LinearizeDepth(sceneDepth); 

        // Ray is behind geometry -> hit (not too far behind though
        float delta = rayPos.z - sceneDepth;
        if (delta > 0.0)
        {
            if (delta <= uStepSize*1.25)
            {
                // rays can go behind objects and emerge from them
                // so light can bounce around a pillar.  When only stop when the distance is small (we are at a surface)
                // then, if BSP check normals to see if it is in fact visible from the reflector.  For meshes, cheat
                // since the back of eg a health pack looks like the front.  Allows the part we see to be reflected by the wall
                // even if the wall can't see that part.  Prevents it from disappearing when we are precisely in line with the object and the wall's perpendicular
                bool isMesh = hitBuf.g == 1;
                float packedHitNormal = hitBuf.b;
                vec3 hitNormal = unpackNormal(packedHitNormal);

                float facing = dot(hitNormal, R);
                if (isMesh || facing > 0.0) {
                    vec4 hitColor = texture(uSceneColor, suv);
                    float distAtten = 1.0 - float(i) / float(uMaxSteps);
                    float roughAtten = 1.0 - roughness;
                    float edgeFade = clamp(min(
                        min(suv.x, 1.0 - suv.x),
                        min(suv.y, 1.0 - suv.y)
                    ) * 5.0, 0.0, 1.0);
                    float baseReflect = mix(0.04, 1.0, metalness);
                    float NdotV = max(dot(normal, -V), 0.0);
                    float fresnel = pow(1.0 - NdotV, 5.0);
                    // schlick approximation
                    float reflectStrength = baseReflect + (1.0 - baseReflect) * fresnel;
                    float ssrStrength = reflectStrength;

                    result = vec4(hitColor.rgb, ssrStrength * distAtten * roughAtten * edgeFade);
                }
                break;
            }
        }
    }
    
    FragColor = result;

    // debug outputs
    //FragColor = texture(uSceneColor, TexCoords);
    //FragColor = vec4(normal*.5+.5, 1);
    //FragColor = vec4(buf.r,0,0,1);
}
