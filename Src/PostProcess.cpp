#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include "XOpenGLDrv.h"
#include "XOpenGL.h"

static GLuint noiseTex;
static UBOOL createdNoise = FALSE;

const int NOISE_SIZE = 8; // 4 or 8
GLuint UXOpenGLRenderDevice::CreateSSAONoiseTexture()
{
    if (!createdNoise)
    {
        std::vector<glm::vec3> noiseData;
        noiseData.reserve(NOISE_SIZE * NOISE_SIZE);

        for (int i = 0; i < NOISE_SIZE * NOISE_SIZE; i++)
        {
            float x = (float(rand()) / RAND_MAX) * 2.0f - 1.0f;
            float y = (float(rand()) / RAND_MAX) * 2.0f - 1.0f;

            noiseData.emplace_back(x, y, 0.0f);
        }

        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);

        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_RGBA32F,                // 16F is enough (or is it)
            NOISE_SIZE,
            NOISE_SIZE,
            0,
            GL_RGB,
            GL_FLOAT,
            noiseData.data()
        );

        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glBindTexture(GL_TEXTURE_2D, 0);
        noiseTex = tex;
        createdNoise = TRUE;
    }
    return noiseTex;
}

void UXOpenGLRenderDevice::DeleteSSAONoiseTexture()
{
    if (createdNoise)
    {
        if (noiseTex != 0)
        {
            glDeleteTextures(1, &noiseTex);
            noiseTex = 0;
        }
    }
    createdNoise = FALSE;
}

static float randFloat()
{
    return float(rand()) / float(RAND_MAX);
}

std::vector<glm::vec3> UXOpenGLRenderDevice::GenerateSSAOKernel(int total)
{
    std::vector<glm::vec3> kernel;
    kernel.reserve(total);

    for (int i = 0; i < total; i++)
    {
        // Random point in hemisphere
        float x = randFloat() * 2.0f - 1.0f;
        float y = randFloat() * 2.0f - 1.0f;
        float z = randFloat(); // hemisphere: z >= 0

        glm::vec3 sample(x, y, z);
        sample = glm::normalize(sample);

        // Random radius
        sample *= randFloat();

        // Scale so more samples are near the origin
        float scale = float(i) / float(total);
        scale = (1.0f - scale * scale) * 0.1f + (scale * scale) * 1.0f; // lerp(0.1, 1.0, scale*scale)

        sample *= scale;

        //sample.z *= -1;

        kernel.push_back(sample);
    }

    return kernel;
}

void UXOpenGLRenderDevice::CreateFullscreenQuad()
{
    if (FullscreenVAO != 0)
        return;

    const GLfloat quadVerts[] =
    {
        // pos      // uv
        -1.f, -1.f,  0.f, 0.f,
        -1.f,  1.f,  0.f, 1.f,
         1.f, -1.f,  1.f, 0.f,
         1.f,  1.f,  1.f, 1.f
    };

    glGenVertexArrays(1, &FullscreenVAO);
    glGenBuffers(1, &FullscreenVBO);

    glBindVertexArray(FullscreenVAO);
    glBindBuffer(GL_ARRAY_BUFFER, FullscreenVBO);

    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVerts), quadVerts, GL_STATIC_DRAW);

    // Position (location = 0)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0, 2, GL_FLOAT, GL_FALSE,
        4 * sizeof(GLfloat),
        (GLvoid*)0
    );

    // UV (location = 1)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 2, GL_FLOAT, GL_FALSE,
        4 * sizeof(GLfloat),
        (GLvoid*)(2 * sizeof(GLfloat))
    );

    glBindVertexArray(0);
}

void UXOpenGLRenderDevice::DeleteFullscreenQuad()
{
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (FullscreenVBO)
    {
        glDeleteBuffers(1, &FullscreenVBO);
        FullscreenVBO = 0;
    }
    if (FullscreenVAO)
    {
        glDeleteVertexArrays(1, &FullscreenVAO);
        FullscreenVAO = 0;
    }
}

void UXOpenGLRenderDevice::DrawFullscreenQuad()
{
    glBindVertexArray(FullscreenVAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void UXOpenGLRenderDevice::RunSSAOPass(FSceneNode* Frame)
{
    guard(UXOpenGLRenderDevice::RunSSAOPass);

    // Create noise texture once
	GLuint SsaoNoiseTex = CreateSSAONoiseTexture();

    // Bind half-res SSAO FBO
    SsaoFbo->Bind();
    glViewport(0, 0, SceneWidth / 2, SceneHeight / 2);

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    glClearColor(1, 1, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    // Activate SSAO shader
    SetProgram(SSAO_Prog);
    auto Shader = static_cast<SSAOProgram*>(Shaders[SSAO_Prog]);

    // -----------------------------
    // Bind textures
    // -----------------------------
    glActiveTexture(GL_TEXTURE20);
    glBindTexture(GL_TEXTURE_2D, gbufferFbo->depthTexID);
    glGenerateMipmap(GL_TEXTURE_2D); // mipmap for distance independent SSAO speed. Originators make their own in a shader to avoid something or other. Consider

    glActiveTexture(GL_TEXTURE21);
    glBindTexture(GL_TEXTURE_2D, gbufferFbo->colorTexIDs[0]); // normals

    glActiveTexture(GL_TEXTURE22);
    glBindTexture(GL_TEXTURE_2D, SsaoNoiseTex);

    if (Shader->uDepth != -1)
        glUniform1i(Shader->uDepth, 20);

    if (Shader->uNormal != -1)
        glUniform1i(Shader->uNormal, 21);

    if (Shader->uNoise != -1)
        glUniform1i(Shader->uNoise, 22);

    // -----------------------------
    // SSAO parameters
    // -----------------------------
    const float zNear = 1.0f;
    const float zFar  = 65336.0f;

    if (Shader->uNearPlane != -1)
        glUniform1f(Shader->uNearPlane, zNear);

    if (Shader->uFarPlane != -1)
        glUniform1f(Shader->uFarPlane, zFar);

    if (Shader->uNoiseScale != -1)
        glUniform2f(Shader->uNoiseScale,
            float(Frame->X) / NOISE_SIZE,
            float(Frame->Y) / NOISE_SIZE);

    // -----------------------------
    // Kernel
    // -----------------------------
    const int kernelSize = 64;

    if (SSAOKernel.empty())
        SSAOKernel = GenerateSSAOKernel(kernelSize);

	// Upload kernel samples
    if (Shader->uSamples[0] != -1)
    {
        glUniform3fv(Shader->uSamples[0], kernelSize, &SSAOKernel[0].x);
    }

	if (Shader->uKernelSize != -1)
		glUniform1i(Shader->uKernelSize, kernelSize);

    // -----------------------------
    // Draw fullscreen quad
    // -----------------------------
    DrawFullscreenQuad();

    Shader->Flush(false);

	RunSSAOBlurPass(5);

    // Restore state
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_BLEND);

    unguard;
}

void UXOpenGLRenderDevice::RunSSAOBlurPass(int iterations)
{
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
    SetProgram(SsaoBlur_Prog);
    auto Shader = static_cast<SsaoBlurProgram*>(Shaders[SsaoBlur_Prog]);

    glViewport(0, 0, SceneWidth / 2, SceneHeight / 2);
    Shader->SetResolution(SceneWidth / 2, SceneHeight / 2);

    for (int i = 0; i < iterations; i++)
    {
        //
        // PASS 1 - Horizontal blur: SsaoFbo -> SsaoBlurFbo
        //
        SsaoBlurFbo->Bind();

        glActiveTexture(GL_TEXTURE20);
        glBindTexture(GL_TEXTURE_2D, SsaoFbo->colorTexIDs[0]);

        Shader->SetInput(20);

        Shader->SetOffset(1.0f, 0.0f);

        DrawFullscreenQuad();
        Shader->Flush(false);

        //
        // PASS 2 - Vertical blur: SsaoBlurFbo -> SsaoFbo
        //
        SsaoFbo->Bind();

        glActiveTexture(GL_TEXTURE20);
        glBindTexture(GL_TEXTURE_2D, SsaoBlurFbo->colorTexIDs[0]);

        Shader->SetInput(20);

        Shader->SetOffset(0.0f, 1.0f);

        DrawFullscreenQuad();
        Shader->Flush(false);
    }
}

void UXOpenGLRenderDevice::PreparePrepassDepthTexture()
{
    // GBuffer depth
    if ((AmbientOcclusion) && gbufferFbo && gbufferFbo->depthTexID)
    {
        if (UsingBindlessTextures)
        {
            gbufferFbo->GetDepthBindlessHandle();
        }
        else
        {
            gbufferFbo->BindDepthTexture(PrepassDepthIndex);
        }
    }
}

void UXOpenGLRenderDevice::RunSSRPass()
{
    guard(UXOpenGLRenderDevice::RunSSRPass);

    // Write raw SSR result into SsaoFbo (reuse half-res FBO)
    SsaoFbo->Bind();
    glViewport(0, 0, SceneWidth / 2, SceneHeight / 2);

    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ZERO);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    // Activate SSR shader
    SetProgram(SSR_Prog);
    auto Shader = static_cast<SSRProgram*>(Shaders[SSR_Prog]);

    // ------------------------------------------------------------
    // Bind textures
    // ------------------------------------------------------------

    // solid surfaces that can show up in reflections
    glActiveTexture(GL_TEXTURE20);
    glBindTexture(GL_TEXTURE_2D, ResolveFbo->colorTexIDs[2]);

    // SSRBuffer (depth + roughness + oct normal)
    glActiveTexture(GL_TEXTURE21);
    glBindTexture(GL_TEXTURE_2D, ResolveFbo->colorTexIDs[1]);

    // ------------------------------------------------------------
    // SSR parameters (per-frame)
    // ------------------------------------------------------------

    if (Shader->uScreenSize != -1)
        glUniform2f(Shader->uScreenSize, float(SceneWidth), float(SceneHeight));

    if (Shader->uMaxSteps != -1)
        glUniform1i(Shader->uMaxSteps, 64);

    if (Shader->uStepSize != -1)
        glUniform1f(Shader->uStepSize, 25.0f);

    if (Shader->uFadeDistance != -1)
        glUniform1f(Shader->uFadeDistance, 4096.0f);

    // ------------------------------------------------------------
    // Draw fullscreen quad
    // ------------------------------------------------------------
    DrawFullscreenQuad();
    Shader->Flush(false);

    // Optional blur for roughness
    RunSSAOBlurPass(3);

    unguard;
}

void UXOpenGLRenderDevice::RunSSRCompositePass()
{
    guard(UXOpenGLRenderDevice::RunSSRCompositePass);

    CompositeFbo->Bind();
    glViewport(0, 0, SceneWidth, SceneHeight);

    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);

    SetProgram(SSRComposite_Prog);
    auto Shader = static_cast<SSRCompositeProgram*>(Shaders[SSRComposite_Prog]);

    // Direct color
    glActiveTexture(GL_TEXTURE20);
    glBindTexture(GL_TEXTURE_2D, ResolveFbo->colorTexIDs[0]);

    // SSR result (blurred half-res)
    glActiveTexture(GL_TEXTURE21);
    glBindTexture(GL_TEXTURE_2D, SsaoFbo->colorTexIDs[0]);

    // SSRBuffer (roughness mask)
    glActiveTexture(GL_TEXTURE22);
    glBindTexture(GL_TEXTURE_2D, ResolveFbo->colorTexIDs[1]);

    // UI
    glActiveTexture(GL_TEXTURE23);
    glBindTexture(GL_TEXTURE_2D, ResolveFbo->colorTexIDs[3]);

    DrawFullscreenQuad();

    Shader->Flush(false);

    unguard;
}


