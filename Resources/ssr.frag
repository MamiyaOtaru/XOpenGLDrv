#version 330 core

in vec2 TexCoords;
out vec4 FragColor;

// -----------------------------------------------------------------------------
// Inputs
// -----------------------------------------------------------------------------
uniform sampler2D uSceneColor;   // ResolveFbo->colorTexIDs[0]
uniform sampler2D uSSRBuffer;    // ResolveFbo->colorTexIDs[1] (depth, roughness, oct-normal)

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

vec3 decodeOctNormal(vec2 e)
{
    vec3 v = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    
    // Fully branchless hemisphere unfolding
    vec2 folded = (1.0 - abs(v.yx)) * trueSign(v.xy);
    v.xy = mix(v.xy, folded, step(v.z, 0.0));

    v = normalize(v);

    // UT does weird things
    v.y *= -1;
    v.z *= -1;
    
    return v;
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
    float roughness = fract(buf.g * 2);
    vec2 oct = buf.ba;
    
    vec3 normal = decodeOctNormal(oct);

    // Extract projection scale from projMat (same one used for depth)
    float fx = projMat[0][0];
    float fy = projMat[1][1];

    // View-space position
    vec3 viewPos = ReconstructViewPos(TexCoords, depth, fx, fy);
    
    // View direction is simply normalized view-space position
    vec3 V = normalize(viewPos);
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

        vec4 hitBuf = texture(uSSRBuffer, suv);
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
                bool isMesh = (floor(hitBuf.g * 2) >= 1);
                vec2 hitOct = hitBuf.ba;
                vec3 hitNormal = decodeOctNormal(hitOct);
                float facing = dot(hitNormal, R);
                if (isMesh || facing > 0.0) {
                    vec4 hitColor = texture(uSceneColor, suv);
                    float distAtten = 1.0 - float(i) / float(uMaxSteps);
                    float roughAtten = 1.0 - roughness;
                    float edgeFade = clamp(min(
                        min(suv.x, 1.0 - suv.x),
                        min(suv.y, 1.0 - suv.y)
                    ) * 5.0, 0.0, 1.0);
                    result = vec4(hitColor.rgb, distAtten * roughAtten * edgeFade);
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
