/*=============================================================================
	DrawPrepass.cpp: Unreal XOpenGL DrawDepthOnly prepass routines.
	Used for BSP depth only pass.

	Revision history:
		* Created by MamiyaOtaru
=============================================================================*/

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "XOpenGLDrv.h"
#include "XOpenGL.h"

glm::vec3 ToVec3(const FPlane& P)
{
    return glm::vec3(P.X, P.Y, P.Z);
}

void UXOpenGLRenderDevice::DrawPrepassSurface(
    const FSceneNode* Frame,
    //const FSurfaceInfo& Surface,
    FSurfInfo& SI)
{
    guard(UXOpenGLRenderDevice::DrawDepthOnlySurface);

    // Depth/normal-only shader is already bound, state already set:
    // - DepthMask(true)
    // - DepthFunc(GL_LESS)
    // - No blending

    auto Shader = dynamic_cast<DrawPrepassProgram*>(Shaders[Prepass_Prog]);
    check(Shader);

    SetProgram(Prepass_Prog);

    // Start a new draw call in the depth-only vertex buffer
    Shader->DrawBuffer.StartDrawCall();
    //auto DrawID = Shader->DrawBuffer.GetDrawID();

    INT FacetVertexCount = 0;

	INT NumPts = SI.Verts.Num();

    TArray<glm::vec3> PolyVertices;
	PolyVertices.AddZeroed(NumPts);
    TArray<glm::vec3> PolyVertexNormals;
    PolyVertexNormals.AddZeroed(NumPts);

	TArray<glm::uint>& TriIdx = SI.TriIdx;

	// Build per-vertex data for this node polygon
	for (INT vi = 0; vi < NumPts; ++vi)
	{
        FVector Vert = SI.Verts(vi).TransformPointBy(Frame->Coords);
        FVector Normal = SI.VertexNormals(vi).TransformVectorBy(Frame->Coords).SafeNormal();

		PolyVertices(vi) = glm::vec3(Vert.X, Vert.Y, Vert.Z);
        PolyVertexNormals(vi) = glm::vec3(Normal.X, Normal.Y, Normal.Z);
	}

	const INT numNodes = SI.Nodes.Num();
	for (INT ni = 0; ni < numNodes; ++ni)
	{
		const FNodeInfo& NI = SI.Nodes(ni);

		INT numTriVerts = NI.TriCount;
		INT triStart = NI.TriStart;
		INT triEnd = triStart + numTriVerts;

		if (NI.VertIndices.Num() < 3)
			continue;

		const INT NeededVerts = numTriVerts;

        if (Shader->DrawBuffer.IsFull() || !Shader->VertBuffer.CanBuffer(NeededVerts))
        {
            // Flush what we have so far
            Shader->DrawBuffer.EndDrawCall(FacetVertexCount);
            Shader->Flush(true);
            Shader->DrawBuffer.StartDrawCall();
            //DrawID = Shader->DrawBuffer.GetDrawID();

            if (NeededVerts >= Shader->VertexBufferSize)
            {
                debugf(TEXT("DrawPrepassSurface facet too big (%d verts, need %d, buffer %d)"),
                       NumPts, NeededVerts, Shader->VertexBufferSize);
                continue;
            }

            FacetVertexCount = 0;
        }

        auto Out = Shader->VertBuffer.GetCurrentElementPtr();
        for (INT ti = triStart; ti < triEnd; ti += 3)
		{
			const INT ia = TriIdx(ti);
			const INT ib = TriIdx(ti + 1);
			const INT ic = TriIdx(ti + 2);
			Out->Coords     = PolyVertices(ia);
            Out->Normal     = PolyVertexNormals(ia);
            Out++;
			Out->Coords     = PolyVertices(ib);
            Out->Normal     = PolyVertexNormals(ib);
            Out++;
			Out->Coords     = PolyVertices(ic);
            Out->Normal     = PolyVertexNormals(ic);
            Out++;
        }

        FacetVertexCount += NeededVerts;
        Shader->VertBuffer.Advance(NeededVerts);
    }

    Shader->DrawBuffer.EndDrawCall(FacetVertexCount);

    unguard;
}

/*-----------------------------------------------------------------------------
	Prepass Surface Shader
-----------------------------------------------------------------------------*/

UXOpenGLRenderDevice::DrawPrepassProgram::DrawPrepassProgram(
    const TCHAR* Name,
    UXOpenGLRenderDevice* RenDev)
    : ShaderProgramImpl(Name, RenDev)
{
    VertexBufferSize             = DRAWCOMPLEX_SIZE * sizeof(DrawPrepassVertex);
    ParametersBufferSize         = 0;      // no per-draw params
    ParametersBufferBindingIndex = 0;      // unused
    NumTextureSamplers           = 0;
    DrawMode                     = GL_TRIANGLES;
    UseSSBOParametersBuffer      = false;
    ParametersInfo               = nullptr;
    bUseExternalShaders          = true;
    VertexShaderFunc             = nullptr;// &BuildVertexShader;
    GeoShaderFunc                = nullptr;
    FragmentShaderFunc           = nullptr;//&BuildFragmentShader;
    ExternalVertexPath           = TEXT("xopengl/shaders/prepass.vert");
    ExternalFragmentPath         = TEXT("xopengl/shaders/prepass.frag");
}

void UXOpenGLRenderDevice::DrawPrepassProgram::CreateInputLayout()
{
    // Position (location = 0)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0, 3, GL_FLOAT, GL_FALSE,
        sizeof(DrawPrepassVertex),
        (GLvoid*)offsetof(DrawPrepassVertex, Coords)
    );

    // Normal (location = 1)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 3, GL_FLOAT, GL_FALSE,
        sizeof(DrawPrepassVertex),
        (GLvoid*)offsetof(DrawPrepassVertex, Normal)
    );

    VertBuffer.SetInputLayoutCreated();
}

void UXOpenGLRenderDevice::DrawPrepassProgram::MapBuffers()
{
    VertBuffer.GenerateVertexBuffer(RenDev);
    VertBuffer.MapVertexBuffer(RenDev->UsingPersistentBuffers, VertexBufferSize);
    VertBuffer.Bind();
    CreateInputLayout();
    // no ParametersBuffer at all
}

void UXOpenGLRenderDevice::DrawPrepassProgram::UnmapBuffers()
{
    VertBuffer.DeleteBuffer();
    // no ParametersBuffer to delete
}

void UXOpenGLRenderDevice::DrawPrepassProgram::Flush(bool Rotate)
{
    const auto HavePendingData = DrawBuffer.TotalCommands > 0;

    if (!HavePendingData && !Rotate)
        return;

#if MACOSX || __LINUX_ARM__ || __LINUX_ARM64__
    Rotate = true;
#endif

    if (HavePendingData)
    {
        VertBuffer.BufferData(false);
        DrawBuffer.Draw(DrawMode, RenDev);
        //        glDrawArrays(GL_TRIANGLES,
        //             VertBuffer.SubBufferOffset,   // or 0 to start
        //             VertBuffer.NextElemIndex);    // total verts written

    }

    if (Rotate)
    {
        VertBuffer.Lock();
        VertBuffer.Rotate(true);
    }

    DrawBuffer.Reset(
        VertBuffer.SubBufferOffset + VertBuffer.NextElemIndex,
        0); // no ParametersBuffer
}

void UXOpenGLRenderDevice::DrawPrepassProgram::ActivateShader()
{
    VertBuffer.Wait();
    VertBuffer.Bind();
    if (!VertBuffer.IsInputLayoutCreated())
        CreateInputLayout();
    UseShader();
}

void UXOpenGLRenderDevice::DrawPrepassProgram::DeactivateShader()
{
    Flush(false);
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
