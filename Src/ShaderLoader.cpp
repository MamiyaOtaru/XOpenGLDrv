#include "ShaderLoader.h"
#include <fstream>
#include <sstream>

bool ShaderLoader::LoadTextFile(const char* Path, std::string& Out)
{
    std::ifstream file(Path, std::ios::in | std::ios::binary);
    if (!file)
    {
        debugf(TEXT("XOpenGL: Failed to open shader file: %s"), ANSI_TO_TCHAR(Path));
        return false;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    Out = ss.str();
    return true;
}

GLuint ShaderLoader::CompileShader(GLenum ShaderType, const char* Path)
{
    std::string Source;
    if (!LoadTextFile(Path, Source))
    {
        debugf(TEXT("XOpenGL: Failed to load shader file: %s"), ANSI_TO_TCHAR(Path));
        return 0;
    }

    GLuint Shader = glCreateShader(ShaderType);
    const char* SrcPtr = Source.c_str();
    glShaderSource(Shader, 1, &SrcPtr, nullptr);
    glCompileShader(Shader);

    GLint Status = GL_FALSE;
    glGetShaderiv(Shader, GL_COMPILE_STATUS, &Status);

    if (Status != GL_TRUE)
    {
        GLint LogLen = 0;
        glGetShaderiv(Shader, GL_INFO_LOG_LENGTH, &LogLen);

        std::string Log(LogLen, '\0');
        glGetShaderInfoLog(Shader, LogLen, nullptr, &Log[0]);

        const TCHAR* TypeName =
            (ShaderType == GL_VERTEX_SHADER)   ? TEXT("vertex") :
            (ShaderType == GL_FRAGMENT_SHADER) ? TEXT("fragment") :
            (ShaderType == GL_GEOMETRY_SHADER) ? TEXT("geometry") :
                                                 TEXT("unknown");

        debugf(TEXT("XOpenGL: %ls shader compile error in %s:\n%s"),
               TypeName, ANSI_TO_TCHAR(Path), *FString(Log.c_str()));

        glDeleteShader(Shader);
        return 0;
    }

    return Shader;
}

bool ShaderLoader::LoadExternalShaders(GLuint Program,
                                       const char* VertexPath,
                                       const char* FragmentPath)
{
    // Compile vertex shader
    GLuint Vert = CompileShader(GL_VERTEX_SHADER, VertexPath);
    if (!Vert)
    {
        debugf(TEXT("XOpenGL: Failed to compile external vertex shader: %s"),
               ANSI_TO_TCHAR(VertexPath));
        return false;
    }

    // Compile fragment shader
    GLuint Frag = CompileShader(GL_FRAGMENT_SHADER, FragmentPath);
    if (!Frag)
    {
        debugf(TEXT("XOpenGL: Failed to compile external fragment shader: %s"),
               ANSI_TO_TCHAR(FragmentPath));
        glDeleteShader(Vert);
        return false;
    }

    // Attach both
    glAttachShader(Program, Vert);
    glAttachShader(Program, Frag);

    // Link program
    glLinkProgram(Program);

    GLint Status = GL_FALSE;
    glGetProgramiv(Program, GL_LINK_STATUS, &Status);

    if (Status != GL_TRUE)
    {
        GLint LogLen = 0;
        glGetProgramiv(Program, GL_INFO_LOG_LENGTH, &LogLen);

        std::string Log(LogLen, '\0');
        glGetProgramInfoLog(Program, LogLen, nullptr, &Log[0]);

        debugf(TEXT("XOpenGL: Program link error:\n%s"),
               *FString(Log.c_str()));

        debugf(TEXT("External VS: %s"), ANSI_TO_TCHAR(VertexPath));
        debugf(TEXT("External FS: %s"), ANSI_TO_TCHAR(FragmentPath));

        glDeleteShader(Vert);
        glDeleteShader(Frag);

        return false;
    }

    // Safe to delete shaders after linking
    glDeleteShader(Vert);
    glDeleteShader(Frag);

    return true;
}