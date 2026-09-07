#version 330 core
out vec4 FragColor;

in vec2 TexCoords;

uniform sampler2D image;
uniform vec2 offset;
uniform vec2 resolution;

void main()
{
    vec2 texel = 1.0 / resolution;

    vec4 center = texture(image, TexCoords);
    float centerAO = center.r;

    vec4 color = center * 0.2270270270;
    float normalization = 0.2270270270;

    // Offsets
    vec2 off1 = 1.3846153846 * offset * texel;
    vec2 off2 = 3.2307692308 * offset * texel;

    // Tap 1
    vec4 s1 = texture(image, TexCoords + off1);
    float w1 = max(0.0, 1.0 - abs(s1.r - centerAO) * 4.0) * 0.3162162162;
    color += s1 * w1;
    normalization += w1;

    s1 = texture(image, TexCoords - off1);
    w1 = max(0.0, 1.0 - abs(s1.r - centerAO) * 4.0) * 0.3162162162;
    color += s1 * w1;
    normalization += w1;

    // Tap 2
    vec4 s2 = texture(image, TexCoords + off2);
    float w2 = max(0.0, 1.0 - abs(s2.r - centerAO) * 4.0) * 0.0702702703;
    color += s2 * w2;
    normalization += w2;

    s2 = texture(image, TexCoords - off2);
    w2 = max(0.0, 1.0 - abs(s2.r - centerAO) * 4.0) * 0.0702702703;
    color += s2 * w2;
    normalization += w2;

    FragColor = vec4(color.rgba / normalization);//, 1.0);
}
