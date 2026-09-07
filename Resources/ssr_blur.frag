#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D image;        // your SSR buffer
uniform sampler2D depthTex;     // linear depth texture
uniform vec2 offset;
uniform vec2 resolution;

// tweak this — smaller = sharper edges, larger = softer blur
const float DEPTH_SIGMA = 40.0;

void main()
{
    vec2 texel = 1.0 / resolution;

    float centerDepth = texture(depthTex, TexCoords).r;

    vec4 center = texture(image, TexCoords);
    vec4 color = center * 0.2270270270;
    float normalization = 0.2270270270;

    vec2 off1 = 1.3846153846 * offset * texel;
    vec2 off2 = 3.2307692308 * offset * texel;

    // Tap 1 (+off1)
    float d1 = texture(depthTex, TexCoords + off1).r;
    float w1 = exp(-abs(centerDepth - d1) * DEPTH_SIGMA) * 0.3162162162;
    color += texture(image, TexCoords + off1) * w1;
    normalization += w1;

    // Tap 1 (-off1)
    d1 = texture(depthTex, TexCoords - off1).r;
    w1 = exp(-abs(centerDepth - d1) * DEPTH_SIGMA) * 0.3162162162;
    color += texture(image, TexCoords - off1) * w1;
    normalization += w1;

    // Tap 2 (+off2)
    float d2 = texture(depthTex, TexCoords + off2).r;
    float w2 = exp(-abs(centerDepth - d2) * DEPTH_SIGMA) * 0.0702702703;
    color += texture(image, TexCoords + off2) * w2;
    normalization += w2;

    // Tap 2 (-off2)
    d2 = texture(depthTex, TexCoords - off2).r;
    w2 = exp(-abs(centerDepth - d2) * DEPTH_SIGMA) * 0.0702702703;
    color += texture(image, TexCoords - off2) * w2;
    normalization += w2;

    FragColor = vec4(color / normalization);
}
