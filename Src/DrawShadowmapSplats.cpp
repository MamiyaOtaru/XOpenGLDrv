/*=============================================================================
	DrawShadowmap.cpp: Unreal XOpenGL DrawShadowmap routines.
	Used for BSP depth only pass.

	Revision history:
		* Created by MamiyaOtaru
=============================================================================*/

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "XOpenGLDrv.h"
#include "XOpenGL.h"

void UXOpenGLRenderDevice::BeginShadowMapSplatsFace(
    INT FaceIndex,
    const FMatrix& ViewMatrix, 
    const FMatrix& ProjMatrix, 
    const FVector& LightPos, 
    FLOAT LightRadius)
{
    guard(UXOpenGLRenderDevice::BeginShadowMapSplatsFace);

    auto Shader = dynamic_cast<DrawShadowMapSplatsProgram*>(Shaders[ShadowMapSplats_Prog]);
    check(Shader);

    // Evaluate parameter capacity configurations safely before writing data
    const bool CanBuffer = Shader->ParametersBuffer.CanBuffer(1);
    Shader->Flush(!CanBuffer);

    // Fetch current element pointer safely within the device scope
    DrawShadowMapSplatsParameters* DrawCallParams = Shader->ParametersBuffer.GetCurrentElementPtr();
    DrawCallParams->ModelMatrix = glm::mat4(1.0f);

    // Convert FMatrix to column-major GLM matrix format
    glm::mat4 ViewMatGLM = glm::mat4(1.0f);
    glm::mat4 ProjMatGLM = glm::mat4(1.0f);
    for (int row = 0; row < 4; ++row)
    {
        for (int col = 0; col < 4; ++col)
        {
            ViewMatGLM[col][row] = ViewMatrix.M(row, col);
            ProjMatGLM[col][row] = ProjMatrix.M(row, col);
        }
    }
    DrawCallParams->ViewMatrix = ViewMatGLM;
    DrawCallParams->ProjMatrix = ProjMatGLM;

    DrawCallParams->LightWorldPos  = glm::vec4(LightPos.X, LightPos.Y, LightPos.Z, 1.0f);
    DrawCallParams->LightRadius    = LightRadius;
    DrawCallParams->IsDynamicActor = 0u; // FALSE for BSP base

    // Advance the parameter slot exactly once for this full face pass
    Shader->ParametersBuffer.Advance(1);

    // OPEN THE UNIFIED DRAW CALL FOR THIS ENTIRE FACE
    // This forces DrawID to increment exactly by 1 per face step 
    // but it can also increment mid call, in which case the parameters are duplicated to the new index
    Shader->DrawBuffer.StartDrawCall();

    unguard;
}

void UXOpenGLRenderDevice::EndShadowMapSplatsFace(INT FaceVertexCount)
{
    guard(UXOpenGLRenderDevice::EndShadowMapSplatsFace);

    auto Shader = dynamic_cast<DrawShadowMapSplatsProgram*>(Shaders[ShadowMapSplats_Prog]);
    check(Shader);

    // CLOSE THE UNIFIED DRAW CALL AND FLUSH LOCALIZED GEOMETRY TO THIS FACE
    // Finalizes our single face command and commits it safely to the active FBO channel
    // before the outer loop can change the texture targets for the next direction!
    Shader->DrawBuffer.EndDrawCall(FaceVertexCount);
    Shader->Flush(false); 

    unguard;
}

void UXOpenGLRenderDevice::HandleShadowMapSplatsOverflow(INT& FaceVertexCounter)
{
    guard(UXOpenGLRenderDevice::HandleShadowMapSplatsOverflow);

    auto Shader = dynamic_cast<DrawShadowMapSplatsProgram*>(Shaders[ShadowMapSplats_Prog]);
    check(Shader);

    // 1. Close out the active filled command packet slot
    Shader->DrawBuffer.EndDrawCall(FaceVertexCounter);
    
    // 2. Backup the active face parameters block data before trashing the state
    DrawShadowMapSplatsParameters StoredFaceParams;
    INT LastIndex;

    if (Shader->ParametersBuffer.NextElemIndex > 0)
    {
        LastIndex = Shader->ParametersBuffer.NextElemIndex - 1;
    }
    else
    {
        // Wrapped: previous element is the last element in the ring
        LastIndex = Shader->ParametersBuffer.Size() - 1;
    }

    appMemcpy(&StoredFaceParams,
              Shader->ParametersBuffer.GetElementPtr(LastIndex),
              sizeof(DrawShadowMapSplatsParameters));

    // =========================================================================
    // FIX PART A: FORCE A TRUE RING ROTATION ON OVERFLOW
    // Changed from false to true! This forces the framework to safely rotate the 
    // underlying VRAM rings and automatically resets NextElemIndex back to 0.
    // =========================================================================
    Shader->Flush(true);
    // =========================================================================

    // 3. Open a fresh overflow packet slot inside our freshly rotated command buffer
    Shader->DrawBuffer.StartDrawCall();

    // =========================================================================
    // FIX PART B: CRASH-PROOF ELEMENT POINTER FETCHING
    // Because Flush(true) zeroed out NextElemIndex, GetCurrentElementPtr() will 
    // cleanly return the very first slot (Index 0) of the new ring allocation block!
    // =========================================================================
    DrawShadowMapSplatsParameters* OverflowParams = Shader->ParametersBuffer.GetCurrentElementPtr();
    
    appMemcpy(OverflowParams, &StoredFaceParams, sizeof(DrawShadowMapSplatsParameters));

    // Advance the parameter track by exactly 1 slot for this new command block instance
    Shader->ParametersBuffer.Advance(1);
    // =========================================================================

    unguard;
}

void UXOpenGLRenderDevice::DrawShadowMapSplats(
    const FSceneNode* Frame,
    AActor* Actor,
    INT& FaceVertexCounter)
{
    guard(UXOpenGLRenderDevice::DrawShadowMapSplats);

    if (!Actor || !Actor->Mesh) return;

    auto Shader = dynamic_cast<DrawShadowMapSplatsProgram*>(Shaders[ShadowMapSplats_Prog]);
    check(Shader);

    SetProgram(ShadowMapSplats_Prog);

    CachedActorSplatArray* CachedEntry = PerFrameActorSplatCache.Find(Actor);

    if (!CachedEntry)
    {
        // Cache Miss! This is either the first light processing this actor on this frame,
        // or a completely new frame tick. We allocate and parse the math exactly ONCE.
        CachedActorSplatArray NewCache;
        NewCache.LastCachedFrame = LocalFrameCounter;
        
        ExtractLodMeshCapsules((ULodMesh*)Actor->Mesh, Actor, NewCache.Splats);
        
        // Store it inside our global device dictionary
        PerFrameActorSplatCache.Set(Actor, NewCache);
        CachedEntry = PerFrameActorSplatCache.Find(Actor);
    }

    // Extract splat centers/radii
    TArray<FCapsuleSplat> Splats = CachedEntry->Splats;

    INT OutVertexCount = Splats.Num(); // 1 vertex per splat

    // Overflow handling
    if (!Shader->VertBuffer.CanBuffer(OutVertexCount) ||
        Shader->DrawBuffer.IsFull())
    {
        HandleShadowMapSplatsOverflow(FaceVertexCounter);
        FaceVertexCounter = 0;
    }

    glm::uint32 DrawID = Shader->DrawBuffer.GetDrawID();
    auto Out = Shader->VertBuffer.GetCurrentElementPtr();

    for (INT i = 0; i < Splats.Num(); i++)
    {
        const FCapsuleSplat& S = Splats(i);

        Out->P0 = glm::vec3(S.P0.X, S.P0.Y, S.P0.Z);
        Out->P1 = glm::vec3(S.P1.X, S.P1.Y, S.P1.Z);
        Out->Radius = S.Radius;
        Out->DrawID = DrawID;
        Out++;
    }

    FaceVertexCounter += OutVertexCount;
    Shader->VertBuffer.Advance(OutVertexCount);

    unguard;
}

/*-----------------------------------------------------------------------------
	Shadowmap Program Metadata & Constructor Definitions
-----------------------------------------------------------------------------*/

UXOpenGLRenderDevice::DrawShadowMapSplatsProgram::DrawShadowMapSplatsProgram(const TCHAR* Name, UXOpenGLRenderDevice* RenDev)
	: ShaderProgramImpl(Name, RenDev)
{
	// Scale buffer capacities like core DrawComplex pipelines
	VertexBufferSize             = DRAWCOMPLEX_SIZE * sizeof(DrawShadowMapSplatsVertex);
	ParametersBufferSize         = DRAWCOMPLEX_SIZE; 
    ParametersBufferBindingIndex = GlobalShaderBindingIndices::ShadowMapSplatsParametersIndex;
	NumTextureSamplers           = 0;
	DrawMode                     = GL_POINTS;;
	
	// Drive capability matching hardware/extension checking state toggles
	UseSSBOParametersBuffer      = RenDev->UsingShaderDrawParameters; 
	ParametersInfo               = DrawShadowMapSplatsParametersInfo;
    FacetIndexRingSize           = 0;
	FacetMetaRingSize            = 0;
	
	// Route code generation directly to inline GLSL string builders
	VertexShaderFunc             = &BuildVertexShader;
	GeoShaderFunc                = &BuildGeometryShader;
	FragmentShaderFunc           = &BuildFragmentShader;
}

/*void UXOpenGLRenderDevice::DrawShadowMapSplatsProgram::CreateInputLayout()
{
    // Center (vec3)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0,
        3,
        GL_FLOAT,
        GL_FALSE,
        sizeof(DrawShadowMapSplatsVertex),
        (GLvoid*)offsetof(DrawShadowMapSplatsVertex, Center)
    );

    // Radius (float)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1,
        1,
        GL_FLOAT,
        GL_FALSE,
        sizeof(DrawShadowMapSplatsVertex),
        (GLvoid*)offsetof(DrawShadowMapSplatsVertex, Radius)
    );

    // DrawID (uint)
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(
        2,
        1,
        GL_UNSIGNED_INT,
        sizeof(DrawShadowMapSplatsVertex),
        (GLvoid*)offsetof(DrawShadowMapSplatsVertex, DrawID)
    );

    VertBuffer.SetInputLayoutCreated();
}*/
void UXOpenGLRenderDevice::DrawShadowMapSplatsProgram::CreateInputLayout()
{
    glEnableVertexAttribArray(0); // P0
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE,
        sizeof(DrawShadowMapSplatsVertex),
        (GLvoid*)offsetof(DrawShadowMapSplatsVertex, P0));

    glEnableVertexAttribArray(1); // P1
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE,
        sizeof(DrawShadowMapSplatsVertex),
        (GLvoid*)offsetof(DrawShadowMapSplatsVertex, P1));

    glEnableVertexAttribArray(2); // Radius
    glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE,
        sizeof(DrawShadowMapSplatsVertex),
        (GLvoid*)offsetof(DrawShadowMapSplatsVertex, Radius));

    glEnableVertexAttribArray(3); // DrawID
    glVertexAttribIPointer(3, 1, GL_UNSIGNED_INT,
        sizeof(DrawShadowMapSplatsVertex),
        (GLvoid*)offsetof(DrawShadowMapSplatsVertex, DrawID));

    VertBuffer.SetInputLayoutCreated();
}



/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
