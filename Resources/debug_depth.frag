#version 330 core

in vec2 TexCoords;
out vec4 FragColor;

uniform sampler2D uDepth;

void main()
{
    float d = texture(uDepth, TexCoords).r;
    FragColor = vec4(d, d, d, 1.0);
}
