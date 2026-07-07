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

void UXOpenGLRenderDevice::BeginShadowMapFace(
    INT FaceIndex,
    const FMatrix& ViewMatrix, 
    const FMatrix& ProjMatrix, 
    const FVector& LightPos, 
    FLOAT LightRadius)
{
    guard(UXOpenGLRenderDevice::BeginShadowMapFace);

    auto Shader = dynamic_cast<DrawShadowMapProgram*>(Shaders[ShadowMap_Prog]);
    check(Shader);

    // Evaluate parameter capacity configurations safely before writing data
    const bool CanBuffer = Shader->ParametersBuffer.CanBuffer(1);
    Shader->Flush(!CanBuffer);

    // Fetch current element pointer safely within the device scope
    DrawShadowMapParameters* DrawCallParams = Shader->ParametersBuffer.GetCurrentElementPtr();
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

void UXOpenGLRenderDevice::EndShadowMapFace(INT FaceVertexCount)
{
    guard(UXOpenGLRenderDevice::EndShadowMapFace);

    auto Shader = dynamic_cast<DrawShadowMapProgram*>(Shaders[ShadowMap_Prog]);
    check(Shader);

    // CLOSE THE UNIFIED DRAW CALL AND FLUSH LOCALIZED GEOMETRY TO THIS FACE
    // Finalizes our single face command and commits it safely to the active FBO channel
    // before the outer loop can change the texture targets for the next direction!
    Shader->DrawBuffer.EndDrawCall(FaceVertexCount);
    Shader->Flush(false); 

    unguard;
}

void UXOpenGLRenderDevice::HandleShadowMapOverflow(INT& FaceVertexCounter)
{
    guard(UXOpenGLRenderDevice::HandleShadowMapOverflow);

    auto Shader = dynamic_cast<DrawShadowMapProgram*>(Shaders[ShadowMap_Prog]);
    check(Shader);

    // Close out the active filled command packet slot
    Shader->DrawBuffer.EndDrawCall(FaceVertexCounter);
    
    // Backup the active face parameters block data before trashing the state
    DrawShadowMapParameters StoredFaceParams;
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
              sizeof(DrawShadowMapParameters));

    // =========================================================================
    // true forces the framework to safely rotate the 
    // underlying VRAM rings and automatically resets NextElemIndex back to 0.
    // =========================================================================
    Shader->Flush(true);
    // =========================================================================

    // Open a fresh overflow packet slot inside our freshly rotated command buffer
    Shader->DrawBuffer.StartDrawCall();

    // =========================================================================
    // Because Flush(true) zeroed out NextElemIndex, GetCurrentElementPtr() will 
    // cleanly return the very first slot (Index 0) of the new ring allocation block!
    // =========================================================================
    DrawShadowMapParameters* OverflowParams = Shader->ParametersBuffer.GetCurrentElementPtr();
    
    appMemcpy(OverflowParams, &StoredFaceParams, sizeof(DrawShadowMapParameters));

    // Advance the parameter track by exactly 1 slot for this new command block instance
    Shader->ParametersBuffer.Advance(1);
    // =========================================================================

    unguard;
}

void UXOpenGLRenderDevice::DrawShadowMapSurface(
    const FSceneNode* Frame,
    UXOpenGLRenderDevice::FSurfInfo& SI,
    INT& FaceVertexCounter) // Pass FaceVertexCounter by reference to accumulate vertices across the face pass!
{
    guard(UXOpenGLRenderDevice::DrawShadowMapSurface);

    bool IsSolidBSP = (Frame->Recursion == 0) && !(SI.PolyFlags & (PF_Modulated | PF_FakeBackdrop | PF_NoSmooth | PF_Flat | PF_Unlit | PF_Highlighted | PF_FlatShaded | PF_Portal));
    if (!IsSolidBSP)
        return;

    auto Shader = dynamic_cast<DrawShadowMapProgram*>(Shaders[ShadowMap_Prog]);
    check(Shader);

    SetProgram(ShadowMap_Prog);

    // Evaluate vertex capacity space bounds only
    const bool CanBuffer = !Shader->DrawBuffer.IsFull()
                        && Shader->VertBuffer.CanBuffer(SI.Verts.Num());
    
    // Mid-surface overflows flush data safely without thashing or rotating ring slots!
    if (!CanBuffer)
    {
        HandleShadowMapOverflow(FaceVertexCounter);
        FaceVertexCounter = 0;
    }

    INT NumPts = SI.Verts.Num();
    TArray<glm::vec3> PolyVertices;
    PolyVertices.AddZeroed(NumPts);
    TArray<glm::uint>& TriIdx = SI.TriIdx;

    for (INT vi = 0; vi < NumPts; ++vi) {
        FVector Vert = SI.Verts(vi);
        PolyVertices(vi) = glm::vec3(Vert.X, Vert.Y, Vert.Z);
    }

    // Grab the current active DrawID. Because the face loop manages starting the call, 
    // this DrawID stays perfectly locked to your face index (0 to 5)!
    auto DrawID = Shader->DrawBuffer.GetDrawID();

    const INT numNodes = SI.Nodes.Num();
    for (INT ni = 0; ni < numNodes; ++ni)
    {
        const FNodeInfo& NI = SI.Nodes(ni);
        INT numTriVerts = NI.TriCount;
        INT triStart    = NI.TriStart;
        INT triEnd      = triStart + numTriVerts;

        if (NI.VertIndices.Num() < 3) continue;
        const INT NeededVerts = numTriVerts;

        auto Out = Shader->VertBuffer.GetCurrentElementPtr();

        for (INT ti = triStart; ti < triEnd; ti += 3)
        {
            const INT ia = TriIdx(ti);
            const INT ib = TriIdx(ti + 1);
            const INT ic = TriIdx(ti + 2);

            // Pack the mirrored 24-byte structural footprint cleanly
            Out->Coords     = PolyVertices(ia);   
            Out->DrawID     = DrawID; // Shared across all surfaces on this face!
            Out->Class      = 0u;
            Out++;

            Out->Coords     = PolyVertices(ib);   
            Out->DrawID     = DrawID;
            Out->Class      = 0u;
            Out++;

            Out->Coords     = PolyVertices(ic);   
            Out->DrawID     = DrawID;
            Out->Class      = 0u;
            Out++;
        }
        FaceVertexCounter += NeededVerts;
        Shader->VertBuffer.Advance(NeededVerts);
    }

    unguard;
}

static const DOUBLE AngleScale = (2.0 * PI) / 65536.0;
inline void GetAxes(FRotator R, FVector& X, FVector& Y, FVector& Z)
{
    // Convert the 16-bit integer Unreal angles into standard radians
    // UT99 angles map 65536 units to a full 360-degree circle (2 * PI)
    DOUBLE SP = appSin((DOUBLE)R.Pitch * AngleScale);
    DOUBLE CP = appCos((DOUBLE)R.Pitch * AngleScale);
    
    DOUBLE SY = appSin((DOUBLE)R.Yaw   * AngleScale);
    DOUBLE CY = appCos((DOUBLE)R.Yaw   * AngleScale);
    
    DOUBLE SR = appSin((DOUBLE)R.Roll  * AngleScale);
    DOUBLE CR = appCos((DOUBLE)R.Roll  * AngleScale);

    // FORWARD VECTOR (X Axis)
    X.X = (FLOAT)(CP * CY);
    X.Y = (FLOAT)(CP * SY);
    X.Z = (FLOAT)SP;

    // RIGHT VECTOR (Y Axis)
    Y.X = (FLOAT)((SR * SP * CY) - (CR * SY));
    Y.Y = (FLOAT)((SR * SP * SY) + (CR * CY));
    Y.Z = (FLOAT)(-SR * CP);

    // UP VECTOR (Z Axis)
    Z.X = (FLOAT)(-(CR * SP * CY) - (SR * SY));
    Z.Y = (FLOAT)(-(CR * SP * SY) + (SR * CY));
    Z.Z = (FLOAT)(CR * CP);
}

FORCEINLINE INT ResolveLodMeshVertex(
    const ULodMesh* L,
    INT wedgeIndex,
    INT level
)
{
    // Collapse wedge (LOD collapse)
    INT w = wedgeIndex;
    for (int t = 0; t < level; t++)
        if (L->CollapseWedgeThus.Num() > 0)
            w = L->CollapseWedgeThus(w);

    // Map wedge -> original vertex index
    INT v = L->Wedges(w).iVertex;

    // Collapse vertex (LOD collapse)
    for (int t = 0; t < level; t++)
        if (L->CollapsePointThus.Num() > 0)
            v = L->CollapsePointThus(v);

    // v is now the correct index into the posed vertex array
    return v;
}

void UXOpenGLRenderDevice::DrawShadowMapMesh(
    const FSceneNode* Frame,
    AActor* Actor,
    INT& FaceVertexCounter) // Stride-aligned signature receiving the face vertex accumulator reference
{
    guard(UXOpenGLRenderDevice::DrawShadowMapMesh);

    if (!Actor || !Actor->Mesh) return;
    UMesh* BaseMesh = Actor->Mesh;
    if (!BaseMesh)
        return;

    auto Shader = dynamic_cast<DrawShadowMapProgram*>(Shaders[ShadowMap_Prog]);
    check(Shader);

    SetProgram(ShadowMap_Prog);

    CachedStaticMeshGeometry* CachedMesh = PerFrameStaticMeshCache.Find(Actor);

    if (!CachedMesh)
    {
        // Cache Miss! First face/light processing this static mesh on this tick.
        CachedStaticMeshGeometry NewCache;

        UMesh* BaseMesh = Actor->Mesh;
        FMeshAnimSeq* Seq = BaseMesh->GetAnimSeq(Actor->AnimSequence);
        UBOOL bIsProjectile = Actor->IsA(AProjectile::StaticClass());
        UBOOL bStaticMesh = (Seq == nullptr) || bIsProjectile;
        FString MeshKey = Actor->Mesh->GetName();
        //const TCHAR* DebugMeshName = *MeshKey;

        // Branch extraction by mesh asset type
        if (!bStaticMesh)
        {
            FMeshConnectivity* Blueprint = GDiscoveredTopologies.Find(BaseMesh->GetName());
            if (!Blueprint) return; // Animated but not yet mapped -> Pass two will be handling it (we should actually never get here)
            ExtractMappedAnimatedTriangles((ULodMesh*)BaseMesh, Actor, *Blueprint, NewCache.Triangles);
        }
        else if (BaseMesh->IsA(USkeletalMesh::StaticClass()))
        {
            ExtractSkeletalMeshTriangles((USkeletalMesh*)BaseMesh, Actor, NewCache.Triangles);
        }
        else if (BaseMesh->IsA(ULodMesh::StaticClass()))
        {
            ExtractLodMeshTriangles((ULodMesh*)BaseMesh, Actor, NewCache.Triangles);
        }
        else if (BaseMesh->IsA(UMesh::StaticClass()))
        {
            const TCHAR* ClassName = Actor->GetClass()->GetName();
            ExtractUMeshTriangles(BaseMesh, Actor, NewCache.Triangles);
        }
        else
        {
            debugf(TEXT("Unsupported mesh type: %s"), BaseMesh->GetClass()->GetName());
            return;
        }
        // Securely lock it into our per-frame dictionary
        PerFrameStaticMeshCache.Set(Actor, NewCache);
        CachedMesh = PerFrameStaticMeshCache.Find(Actor);
    }
    const TArray<FShadowTriangle>& ShadowTris = CachedMesh->Triangles;
    INT NumTriangles = ShadowTris.Num();
    INT OutVertexCount = NumTriangles * 3;

    // Evaluate dynamic vertex capacities safely
    const bool CanBuffer = !Shader->DrawBuffer.IsFull()
                        && Shader->VertBuffer.CanBuffer(OutVertexCount);

    // Mid-face overflow safety: Flush current stream data cleanly without thashing ring memory blocks
    if (!CanBuffer)
    {
        HandleShadowMapOverflow(FaceVertexCounter);
        FaceVertexCounter = 0;
    }

    // Natively extract our current multi-draw command tracking index handle [Result 1].
    // Since draw calls are batched per face, this matches the active FaceIndex perfectly [Result 1]!
    auto DrawID = Shader->DrawBuffer.GetDrawID();

    // ------------------------------------------------------------
    // STAGE 2: Geometry Buffering Layout Pass
    // ------------------------------------------------------------
    auto Out = Shader->VertBuffer.GetCurrentElementPtr();

    // Loop through every triangle defined in the mesh asset layout structure
    for (INT i = 0; i < ShadowTris.Num(); i++)
    {
        const FShadowTriangle& T = ShadowTris(i);

        // buffer into your shadowmap vertex buffer
        FVector V0 = T.V0;
        FVector V1 = T.V1;
        FVector V2 = T.V2;

        // Triangle Corner 0: Fully pack the mirrored 24-byte layout footprint [Result 1]
        Out->Coords     = glm::vec3(V0.X, V0.Y, V0.Z); 
        Out->DrawID     = DrawID;
        Out->Class      = 1u; // 1u flags this vertex as an animated dynamic actor mesh [Result 1]
        Out->Padding    = 0u;
        Out++;

        // Triangle Corner 1
        Out->Coords     = glm::vec3(V1.X, V1.Y, V1.Z); 
        Out->DrawID     = DrawID;
        Out->Class      = 1u; 
        Out->Padding    = 0u;
        Out++;

        // Triangle Corner 2
        Out->Coords     = glm::vec3(V2.X, V2.Y, V2.Z); 
        Out->DrawID     = DrawID;
        Out->Class      = 1u;
        Out->Padding    = 0u;
        Out++;
    }

    // Accumulate the dynamic vertices into our active face tracking slot reference [Result 1]
    FaceVertexCounter += OutVertexCount;
    Shader->VertBuffer.Advance(OutVertexCount);

    unguard;
}

/*-----------------------------------------------------------------------------
	Shadowmap Program Metadata & Constructor Definitions
-----------------------------------------------------------------------------*/

UXOpenGLRenderDevice::DrawShadowMapProgram::DrawShadowMapProgram(const TCHAR* Name, UXOpenGLRenderDevice* RenDev)
	: ShaderProgramImpl(Name, RenDev)
{
	// Scale buffer capacities exactly like your core DrawComplex pipelines
	VertexBufferSize             = DRAWCOMPLEX_SIZE * sizeof(DrawShadowMapVertex);
	ParametersBufferSize         = DRAWCOMPLEX_SIZE; 
    ParametersBufferBindingIndex = GlobalShaderBindingIndices::ShadowMapParametersIndex;
	NumTextureSamplers           = 0;
	DrawMode                     = GL_TRIANGLES;
	
	// Drive capability matching your hardware/extension checking state toggles
	UseSSBOParametersBuffer      = RenDev->UsingShaderDrawParameters; 
	ParametersInfo               = DrawShadowMapParametersInfo;
    FacetIndexRingSize           = 0;
	FacetMetaRingSize            = 0;
	
	// Route code generation directly to your inline GLSL string builders
	VertexShaderFunc             = &BuildVertexShader;
	GeoShaderFunc                = nullptr;
	FragmentShaderFunc           = &BuildFragmentShader;
}

void UXOpenGLRenderDevice::DrawShadowMapProgram::CreateInputLayout()
{
	glEnableVertexAttribArray(0); // Coords Array
	glEnableVertexAttribArray(1); // DrawID Array
    glEnableVertexAttribArray(2); // Classification Stride Array
	glEnableVertexAttribArray(3); // Padding Stride Array

	// Coords attribute maps to standard float coordinates mapping vectors
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(DrawShadowMapVertex), (GLvoid*)offsetof(DrawShadowMapVertex, Coords));
	
	// CRITICAL SHADER LINK: Use glVertexAttribIPointer (with an 'I') for pure integers!
	glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(DrawShadowMapVertex), (GLvoid*)offsetof(DrawShadowMapVertex, DrawID));
	
    // classification
	glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(DrawShadowMapVertex), (GLvoid*)offsetof(DrawShadowMapVertex, Class));

	// Pass the padding slots cleanly as generic raw float vectors to prevent stride stalls
	glVertexAttribIPointer(3, 1, GL_UNSIGNED_INT, sizeof(DrawShadowMapVertex), (GLvoid*)offsetof(DrawShadowMapVertex, Padding));

	VertBuffer.SetInputLayoutCreated();
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
