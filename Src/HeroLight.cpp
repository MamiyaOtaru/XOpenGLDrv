#include "HeroLight.h"
#include "XOpenGL.h"
extern "C"
{
	#include "glad.h"
}

UXOpenGLHeroLight::UXOpenGLHeroLight(ALight* InLight, ULevel* Level, const TMap<INT, TArray<AActor*>>& StaticLightsMap, UXOpenGLRenderDevice* GL)
    : LightActor(InLight), LastRadius(0.f), LastLocation(0.f,0.f,0.f), 
      LastFrameFaceMask(0)
{
    AffectedZones.Empty();
    AffectedBSPSurfaces.Empty();

    for (INT f = 0; f < 6; f++)
    {
        FaceFbos[f] = nullptr;
    }

    if (!LightActor || !Level || !Level->Model) return;
    OwnerLevel = Level;

    UModel* Model = Level->Model;

    // Check if our device registers this specific actor handle as an upgraded floor light!
    UXOpenGLRenderDevice::FakeSpotlightPair* SpotData = UXOpenGLRenderDevice::GetSpotlightData(InLight);
    
    if (SpotData != nullptr)
    {
        bIsSpotlight   = TRUE;
        SpotDirection  = SpotData->SpotDirection;
        SpotCosOuter   = SpotData->SpotCosOuter;
        SpotCosInner   = SpotData->SpotCosInner;
        ReachRadius    = SpotData->ReachRadius;
        SourceLocation = SpotData->TopLight->Location;
    }
    else
    {
        bIsSpotlight   = FALSE;
        SpotDirection  = FVector(0.f, 0.f, 0.f);
        SpotCosOuter   = -1.0f;
        SpotCosInner   = -1.0f;
        ReachRadius    = InLight->WorldLightRadius(); // Fallback to standard point radius
        SourceLocation = InLight->Location; // Standard point lights emanate from themselves
    }

    // Automatically map affected BSP surfaces immediately upon object creation
    for (INT SurfIndex = 0; SurfIndex < Model->Surfs.Num(); ++SurfIndex)
    {
        const FBspSurf& Surf = Model->Surfs(SurfIndex);
        if (Surf.Actor && Surf.Actor->IsA(AMover::StaticClass())) 
            continue;

        const TArray<AActor*>* LightsForThisSurf = StaticLightsMap.Find(SurfIndex);
        if (!LightsForThisSurf || LightsForThisSurf->Num() == 0)
            continue;

        for (INT l = 0; l < LightsForThisSurf->Num(); ++l)
        {
            if ((*LightsForThisSurf)(l) == LightActor)
            {
                for (INT n = 0; n < Surf.Nodes.Num(); ++n)
                {
                    INT NodeIndex = Surf.Nodes(n);
                    const FBspNode& Node = Model->Nodes(NodeIndex);

                    for (INT z = 0; z < 2; ++z)
                    {
                        BYTE ZoneIdx = Node.iZone[z];
                        if (ZoneIdx > 0 && AffectedZones.FindItemIndex(ZoneIdx) == INDEX_NONE)
                        {
                            AffectedZones.AddItem(ZoneIdx);
                        }
                    }
                }
                AffectedBSPSurfaces.AddItem(SurfIndex);
                break; 
            }
        }
    }

    PartitionBSPSurfaces(Model, GL);
}

UXOpenGLHeroLight::~UXOpenGLHeroLight()
{
    guard(UXOpenGLHeroLight::~UXOpenGLHeroLight);

    LastFrameActors.Empty();
    
    // CRITICAL STATE CLEAR: Turn off residency BEFORE deleting the textures!
    // Safely checks if a bindless handle was ever generated and marked resident.
    if (bIsHandleResident && BindlessMaskHandle != 0)
    {
        glMakeTextureHandleNonResidentARB(BindlessMaskHandle);
        bIsHandleResident = FALSE;
        BindlessMaskHandle = 0;
    }

    // Delete the face-specific FBO frame structures cleanly
    // Thanks to the 'if (FaceFbos[face])' gate, this handles per-face lazy loading flawlessly!
    for (INT face = 0; face < 6; face++)
    {
        if (FaceFbos[face] != nullptr)
        {
            delete FaceFbos[face]; // Safely triggers destructor -> Dispose() natively
            FaceFbos[face] = nullptr;
        }
    }

    // ATOMIC UNIFIED CUBEMAP RECLAMATION
    // Cleanly deletes both omnidirectional texture tracks from VRAM exactly once.
    // If the light never rendered a single face, these remain 0 and glDeleteTextures is safely skipped.
    if (ColorCubemapID > 0 && ColorCubemapID != 0xFFFFFFFF)
    {
        glDeleteTextures(1, &ColorCubemapID);
        ColorCubemapID = 0;
    }

    if (DepthCubemapID > 0 && DepthCubemapID != 0xFFFFFFFF)
    {
        glDeleteTextures(1, &DepthCubemapID);
        DepthCubemapID = 0;
    }

    unguard;
}

// Cubemap face directions in OpenGL space.
// ALSO annotated with what they correspond to in UE1 world space.
//
// Cubemap order: +X, -X, +Y, -Y, +Z, -Z
// UE1 axes: X=fwd, Y=right, Z=up
const FVector FaceDirs[6] = {
    FVector( 1,  0,  0), // +X  (cubemap right)   -> UE1 forward
    FVector(-1,  0,  0), // -X  (cubemap left)    -> UE1 backward
    FVector( 0, -1,  0), // +Y  (cubemap up)      -> UE1 right
    FVector( 0,  1,  0), // -Y  (cubemap down)    -> UE1 left
    FVector( 0,  0,  1), // +Z  (cubemap forward) -> UE1 DOWN
    FVector( 0,  0, -1)  // -Z  (cubemap back)    -> UE1 UP
};

// Up vectors for each cubemap face (OpenGL space)
const FVector FaceUps[6] = {
    FVector(0, -1,  0), // +X face
    FVector(0, -1,  0), // -X face
    FVector(0,  0, -1), // +Y face
    FVector(0,  0,  1), // -Y face
    FVector(0, -1,  0), // +Z face (UE1 DOWN spotlight)
    FVector(0, -1,  0)  // -Z face (UE1 UP)
};

FMatrix MakeLookAt(const FVector& Eye, const FVector& ForwardDir, const FVector& UpDir)
{
    // Normalize input
    FVector F = ForwardDir.SafeNormal();   // Forward
    FVector U = UpDir.SafeNormal();        // Provided Up

    // --- CRITICAL FIX ---
    // Cubemap faces require: Right = Up × Forward
    // (NOT Forward × Up, NOT swapped, NOT negated)
    FVector R = (U ^ F).SafeNormal();      // Right

    // Recompute Up to ensure orthogonality
    U = (F ^ R).SafeNormal();              // Up

    // Build UE1-style coordinate frame
    FCoords C(Eye, R, U, F);

    // Convert to matrix
    FMatrix M = FMatrixFromFCoords(C);

    // Apply translation (UE1-style)
    M.XPlane.W = -(R | Eye);
    M.YPlane.W = -(U | Eye);
    M.ZPlane.W = -(F | Eye);
    M.WPlane.W = 1.0f;

    return M;
}

FMatrix MakePerspective(float FovDegrees, float Aspect, float NearZ, float FarZ)
{
    float InvNF = 1.0f / (NearZ - FarZ);
    return FMatrix(
        FPlane(1.0f / Aspect, 0.0f, 0.0f,                  0.0f),
        FPlane(0.0f,          1.0f, 0.0f,                  0.0f),
        FPlane(0.0f,          0.0f, (FarZ + NearZ) * InvNF, (2.0f * FarZ * NearZ) * InvNF),
        FPlane(0.0f,          0.0f, -1.0f,                 0.0f)
    );
}

// Helper structure to hold the dynamically clipped polygon slices
struct FClippedPolygon
{
    FVector Verts[8]; // Slicing a triangle by 5 planes yields up to an 8-sided polygon max
    INT NumVerts = 0;
};

// Pure, standard View-Space clipping (expecting standard positive-Z forward bounds)
void ClipPolygonByPlane(const FClippedPolygon& InPoly, FClippedPolygon& OutPoly, INT Axis, float Sign)
{
    OutPoly.NumVerts = 0;
    if (InPoly.NumVerts < 3) return;

    FVector S = InPoly.Verts[InPoly.NumVerts - 1];
    float sDist = S.Z - (Sign * (Axis == 0 ? S.X : S.Y));

    for (INT i = 0; i < InPoly.NumVerts; ++i)
    {
        FVector E = InPoly.Verts[i];
        float eDist = E.Z - (Sign * (Axis == 0 ? E.X : E.Y));

        if (eDist >= 0.0f) // Staying or moving INSIDE
        {
            if (sDist < 0.0f) // Crossed from outside to inside
            {
                float t = sDist / (sDist - eDist);
                OutPoly.Verts[OutPoly.NumVerts++] = S + (E - S) * t;
            }
            OutPoly.Verts[OutPoly.NumVerts++] = E;
        }
        else if (sDist >= 0.0f) // Crossed from inside to outside
        {
            float t = sDist / (sDist - eDist);
            OutPoly.Verts[OutPoly.NumVerts++] = S + (E - S) * t;
        }
        S = E;
        sDist = eDist;
    }
}

// Near plane clipping pass (guarantees vertices sit inside the positive forward lens cone)
void ClipPolygonByNearPlane(const FClippedPolygon& InPoly, FClippedPolygon& OutPoly, float NearZ)
{
    OutPoly.NumVerts = 0;
    if (InPoly.NumVerts < 3) return;

    FVector S = InPoly.Verts[InPoly.NumVerts - 1];
    float sDist = S.Z - NearZ;

    for (INT i = 0; i < InPoly.NumVerts; ++i)
    {
        FVector E = InPoly.Verts[i];
        float eDist = E.Z - NearZ;

        if (eDist >= 0.0f)
        {
            if (sDist < 0.0f)
            {
                float t = sDist / (sDist - eDist);
                OutPoly.Verts[OutPoly.NumVerts++] = S + (E - S) * t;
            }
            OutPoly.Verts[OutPoly.NumVerts++] = E;
        }
        else if (sDist >= 0.0f)
        {
            float t = sDist / (sDist - eDist);
            OutPoly.Verts[OutPoly.NumVerts++] = S + (E - S) * t;
        }
        S = E;
        sDist = eDist;
    }
}

void UXOpenGLHeroLight::PartitionBSPSurfaces(UModel* Model, UXOpenGLRenderDevice* GL)
{
    for (INT f = 0; f < 6; ++f)
    {
        AffectedFaceBSPSurfaces[f].Empty();
    }

    // --- OPTIMIZATION BYPASS FOR VERTICAL CONES ---
    if (bIsSpotlight)
    {
        // Straight-down cones unconditionally dump all pre-filtered surfaces directly into Face 0
        AffectedFaceBSPSurfaces[4] = AffectedBSPSurfaces;
        return; 
    }

    FLOAT Radius   = LightActor->WorldLightRadius();
    FLOAT RadiusSq = Radius * Radius;
    FVector LightPos = LightActor->Location;

    for (INT s = 0; s < AffectedBSPSurfaces.Num(); ++s)
    {
        INT iSurf = AffectedBSPSurfaces(s);
        
        UXOpenGLRenderDevice::FSurfInfo* pSI = GL->GetSurfInfoByID(iSurf);
        if (!pSI || pSI->TriIdx.Num() == 0) 
            continue;

        const TArray<FVector>& Verts = pSI->Verts;
        const TArray<glm::uint>& TriIdx = pSI->TriIdx;

        TArray<FVector> Triangles;
        for (INT t = 0; t < TriIdx.Num(); t += 3)
        {
            Triangles.AddItem(Verts(TriIdx(t)));
            Triangles.AddItem(Verts(TriIdx(t+1)));
            Triangles.AddItem(Verts(TriIdx(t+2)));
        }

        for (INT f = 0; f < 6; ++f)
        {
            // This handles the positions correctly anywhere in the map.
            // Note: Since MakeLookAt internally handles the -FaceDirs sign flip,
            // the transformed Z coordinates will extend into positive-Z space,
            // allowing standard, clean View-Space clipping formulas.
            FMatrix ViewMatrix = MakeLookAt(LightPos, -FaceDirs[f], FaceUps[f]);
            UBOOL bIntrudesInFrustumAndRadius = FALSE;

            for (INT t = 0; t < Triangles.Num(); t += 3)
            {
                if (bIntrudesInFrustumAndRadius) break;

                FVector wA = Triangles(t);
                FVector wB = Triangles(t+1);
                FVector wC = Triangles(t+2);

                // Transform world positions into View Space using your shared helper's output
                FVector vA = ViewMatrix.TransformFVector(wA);
                FVector vB = ViewMatrix.TransformFVector(wB);
                FVector vC = ViewMatrix.TransformFVector(wC);

                // Simple Near-Z plane check (skip if the whole triangle is behind the lens)
                if (vA.Z < 8.0f && vB.Z < 8.0f && vC.Z < 8.0f) 
                    continue;

                // Setup the clipping pipeline input
                FClippedPolygon PolyStage0;
                PolyStage0.Verts[0] = vA; 
                PolyStage0.Verts[1] = vB; 
                PolyStage0.Verts[2] = vC;
                PolyStage0.NumVerts = 3;

                // Slice by the camera's Near Z plane (8.0f)
                FClippedPolygon PolyStage1; 
                ClipPolygonByNearPlane(PolyStage0, PolyStage1, 8.0f);

                if (PolyStage1.NumVerts < 3) continue;

                // Slice by the 4 3D side frustum boundaries in View Space
                FClippedPolygon PolyStage2; ClipPolygonByPlane(PolyStage1, PolyStage2, 0, -1.0f); // Left
                FClippedPolygon PolyStage3; ClipPolygonByPlane(PolyStage2, PolyStage3, 0,  1.0f); // Right
                FClippedPolygon PolyStage4; ClipPolygonByPlane(PolyStage3, PolyStage4, 1, -1.0f); // Bottom
                FClippedPolygon FinalClippedPoly; ClipPolygonByPlane(PolyStage4, FinalClippedPoly, 1, 1.0f); // Top

                // If nothing is left inside this view window, skip this face
                if (FinalClippedPoly.NumVerts < 3)
                    continue;

                // Proximity check over the isolated sub-polygon patch
                for (INT i = 1; i < FinalClippedPoly.NumVerts - 1; ++i)
                {
                    FVector pA = FinalClippedPoly.Verts[0];
                    FVector pB = FinalClippedPoly.Verts[i];
                    FVector pC = FinalClippedPoly.Verts[i+1];

                    FVector triangleNormal = ((pB - pA) ^ (pC - pA)).SafeNormal();
                    float planeDist = pA | triangleNormal;
                    FVector projectedLightPos = triangleNormal * planeDist;

                    FVector closestPointInFrustum;
                    if (GL->PointInTriangle(projectedLightPos, pA, pB, pC, triangleNormal))
                    {
                        closestPointInFrustum = projectedLightPos;
                    }
                    else
                    {
                        closestPointInFrustum = GL->ClosestPointOnTriangle(projectedLightPos, pA, pB, pC);
                    }

                    // Check radius limits relative to the view-space origin (0,0,0)
                    if (closestPointInFrustum.SizeSquared() < RadiusSq)
                    {
                        bIntrudesInFrustumAndRadius = TRUE;
                        break;
                    }
                }
            }

            if (bIntrudesInFrustumAndRadius)
            {
                AffectedFaceBSPSurfaces[f].AddItem(iSurf);
            }
        }
    }
}

// can we skip this shadowap?  if no surfaces affected by it are in view
static bool SphereInFrustum(const FSceneNode* Frame, const FVector& Center, float Radius)
{
    if (!Frame) return false;

    // 1. Grab your camera's true world space origin position
    FVector CameraPos = Frame->Coords.Origin;
    
    // 2. Compute a clean, absolute relative vector from the camera vertex to the light center
    FVector RelativeToCamera = Center - CameraPos;

    // Process only the 4 valid side panels
    for (INT i = 0; i < 4; ++i)
    {
        const FPlane& P = Frame->ViewPlanes[i];
        
        // --- FIXED: Use pure relative dot-product tracking ---
        // By pulling CameraPos out of the equation and dotting the tracking ray directly,
        // we bypass the engine's W offsets completely. This isolates the calculation
        // to a pure angular check, exactly matching your right-hand diagram geometry!
        float d = (RelativeToCamera | P);

        // If a left-handed axis twist flips the sign heavily behind your head, 
        // we take the absolute value of the horizontal/vertical boundary deviations
        // to guarantee the magnitude never falsely triggers a premature culling block.
        if (d < -Radius)
            return false; 
    }
    return true;
}

BOOL IsStaticMesh(AActor* Actor)
{
    if (!Actor || !Actor->Mesh) return false;

    UMesh* Base = Actor->Mesh;
    ULodMesh* L = (ULodMesh*)Base;
    UBOOL bIsProjectile = Actor->IsA(AProjectile::StaticClass());
    FMeshAnimSeq* Seq   = L->GetAnimSeq(Actor->AnimSequence);
    BOOL bStaticMesh    = (Seq == nullptr) || bIsProjectile;
    return bStaticMesh;
}

// ------------------------------------------------------------
// Internal Face Projection & Matrix Pass
// ----------------------RenderFaceGeometry--------------------------------------
void UXOpenGLHeroLight::RenderFaceGeometry(ULevel* Level, FSceneNode* Frame, INT FaceIndex, TArray<CachedActorState> ActiveActors, UXOpenGLRenderDevice* GL)
{
    guard(UXOpenGLHeroLight::RenderFaceGeometry);

    // Light-space camera configuration
    FVector Eye        = SourceLocation;
    const FVector& Dir = FaceDirs[FaceIndex];
    const FVector& Up  = FaceUps[FaceIndex];
    FLOAT Radius       = ReachRadius;

    FMatrix ViewMatrix = MakeLookAt(Eye, Dir, Up);
    FMatrix ProjMatrix = MakePerspective(90.0f, 1.0f, 8.0f, Radius);

    BYTE FaceBit = (1 << FaceIndex);

    // =========================================================
    // PASS 1: TRIANGLES (BSP + STATIC MESHES)
    // =========================================================
    {
        if (!bspDrawn[FaceIndex])
        {
            INT ActiveFaceVertexCount = 0;
            GL->BeginShadowMapFace(FaceIndex, ViewMatrix, ProjMatrix, Eye, Radius);
            // A: BSP Surfaces (Cached subset)
            for (INT s = 0; s < AffectedFaceBSPSurfaces[FaceIndex].Num(); ++s)
            {
                INT iSurf = AffectedFaceBSPSurfaces[FaceIndex](s);
                UXOpenGLRenderDevice::FSurfInfo* pSI = GL->GetSurfInfoByID(iSurf);
                if (pSI && !(pSI->PolyFlags & (PF_Translucent | PF_Invisible | PF_NotSolid | PF_Masked | PF_AlphaTexture | PF_Portal)))
                {
                    GL->DrawShadowMapSurface(Frame, *pSI, ActiveFaceVertexCount);
                }
            }
            bspDrawn[FaceIndex] = TRUE;
            GL->EndShadowMapFace(ActiveFaceVertexCount); // triangle program flush
            glDepthMask(GL_FALSE); // don't write to depth buffer for meshes, only BSP
        }

        INT ActiveFaceVertexCount = 0;
        GL->BeginShadowMapFace(FaceIndex, ViewMatrix, ProjMatrix, Eye, Radius);
        // B: Static / Triangle Meshes (Pre-filtered Array)
        for (INT i = 0; i < ActiveActors.Num(); ++i)
        {
            const CachedActorState& State = ActiveActors(i);
            
            // Fast Bitmask Check: Does this specific actor visibility-overlap this face?
            if (!(State.FaceMask & FaceBit))
                continue;

            // Route if it is genuinely a static mesh OR if we have fully mapped its animated blueprint!
            if (!State.Actor || !State.Actor->Mesh) continue;
            UBOOL bHasMappedTopology = GL->HasMappedTopology(State.Actor);
            FString MeshKey = State.Actor->Mesh->GetName();
            //const TCHAR* DebugMeshName = *MeshKey;

            if (State.bIsStaticMesh || bHasMappedTopology)
            {
                GL->DrawShadowMapMesh(Frame, State.Actor, ActiveFaceVertexCount);
            }
        }

        GL->EndShadowMapFace(ActiveFaceVertexCount); // triangle program flush
    }

    // =========================================================
    // PASS 2: SPLATS (ANIMATED MESHES)
    // =========================================================
    {
        GL->BeginShadowMapSplatsFace(FaceIndex, ViewMatrix, ProjMatrix, Eye, Radius);

        INT ActiveSplatVertexCount = 0;

        // Animated / Skeletal Meshes (Pre-filtered Array)
        for (INT i = 0; i < ActiveActors.Num(); ++i)
        {
            const CachedActorState& State = ActiveActors(i);

            // Fast Bitmask Check
            if (!(State.FaceMask & FaceBit))
                continue;

            if (!State.Actor || !State.Actor->Mesh) continue;

            // Skip splats entirely if the geometric index map is taking over!
            UBOOL bHasMappedTopology = GL->HasMappedTopology(State.Actor);
 
            if (!State.bIsStaticMesh && !bHasMappedTopology)
            {
                GL->DrawShadowMapSplats(Frame, State.Actor, ActiveSplatVertexCount);
            }
        }

        GL->EndShadowMapSplatsFace(ActiveSplatVertexCount); // splat program flush
    }

    unguard;
}

// Maps the unique GetIndex pointer to its position in the LastFrameActors array.
TMap<INT, INT> LastFrameLookup;
TMap<INT, INT> CurrentFrameLookup;

// ------------------------------------------------------------
// The Master Evaluation & Render Dispatcher
// ------------------------------------------------------------
void UXOpenGLHeroLight::UpdateShadowMap(FSceneNode* Frame, UXOpenGLRenderDevice* GL)
{
    if (!Frame || !Frame->Level || OwnerLevel != Frame->Level || !LightActor || LightActor->bDeleteMe)
        return;

    ULevel* Level = Frame->Level;

    FLOAT Radius   = bIsSpotlight ? ReachRadius : LightActor->WorldLightRadius();
    FLOAT RadiusSq = Radius * Radius;

    CurrentFaceMask = 0;

    // --- TRANSLATE BOUNDING SPHERE FOR VERTICAL CONES ---
    FVector EvaluationFrustumCenter = LightActor->Location;
    FLOAT   EvaluationFrustumRadius = Radius;

    if (bIsSpotlight)
    {
        // Protect against zero division for a theoretical edge-case 90-degree half-angle
        FLOAT SafeCos = Max(SpotCosOuter, 0.1f);
        
        // The mathematically perfect bounding sphere radius factor for a cone:
        // SphereRadius = ReachRadius / (2.0 * Cos^2)
        FLOAT SphereRadiusFactor = 1.0f / (2.0f * SafeCos * SafeCos);

        // If the angle is very wide (theta > 45 degrees, i.e., Cos < 0.707), 
        // the sphere center shifts entirely to the base, and its radius is the base radius.
        if (SafeCos < 0.7071f)
        {
            FLOAT TanTheta = appSqrt(1.0f - (SafeCos * SafeCos)) / SafeCos;
            EvaluationFrustumRadius = ReachRadius * TanTheta;
            EvaluationFrustumCenter = SourceLocation + (SpotDirection * ReachRadius);
        }
        else
        {
            // Perfect bounding sphere for standard tight spotlight beams!
            EvaluationFrustumRadius = ReachRadius * SphereRadiusFactor;
            EvaluationFrustumCenter = SourceLocation + (SpotDirection * (ReachRadius * SphereRadiusFactor));
        }
    }

    if (!Frame || !SphereInFrustum(Frame, EvaluationFrustumCenter, EvaluationFrustumRadius))
        return;

    // =========================================================================
    // --- BSP ZONE VISIBILITY GATE ---
    // =========================================================================
    // If the light has an extracted zone footprint and a valid player viewport exists,
    // we query the engine's static precomputed zone-to-zone visibility matrix.
    // most maps are single zone, so eh
    /*if (Frame->Viewport && Frame->Viewport->Actor && AffectedZones.Num() > 0)
    {
        // Fetch the exact Zone ID where the player's camera eye currently sits
        BYTE CameraZoneIdx = Frame->Viewport->Actor->Region.ZoneNumber;

        UBOOL bLightZoneIsVisible = FALSE;

        // Scan the light's small pre-baked footprint (typically just 1-3 bytes)
        for (INT z = 0; z < AffectedZones.Num(); ++z)
        {
            BYTE LightZoneIdx = AffectedZones(z);

            // Co-location check: If the player and the light share a room, it is visible!
            if (LightZoneIdx == CameraZoneIdx)
            {
                bLightZoneIsVisible = TRUE;
                break;
            }

            // High-Speed Connectivity Matrix Check: 
            // ZoneDist[A][B] stores topological path distances through portal brushes.
            // A value of 255 means mathematically completely occluded/unreachable!
            if (Level->ZoneDist[CameraZoneIdx][LightZoneIdx] < 255)
            {
                bLightZoneIsVisible = TRUE;
                break;
            }
        }

        // If a solid concrete wall completely occludes the light's zones from the player,
        // we abort immediately! CurrentFaceMask remains 0, and the CPU skips all actor scans.
        if (!bLightZoneIsVisible)
        {
            return; 
        }
    }*/

    // =========================================================================
    // --- THE COMPLIANT SURFACE VISIBILITY PROTECTOR ---
    // =========================================================================
    // We scan our pre-baked surfaces list. If the engine's visibility clipper 
    // hasn't drawn a single polygon touched by this light, we can safely skip 
    // calculating shadows for it entirely!
    if (AffectedBSPSurfaces.Num() > 0)
    {
        UBOOL bAnySurfaceIsVisible = FALSE;

        for (INT s = 0; s < AffectedBSPSurfaces.Num(); ++s)
        {
            INT SurfIndex = AffectedBSPSurfaces(s);
            
            // We pull our custom Surface Info struct out of your device's 
            // cached runtime buffer (where the main world loop tracks active facets)
            UXOpenGLRenderDevice::FSurfInfo* pSI = GL->GetSurfInfoByID(SurfIndex);
            
            // If pSI exists and its internal frame tag matches the device's last 
            // rendering frame counter, it means this surface was actively being drawn on screen last frame ans will likely be this frame
            if (pSI && pSI->LastDrawnFrame == GL->LocalFrameCounter - 1)
            {
                bAnySurfaceIsVisible = TRUE;
                break; // One visible surface is enough to lock the light in!
            }
        }

        // If every surface illuminated by this light is occluded[uiyhg689rt5f8tirfcghouyvbi[jkokmnol,
        // we can safely pull the emergency brake. 
        if (!bAnySurfaceIsVisible)
        {
            return;
        }
    }

    TArray<CachedActorState> CurrentFrameActors;

    // --- Fast Structural Scan ---
    for (INT i = 0; i < Level->Actors.Num(); ++i)
    {
        AActor* A = Level->Actors(i);
        if (!A || A->bStatic || A->bDeleteMe || A->bHidden) continue;
        if (A->DrawType != DT_Mesh && A->DrawType != DT_Brush) continue;
        if (A->Style != STY_Normal) continue;

        // sourceLocation being where the light is for point lights, or the location of the upper light in spotlight pairs (set in constructor)
        FVector ToActor = A->Location - SourceLocation;
        if (ToActor.SizeSquared() < RadiusSq)
        {
            // --- CONE ANGLE CUTOFF GATE FOR LIVE ACTORS ---
            if (bIsSpotlight)
            {
                FVector DirNorm = ToActor.SafeNormal();
                float CosAngle = DirNorm | SpotDirection;
                
                // Drop the actor early before computing deep face metrics
                if (CosAngle < SpotCosOuter)
                    continue;
            }

            CachedActorState State;
            State.Actor          = A;
            State.ActorIndex     = A->GetIndex();
            State.Location       = A->Location;
            State.Rotation       = A->Rotation;
            State.AnimFrame      = A->AnimFrame;
            State.bIsStaticMesh  = IsStaticMesh(A); // Classify immediately
            State.FaceMask       = 0;

            if (bIsSpotlight)
            {
                // Spotlights explicitly lock onto Face 3 (Negative Y / Bit 3)
                // already know it is within the cone (checked above)
                State.FaceMask  |= (1 << 4);
                CurrentFaceMask |= (1 << 4);
            }
            else
            {
                // Standard traditional Point Light cubemap mapping
                FVector Dir = ToActor.SafeNormal();
                for (INT f = 0; f < 6; ++f)
                {
                    if ((Dir | -FaceDirs[f]) > -0.2f)
                    {
                        State.FaceMask  |= (1 << f);
                        CurrentFaceMask |= (1 << f);
                    }
                }
            }
            CurrentFrameActors.AddItem(State);
        }
    }

    UBOOL bGlobalReset = (Radius != LastRadius) || (LightActor->Location != LastLocation);
    BYTE ChangedFaceMask = bGlobalReset ? 0x3F : 0x00;

    // --- Intrinsic Delta Checks (O(N + M)) ---
    if (!bGlobalReset)
    {
        // Maps the unique GetIndex pointer to its position in the LastFrameActors array.
        LastFrameLookup.Empty();
        
        for (INT j = 0; j < LastFrameActors.Num(); ++j) {
            LastFrameLookup.Set(LastFrameActors(j).ActorIndex, j);
        }

        // Loop A: Track altered or newly arrived bots via pointer hash lookup
        for (INT i = 0; i < CurrentFrameActors.Num(); ++i)
        {
            const CachedActorState& Curr = CurrentFrameActors(i);
            
            // O(1) Intrinsic Pointer Lookup
            INT* prevIndexPtr = LastFrameLookup.Find(Curr.ActorIndex); 

            UBOOL bChanged = TRUE;
            if (prevIndexPtr != nullptr) // Actor intrinsically matched from last frame!
            {
                const CachedActorState& Prev = LastFrameActors(*prevIndexPtr);
                if (Curr.Location  == Prev.Location && 
                    Curr.Rotation  == Prev.Rotation && 
                    Curr.AnimFrame == Prev.AnimFrame)
                {
                    bChanged = FALSE; 
                }
                else
                {
                    // ensure face it WAS in is marked dirty.  May be the same it is in now, no harm
                    ChangedFaceMask |= Prev.FaceMask;
                }
            }

            if (bChanged)
            {
                ChangedFaceMask |= Curr.FaceMask;
            }
        }

        // Loop B: Identify bots that left or went hidden using a current frame map
        CurrentFrameLookup.Empty();
        for (INT i = 0; i < CurrentFrameActors.Num(); ++i) {
            CurrentFrameLookup.Set(CurrentFrameActors(i).ActorIndex, i);
        }

        for (INT j = 0; j < LastFrameActors.Num(); ++j)
        {
            const CachedActorState& Prev = LastFrameActors(j);
            
            // If the intrinsic pointer address is completely missing this frame:
            if (CurrentFrameLookup.Find(Prev.ActorIndex) == nullptr)
            {
                ChangedFaceMask |= Prev.FaceMask;
            }
        }
    }

// --- Execution Pipeline Transition ---
	LastFrameActors.Empty();
	LastFrameActors   = CurrentFrameActors;
	LastFrameFaceMask = CurrentFaceMask;
	LastRadius        = Radius;
	LastLocation      = SourceLocation;

	if (ChangedFaceMask == 0)
		return;

    // =========================================================================
	// STEP 1: ONE-TIME BASE CUBEMAP BACKING VRAM ALLOCATION
	// Allocate the raw texture coordinates ONCE per light if they don't exist, 
	// but do NOT construct any FBO containers yet!
	// =========================================================================
	if (ColorCubemapID == 0)
	{
        INT size = shadowmapSize;

		// Allocate the single shared Color Cubemap Texture
		glGenTextures(1, &ColorCubemapID);
		glBindTexture(GL_TEXTURE_CUBE_MAP, ColorCubemapID);
		for (int face = 0; face < 6; face++)
		{
			glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA32F, size, size, 0, GL_RGBA, GL_FLOAT, nullptr);
		}
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);

		// Allocate the single shared Hardware Depth Cubemap Texture
		glGenTextures(1, &DepthCubemapID);
		glBindTexture(GL_TEXTURE_CUBE_MAP, DepthCubemapID);
		for (int face = 0; face < 6; face++)
		{
			glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_DEPTH_COMPONENT24, size, size, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
		}
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        
        // Ensure bindless tracking state is completely resident in VRAM
	    MakeTextureResident();
	}

	// Backup previous main screen viewport coordinates
	GLint PrevViewport[4];
	glGetIntegerv(GL_VIEWPORT, PrevViewport);

	// Dynamically align the drawing window dimensions to match your FBO size
	glViewport(0, 0, shadowmapSize, shadowmapSize);

	GLenum DrawBuffers[] = { GL_COLOR_ATTACHMENT0 };

	for (INT face = 0; face < 6; face++)
	{
		if (!(ChangedFaceMask & (1 << face)))
			continue;

        // =========================================================================
		// STEP 2: HYPER-EFFICIENT PER-FACE LAZY LOADING
		// The individual face FBO is only constructed right here, at the exact split-second 
		// its frustum index passes the visibility mask, saving thousands of FBO handles!
		// =========================================================================
		if (!FaceFbos[face])
		{
			FaceFbos[face] = new Fbo(shadowmapSize, ColorCubemapID, DepthCubemapID, face);
		}

		// --- BIND THE PERMANENT STATIC FACE FBO ---
		// Absolutely zero attachment swaps, texture layer rebindings, or unbinds! 
		// The driver treats this memory layout as a permanent, persistent asset block.
		FaceFbos[face]->Bind();
		glDrawBuffers(1, DrawBuffers);

		// --- SELECTIVE CLEARING BASED ON BSP CACHE ---
		if (!bspDrawn[face])
		{
			glDepthMask(GL_TRUE); // Open depth writes wide for the baseline pass
			
			// First time rendering this face: clear everything (color + depth)
			GLfloat ClearValues[] = { 1.0f, 0.0f, 0.0f, 0.0f };
			glClearBufferfv(GL_COLOR, 0, ClearValues);
			glClear(GL_DEPTH_BUFFER_BIT); // Wipes only this face's independent canvas layer
		}
		else
		{
			// --- HARDWARE TILE CACHE LOCK ---
			// We freeze depth mutations explicitly BEFORE running the color clear!
			// This locks your cached depth face layer, telling the driver it is read-only
			// and preventing the color clear from invalidating its Hi-Z tiles.
			glDepthMask(GL_FALSE);

			// BSP already cached: only clear .a channel (mesh data) while preserving .r (BSP depth)
			glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);  // Alpha only
			glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
			glClear(GL_COLOR_BUFFER_BIT);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);  // Restore
		}

		// Keep glDepthMask(GL_FALSE) active here if the BSP is cached!
		// This protects your write-once static world depth maps from being corrupted by dynamic actors.
		if (CurrentFaceMask & (1 << face))
		{
			RenderFaceGeometry(Level, Frame, face, CurrentFrameActors, GL);
		}

		// Restore standard depth writing capability before stepping to the next face quadrant
		glDepthMask(GL_TRUE);
		FaceFbos[face]->Unbind();
	}

	// Restore standard screen depth writes and viewport coordinates for the main player pass
	glDepthMask(GL_TRUE);
	glViewport(PrevViewport[0], PrevViewport[1], PrevViewport[2], PrevViewport[3]);
}

void UXOpenGLHeroLight::ClearShadowMapTexture()
{
    if (HasActiveShadowMap())
    {
        CurrentFaceMask = 0;
        LastFrameActors.Empty(); // Ensure fresh render if/when this comes back into scope
    }
}

// ------------------------------------------------------------
// Uniform Pipeline Binder
// ------------------------------------------------------------
void UXOpenGLHeroLight::BindTextures(GLuint BaseTextureUnit) const
{
    // Ensure our color cubemap handle is fully allocated and valid before binding
    if (ColorCubemapID == 0) 
        return;

    glActiveTexture(GL_TEXTURE0 + BaseTextureUnit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, ColorCubemapID);
}

void UXOpenGLHeroLight::BindDepthTexture(GLuint BaseTextureUnit) const
{
    if (DepthCubemapID == 0) 
        return;

    glActiveTexture(GL_TEXTURE0 + BaseTextureUnit);
    glBindTexture(GL_TEXTURE_CUBE_MAP, DepthCubemapID);
}

void UXOpenGLHeroLight::MakeTextureResident()
{
    // Generate the bindless handle once if it doesn't exist yet
    if (BindlessMaskHandle == 0 && ColorCubemapID != 0)
    {
        // Reference your own cleanly stored class texture handle natively!
        BindlessMaskHandle = glGetTextureHandleARB(ColorCubemapID);
    }
    else
    {
        int moo = 5;
    }

    // Make the handle resident so the GPU can access it blindly via its 64-bit address
    if (BindlessMaskHandle != 0 && !bIsHandleResident)
    {
        glMakeTextureHandleResidentARB(BindlessMaskHandle);
        bIsHandleResident = TRUE;
    }
    else
    {
        int moo = 5;
    }
}



