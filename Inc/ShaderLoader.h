#pragma once

#include "XOpenGLDrv.h"
extern "C" {
    #include "glad.h"
}
#include <string>

class ShaderLoader
{
public:
    // Load a text file into a string
    static bool LoadTextFile(const char* Path, std::string& Out);

    // Compile a shader from file
    static GLuint CompileShader(GLenum ShaderType, const char* Path);

    // Load, compile, and attach vertex + fragment shaders to a program
    static bool LoadExternalShaders(GLuint Program,
                                    const char* VertexPath,
                                    const char* FragmentPath);
};