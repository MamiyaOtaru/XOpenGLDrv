#version 330 core

layout(location = 0) in vec3 Coords;
layout(location = 1) in vec3 Normal;

layout(std140) uniform FrameState {
    mat4 projMat;
    mat4 viewMat;
    mat4 modelMat;
    mat4 modelviewMat;
    mat4 modelviewprojMat;
};

out vec3 FragNormal;

void main()
{
    // Transform normal into view space
    FragNormal = mat3(modelviewMat) * Normal;

    // Standard position transform
    gl_Position = modelviewprojMat * vec4(Coords, 1.0);
}