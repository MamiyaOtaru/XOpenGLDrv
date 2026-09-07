#version 330 core

in vec3 FragNormal;

layout(location = 0) out vec3 outNormal;

void main()
{
    outNormal = normalize(FragNormal);
    // Depth is written automatically
}