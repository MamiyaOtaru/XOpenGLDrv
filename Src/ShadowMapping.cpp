#include "XOpenGLDrv.h"
#include "XOpenGL.h"

struct HeroLightScore
{
    ALight* Light;
    float   BaseScore;
    float   CurrentScore; // Changes dynamically during selection
};

INT Compare(const HeroLightScore& A, const HeroLightScore& B)
{
    // descending order
    if (A.CurrentScore < B.CurrentScore) return +1;
    if (A.CurrentScore > B.CurrentScore) return -1;
    return 0;
}

// ------------------------------------------------------------
// Compute luminance from HSV
// ------------------------------------------------------------
static float ComputeLuminance(const FPlane& RGB)
{
    float r = Clamp(RGB.X / 255.0f, 0.0f, 1.0f);
    float g = Clamp(RGB.Y / 255.0f, 0.0f, 1.0f);
    float b = Clamp(RGB.Z / 255.0f, 0.0f, 1.0f);

    return 0.299f * r + 0.587f * g + 0.114f * b;
}

// ------------------------------------------------------------
// Compute brightnessFactor = max(luminance, LightBrightness/255)
// ------------------------------------------------------------
static float ComputeBrightnessFactor(ALight* L)
{
    float brightness = L->LightBrightness / 255.0f;

    FPlane RGB = FGetHSV(
        L->LightHue,
        L->LightSaturation,
        L->LightBrightness
    );

    float lum = ComputeLuminance(RGB);
    return Max(lum, brightness);
}

static bool IsPointVisibleFromLight(UModel* Model, const FVector& LightLoc, const FVector& TargetPoint)
{
    // Apply -50.f to Y because -Y is Up (and +Y is Down) in this framework!
    // Keeping X and Z flat ensures the ray stays perfectly centered horizontally.
    FVector AdjustedTarget = TargetPoint + FVector(0.f, -50.f, 0.f);

    // Pass -1 to signal a generic free-space visibility ray test
    return UXOpenGLRenderDevice::BSPVisibilityRay(Model, -1, AdjustedTarget, LightLoc);
}

// ------------------------------------------------------------
// Spatial Relevance utilizing linear attenuation and line-of-sight
// ------------------------------------------------------------
static float ComputeSpatialRelevance(UModel* Model, ALight* L, const TArray<FVector>& NavPoints)
{
    float Radius = L->WorldLightRadius();
    if (Radius <= 0.001f) return 0.0f;

    float TotalIllumination = 0.0f;

    for (INT i = 0; i < NavPoints.Num(); ++i)
    {
        const FVector& P = NavPoints(i);
        float Dist = (L->Location - P).Size();

        if (Dist < Radius)
        {
            // First check line-of-sight to ensure it isn't bleeding through solid BSP walls
            if (!IsPointVisibleFromLight(Model, L->Location, P))
                continue;

            // Linear attenuation matching the rendering pipeline
            float Attenuation = 1.0f - (Dist / Radius);
            TotalIllumination += Attenuation;
        }
    }

    // Completely disqualify lights that illuminate 0 play area nodes
    if (TotalIllumination <= 0.0f) return 0.0f;

    // Return a multiplier based on visible room density (capped smoothly)
    return Min(1.0f + (TotalIllumination * 0.15f), 4.0f);
}

// ------------------------------------------------------------
// Compute hero-light score
// ------------------------------------------------------------
static float ComputeHeroBaseScore(UModel* Model, ALight* L, const TArray<FVector>& NavPoints)
{
    float Radius = L->WorldLightRadius();
    
    // Calculate Perceptual Luminance
    float Brightness = L->LightBrightness / 255.0f;
    FPlane RGB = FGetHSV(L->LightHue, L->LightSaturation, L->LightBrightness);
    float Lum = 0.299f * Clamp(RGB.X/255.0f, 0.f, 1.f) + 
                0.587f * Clamp(RGB.Y/255.0f, 0.f, 1.f) + 
                0.114f * Clamp(RGB.Z/255.0f, 0.f, 1.f);
    float BrightnessFactor = Max(Lum, Brightness);

    float Relevance = ComputeSpatialRelevance(Model, L, NavPoints);
    if (Relevance <= 0.0f) return 0.0f;

    // Radius squared gives geometric weight to large area coverage
    return (Radius * Radius) * BrightnessFactor * Relevance;
}

// ------------------------------------------------------------
// Main hero-light selection
// ------------------------------------------------------------
void UXOpenGLRenderDevice::PickHeroLights(
    ULevel* Level,
    const TArray<AActor*>& StaticLevelLights,
    TArray<ALight*>& OutHeroLights,
    INT DesiredCount)
{
    TArray<HeroLightScore> Candidates;
   
    TArray<FVector> NavPoints;
    for (INT i = 0; i < Level->Actors.Num(); ++i)
    {
        AActor* A = Level->Actors(i);
        if (!A) continue;

        if (A->IsA(ANavigationPoint::StaticClass()))// || A->IsA(AInventory::StaticClass()))
        {
            NavPoints.AddItem(A->Location);
        }
    }

    // Filter and score candidates initially
    for (INT i = 0; i < StaticLevelLights.Num(); ++i)
    {
        ALight* L = Cast<ALight>(StaticLevelLights(i));
        if (!L) continue;

        if (L->WorldLightRadius() < 128.0f) continue; // Raised slightly to ignore tiny trim lights
        if (L->LightBrightness < 32) continue;
        if (L->bSpecialLit) continue;

        float BaseScore = ComputeHeroBaseScore(Level->Model, L, NavPoints);
        if (BaseScore <= 0.0f) continue; // Disqualified

        HeroLightScore HLS;
        HLS.Light = L;
        HLS.BaseScore = BaseScore;
        HLS.CurrentScore = BaseScore;
        Candidates.AddItem(HLS);
    }

    // Sort by base score descending for the initial pass
    if (Candidates.Num() > 1)
        Sort(&Candidates(0), Candidates.Num());

    // Adaptive Greedy Selection Loop
    while (OutHeroLights.Num() < DesiredCount && Candidates.Num() > 0)
    {
        // Re-sort the remaining pool based on their dynamically adjusted scores
        if (Candidates.Num() > 1)
            Sort(&Candidates(0), Candidates.Num());

        // Top candidate wins this round
        HeroLightScore Best = Candidates(0);
        
        // If the best remaining light has no score left, we stop early
        if (Best.CurrentScore <= 0.001f) break;

        // Add to our chosen hero lights list
        OutHeroLights.AddItem(Best.Light);
        Candidates.Remove(0); // Remove the picked light from the pool

        // Apply smooth distance penalties to all remaining candidates
        FVector BestLoc = Best.Light->Location;
        float BestRadius = Best.Light->WorldLightRadius();

        for (INT i = 0; i < Candidates.Num(); ++i)
        {
            ALight* Target = Candidates(i).Light;
            float Dist = (Target->Location - BestLoc).Size();
            
            // Interaction zone based on typical room scale (approx 2000 units)
            float InfluenceRadius = Max(2000.0f, BestRadius + Target->WorldLightRadius());

            /*
            // this fully eliminates lights from contention
            // could be good for processing speed if we recalculate this per frame or something
            // but for now it is a one time thing on level load
            if (Dist < InfluenceRadius)
            {
                // Smooth Gaussian decay curve
                // Close lights get heavily penalized (~0), distant lights stay intact (~1)
                float Factor = Dist / InfluenceRadius;
                float Penalty = Factor * Factor; 

                Candidates(i).CurrentScore *= Penalty;
            }*/

            if (Dist < InfluenceRadius)
            {
                // --- SUBTRACTIVE GRANULAR PENALTY ---
                // Linear penalty factor: 1.0 at overlapping center, sliding to 0.0 at the boundary edge
                float Factor = 1.0f - (Dist / InfluenceRadius);
                
                // We penalize the CURRENT score by reducing it by a fraction of its own BASE score.
                // The max penalty is capped at 75%, ensuring a 25% minimum score floor remains.
                // This pushes crowded lights down the sorting queue without ever erasing them completely!
                float Penalty = Candidates(i).BaseScore * Factor * 0.75f;
                
                Candidates(i).CurrentScore = Max(0.005f, Candidates(i).CurrentScore - Penalty);
            }
        }
    }
} // end function PickHeroLights

// splatting stuff, uses global cache for pose data and per frame position data
// stored, cachable capsule info (connectivity)
struct BoneCapsuleInfo
{
	float radius;   // cached once
	int v0;         // vertex index for endpoint 0
	int v1;         // vertex index for endpoint 1
};
struct MeshCapsuleCache
{
	TArray<int> UsedVerts;                 // unique vertex indices used by any capsule
	TArray<BoneCapsuleInfo> Capsules;      // all capsules for this mesh
};
// map meshes to stored connectivity data
static TMap<ULodMesh*, MeshCapsuleCache> CapsuleCache;
void GenerateCapsulesForMesh(ULodMesh* L, MeshCapsuleCache& Out)
{
    Out.UsedVerts.Empty();
    Out.Capsules.Empty();

    if (!L || L->Verts.Num() == 0)
        return;

    const INT FrameVerts = (L->FrameVerts > 0) ? L->FrameVerts : L->ModelVerts;
    if (FrameVerts <= 1)
        return;

    // 1. Extract base (frame 0) vertex positions in *scaled mesh space*
    TArray<FVector> BaseVerts;
    BaseVerts.AddZeroed(FrameVerts);

    for (INT i = 0; i < FrameVerts; i++)
    {
        FVector P = L->Verts(i).Vector();

        // Apply mesh scale (NOT actor scale)
        P.X *= L->Scale.X;
        P.Y *= L->Scale.Y;
        P.Z *= L->Scale.Z;

        BaseVerts(i) = P;
    }

    const FLOAT SpatialThresholdSq = 30.0f * 30.0;  // tweakable
    const FLOAT MinLength = 3.0f;

    // Manual dedupe arrays (UE1 has no TSet)
    TArray<INT> PairA;
    TArray<INT> PairB;

    // 2. Build raw capsule pairs
    for (INT i = 0; i < FrameVerts; i++)
    {
        const FVector& A = BaseVerts(i);

        // Sequential neighbor
        INT best = (i + 1 < FrameVerts) ? (i + 1) : (i - 1);

        FLOAT d2 = (BaseVerts(best) - A).SizeSquared();
        if (d2 > SpatialThresholdSq)
            best = -1;

        // Fallback: nearest vertex
        /*if (best == -1)
        {
            FLOAT bestDistSq = 999999999.0f;
            for (INT j = 0; j < FrameVerts; j++)
            {
                if (j == i) continue;

                FLOAT d2b = (BaseVerts(j) - A).SizeSquared();
                if (d2b < bestDistSq)
                {
                    bestDistSq = d2b;
                    best = j;
                }
            }
            if (bestDistSq > SpatialThresholdSq * 1.1f)
                best = -1;
        }*/

        if (best < 0)
            continue;

        const FVector& B = BaseVerts(best);
        FLOAT length = (B - A).Size();
        if (length < MinLength)
            continue;

        // Canonicalize pair
        INT a = i;
        INT b = best;
        if (a > b)
        {
            INT t = a;
            a = b;
            b = t;
        }

        // Check if pair already exists
        UBOOL bExists = 0;
        for (INT k = 0; k < PairA.Num(); k++)
        {
            if (PairA(k) == a && PairB(k) == b)
            {
                bExists = 1;
                break;
            }
        }
        if (bExists)
            continue;

        // Add new pair
        PairA.AddItem(a);
        PairB.AddItem(b);

        // Store capsule (radius in scaled mesh space)
        BoneCapsuleInfo C;
        C.v0 = a;
        C.v1 = b;
        C.radius = Max(3.0f, Min(5.0f, 0.25f * length));

        Out.Capsules.AddItem(C);
    }

    // 3. Build UsedVerts list (UE1-compatible)
    TArray<INT> Unique;

    for (INT i = 0; i < Out.Capsules.Num(); i++)
    {
        BoneCapsuleInfo& C = Out.Capsules(i);

        // Add v0 if not present
        UBOOL found = 0;
        for (INT j = 0; j < Unique.Num(); j++)
            if (Unique(j) == C.v0) { found = 1; break; }
        if (!found)
            Unique.AddItem(C.v0);

        // Add v1 if not present
        found = 0;
        for (INT j = 0; j < Unique.Num(); j++)
            if (Unique(j) == C.v1) { found = 1; break; }
        if (!found)
            Unique.AddItem(C.v1);
    }

    // Manual bubble sort (UE1 has no Sort())
    for (INT i = 0; i < Unique.Num(); i++)
    {
        for (INT j = i + 1; j < Unique.Num(); j++)
        {
            if (Unique(j) < Unique(i))
            {
                INT t = Unique(i);
                Unique(i) = Unique(j);
                Unique(j) = t;
            }
        }
    }

    Out.UsedVerts = Unique;
}

static MeshCapsuleCache& GetCapsuleCacheForMesh(ULodMesh* Mesh)
{
    MeshCapsuleCache* Found = CapsuleCache.Find(Mesh);
    if (Found)
        return *Found;

    // Not found -> generate
    MeshCapsuleCache NewCache;
    GenerateCapsulesForMesh(Mesh, NewCache);

    // Store and return
    CapsuleCache.Set(Mesh, NewCache);
    return NewCache;
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

// helper: mesh-space -> world-space (no animation here)
static FVector TransformMeshSpaceToWorld(const FVector& P, ULodMesh* L, AActor* Actor)
{
    FVector S = P;
    S.X *= L->Scale.X;
    S.Y *= L->Scale.Y;
    S.Z *= L->Scale.Z;

    FVector MX, MY, MZ;
    GetAxes(L->RotOrigin, MX, MY, MZ);

    FVector R;
    R.X = S.X * MX.X + S.Y * MY.X + S.Z * MZ.X;
    R.Y = S.X * MX.Y + S.Y * MY.Y + S.Z * MZ.Y;
    R.Z = S.X * MX.Z + S.Y * MY.Z + S.Z * MZ.Z;

    R *= Actor->DrawScale;

    FVector AX, AY, AZ;
    GetAxes(Actor->Rotation, AX, AY, AZ);

    FVector W;
    W.X = R.X * AX.X + R.Y * AY.X + R.Z * AZ.X;
    W.Y = R.X * AX.Y + R.Y * AY.Y + R.Z * AZ.Y;
    W.Z = R.X * AX.Z + R.Y * AY.Z + R.Z * AZ.Z;

    return W + Actor->Location + Actor->PrePivot;
}

void UXOpenGLRenderDevice::ExtractLodMeshCapsules(ULodMesh* L, AActor* Actor, TArray<FCapsuleSplat>& OutCapsules)
{
    if (!L || !Actor || L->Faces.Num() == 0 || L->Verts.Num() == 0)
        return;

    MeshCapsuleCache& Cache = GetCapsuleCacheForMesh(L);
    if (Cache.Capsules.Num() == 0 || Cache.UsedVerts.Num() == 0)
        return;

    const INT MemoryStride = (L->FrameVerts > 0) ? L->FrameVerts : L->ModelVerts;

    // --- animation sampling setup (once per frame) ---

    FMeshAnimSeq* Seq = L->GetAnimSeq(Actor->AnimSequence);
    UBOOL bStaticMesh = (Seq == nullptr);

    INT FrameA_Index = 0, FrameB_Index = 0;
    FLOAT Alpha = 0.0f;

    if (!bStaticMesh && Actor->AnimFrame >= 0.0f)
    {
        if (Actor->AnimFrame < 1.0f)
        {
            FLOAT FloatFrame = Seq->StartFrame + (Actor->AnimFrame * (FLOAT)Seq->NumFrames);
            FrameA_Index = appFloor(FloatFrame);
            FrameB_Index = FrameA_Index + 1;
            Alpha = FloatFrame - (FLOAT)FrameA_Index;
        }
        else
        {
            FrameA_Index = Seq->StartFrame + appFloor(Actor->AnimFrame);
            FrameB_Index = FrameA_Index + 1;
            Alpha = Actor->AnimFrame - appFloor(Actor->AnimFrame);
        }

        INT MaxSeqFrame = Seq->StartFrame + Seq->NumFrames - 1;
        if (FrameA_Index > MaxSeqFrame)  FrameA_Index = MaxSeqFrame;
        if (FrameB_Index > MaxSeqFrame)  FrameB_Index = Seq->StartFrame;
    }
    else if (!bStaticMesh)
    {
        FrameA_Index = Seq->StartFrame;
        FrameB_Index = Seq->StartFrame;
        Alpha = 0.0f;
    }

    INT FrameA_Offset = FrameA_Index * MemoryStride;
    INT FrameB_Offset = FrameB_Index * MemoryStride;

    const INT UsedCount = Cache.UsedVerts.Num();

    // vert index -> local index into WorldVerts
    TArray<INT> VertToLocal;
    const INT FrameVerts = (L->FrameVerts > 0) ? L->FrameVerts : L->ModelVerts;
    VertToLocal.AddZeroed(FrameVerts);
    for (INT i = 0; i < L->FrameVerts; ++i)
        VertToLocal(i) = -1;

    TArray<FVector> WorldVerts;
    WorldVerts.AddZeroed(UsedCount);

    // --- interpolate only used verts, then transform to world ---

    for (INT i = 0; i < UsedCount; ++i)
    {
        const INT v = Cache.UsedVerts(i);
        VertToLocal(v) = i;

        const INT VertA_Addr = FrameA_Offset + v;
        const INT VertB_Addr = FrameB_Offset + v;

        FVector P(0,0,0);

        if (VertA_Addr < L->Verts.Num() && VertB_Addr < L->Verts.Num())
        {
            const FVector VA = L->Verts(VertA_Addr).Vector();
            const FVector VB = L->Verts(VertB_Addr).Vector();
            P = VA + (VB - VA) * Alpha;
        }

        WorldVerts(i) = TransformMeshSpaceToWorld(P, L, Actor);
    }

    // --- build final splats from cached capsules + per-frame world verts ---

    OutCapsules.Empty();
    OutCapsules.Reserve(Cache.Capsules.Num());

    for (INT ci = 0; ci < Cache.Capsules.Num(); ++ci)
    {
        const BoneCapsuleInfo& CInfo = Cache.Capsules(ci);

        const INT i0 = (CInfo.v0 >= 0 && CInfo.v0 < VertToLocal.Num()) ? VertToLocal(CInfo.v0) : -1;
        const INT i1 = (CInfo.v1 >= 0 && CInfo.v1 < VertToLocal.Num()) ? VertToLocal(CInfo.v1) : -1;
        if (i0 < 0 || i1 < 0)
            continue;

        FCapsuleSplat S;
        S.P0     = WorldVerts(i0);
        S.P1     = WorldVerts(i1);
        S.Radius = CInfo.radius;

        OutCapsules.AddItem(S);
    }
}

void UXOpenGLRenderDevice::ExtractLodMeshTriangles(ULodMesh* L, AActor* Actor, TArray<FShadowTriangle>& OutTris)
{
    if (!L || !Actor || L->Faces.Num() == 0 || L->Verts.Num() == 0) return;

    // type assignment for debugging
    //const TCHAR* NativeClassName = Actor->GetClass()->GetName();

    // classification via fname target checks
    UBOOL bIsProjectile = Actor->IsA(AProjectile::StaticClass());
    UBOOL bIsRocket = Actor->GetClass()->GetFName() == FName(TEXT("RocketMk2")) || 
                      Actor->GetClass()->GetFName() == FName(TEXT("UT_Grenade"));
                      
    UBOOL bIsPulseBeam = Actor->GetClass()->GetFName() == FName(TEXT("StarterBolt")) || 
                         Actor->GetClass()->GetFName() == FName(TEXT("PBolt"));

    // drop the pulse beam assets out of scope immediately
    if (bIsPulseBeam)
    {
        return; 
    }

    const INT MemoryStride = (L->FrameVerts > 0) ? L->FrameVerts : L->ModelVerts;
    const INT DrawVerts    = L->ModelVerts;
    
    TArray<FVector> PosedVerts;
    PosedVerts.AddZeroed(MemoryStride);

    // timeline animation tracker or base static override
    FMeshAnimSeq* Seq = L->GetAnimSeq(Actor->AnimSequence);
    UBOOL bStaticMesh = (Seq == nullptr);

    INT FrameA_Index = 0, FrameB_Index = 0;
    FLOAT Alpha = 0.0f;

    if (!bStaticMesh && Actor->AnimFrame >= 0.0f)
    {
        if (Actor->AnimFrame < 1.0f)
        {
            FLOAT FloatFrame = Seq->StartFrame + (Actor->AnimFrame * (FLOAT)Seq->NumFrames);
            FrameA_Index = appFloor(FloatFrame);
            FrameB_Index = FrameA_Index + 1;
            Alpha = FloatFrame - (FLOAT)FrameA_Index;
        }
        else
        {
            FrameA_Index = Seq->StartFrame + appFloor(Actor->AnimFrame);
            FrameB_Index = FrameA_Index + 1;
            Alpha = Actor->AnimFrame - appFloor(Actor->AnimFrame);
        }
        INT MaxSeqFrame = Seq->StartFrame + Seq->NumFrames - 1;
        if (FrameA_Index > MaxSeqFrame)  FrameA_Index = MaxSeqFrame;
        if (FrameB_Index > MaxSeqFrame)  FrameB_Index = Seq->StartFrame; 
    }
    else if (!bStaticMesh)
    {
        FrameA_Index = Seq->StartFrame;
        FrameB_Index = Seq->StartFrame;
        Alpha = 0.0f;
    }

    INT FrameA_Offset = FrameA_Index * MemoryStride;
    INT FrameB_Offset = FrameB_Index * MemoryStride;

    // passive vertex decompression
    for (INT i = 0; i < MemoryStride; i++)
    {
        INT VertA_Addr = FrameA_Offset + i;
        INT VertB_Addr = FrameB_Offset + i;

        if (VertA_Addr >= L->Verts.Num() || VertB_Addr >= L->Verts.Num())
            continue;

        PosedVerts(i) = L->Verts(VertA_Addr).Vector() + 
                       (L->Verts(VertB_Addr).Vector() - L->Verts(VertA_Addr).Vector()) * Alpha;
    }

    // spacial transform matrix conversions
    FRotator ImportRotation = L->RotOrigin;

    FVector MX, MY, MZ;
    GetAxes(ImportRotation, MX, MY, MZ);

    FVector AX, AY, AZ;
    GetAxes(Actor->Rotation, AX, AY, AZ);

    for (INT i = 0; i < MemoryStride; i++)
    {
        FVector P = PosedVerts(i);

        // Scale raw coordinates uniformly on their native axes
        P.X *= L->Scale.X;
        P.Y *= L->Scale.Y;
        P.Z *= L->Scale.Z;

        FVector RotatedP;

        // Clean streamlined geometry filters isolation mapping
        /*if (bStaticMesh && !bIsProjectile)
        {
            // static decoration and pickup path
            P -= L->Origin;
        }*/
        RotatedP.X = (P.X * MX.X) + (P.Y * MY.X) + (P.Z * MZ.X);
        RotatedP.Y = (P.X * MX.Y) + (P.Y * MY.Y) + (P.Z * MZ.Y);
        RotatedP.Z = (P.X * MX.Z) + (P.Y * MY.Z) + (P.Z * MZ.Z);

        // Apply Global Actor DrawScale
        RotatedP *= Actor->DrawScale;

        // forward world rotation pass
        FVector WorldP;
        WorldP.X = (RotatedP.X * AX.X) + (RotatedP.Y * AY.X) + (RotatedP.Z * AZ.X);
        WorldP.Y = (RotatedP.X * AX.Y) + (RotatedP.Y * AY.Y) + (RotatedP.Z * AZ.Y);
        WorldP.Z = (RotatedP.X * AX.Z) + (RotatedP.Y * AY.Z) + (RotatedP.Z * AZ.Z);

        WorldP += Actor->Location;
        PosedVerts(i) = WorldP;
    }

    // pack out final world space shadow map triangles
    for (INT ti = 0; ti < L->Faces.Num(); ti++)
    {
        const FMeshFace& Face = L->Faces(ti);

        INT v0 = L->Wedges(Face.iWedge[0]).iVertex;
        INT v1 = L->Wedges(Face.iWedge[1]).iVertex;
        INT v2 = L->Wedges(Face.iWedge[2]).iVertex;
        //INT v0 = ResolveLodMeshVertex(L, Face.iWedge[0], 1);
        //INT v1 = ResolveLodMeshVertex(L, Face.iWedge[1], 1);
        //INT v2 = ResolveLodMeshVertex(L, Face.iWedge[2], 1);

        if (v0 >= DrawVerts || v1 >= DrawVerts || v2 >= DrawVerts)
            continue;

        if ((PosedVerts(v0) - PosedVerts(v1)).IsNearlyZero() || 
            (PosedVerts(v1) - PosedVerts(v2)).IsNearlyZero())
            continue;

        FShadowTriangle T;
        T.V0 = PosedVerts(v0);
        T.V1 = PosedVerts(v1);
        T.V2 = PosedVerts(v2);

        OutTris.AddItem(T);
    }
}

void UXOpenGLRenderDevice::ExtractSkeletalMeshTriangles(USkeletalMesh* S, AActor* Actor, TArray<FShadowTriangle>& OutTris)
{
    if (!S || !Actor || S->Faces.Num() == 0) return;

    const INT TotalVerts = S->Points.Num();
    if (TotalVerts == 0) return;

    TArray<FVector> PosedVerts;
    PosedVerts.AddZeroed(TotalVerts);

    // Save state variables
    FLOAT SavedAnimFrame    = Actor->AnimFrame;
    FName  SavedAnimSequence = Actor->AnimSequence;

    INT LODRequest = 0;

    // Evaluates directly using v469 skeletal transformations
    S->GetFrame(&PosedVerts(0), sizeof(FVector), GMath.UnitCoords, Actor, LODRequest);

    // Restore state variables safely
    Actor->AnimFrame    = SavedAnimFrame;
    Actor->AnimSequence = SavedAnimSequence;

    // Convert local bone layout vertex maps straight to global matrices
    FVector AX, AY, AZ;
    GetAxes(Actor->Rotation, AX, AY, AZ);
    FCoords ActorCoords(Actor->Location, AX, AY, AZ);

    for (INT i = 0; i < TotalVerts; i++)
    {
        FVector P = PosedVerts(i);

        if (P.IsZero() && i < S->Points.Num())
        {
            P = S->Points(i);
        }

        P *= S->Scale;
        P *= Actor->DrawScale;
        P = P.TransformPointBy(ActorCoords);

        PosedVerts(i) = P;
    }

    for (INT ti = 0; ti < S->Faces.Num(); ti++)
    {
        const FMeshFace& Face = S->Faces(ti);

        INT v0 = S->Wedges(Face.iWedge[0]).iVertex;
        INT v1 = S->Wedges(Face.iWedge[1]).iVertex;
        INT v2 = S->Wedges(Face.iWedge[2]).iVertex;

        if (v0 >= TotalVerts || v1 >= TotalVerts || v2 >= TotalVerts)
            continue;

        if ((PosedVerts(v0) - PosedVerts(v1)).IsNearlyZero() || 
            (PosedVerts(v1) - PosedVerts(v2)).IsNearlyZero())
            continue;

        FShadowTriangle T;
        T.V0 = PosedVerts(v0);
        T.V1 = PosedVerts(v1);
        T.V2 = PosedVerts(v2);

        OutTris.AddItem(T);
    }
}

void UXOpenGLRenderDevice::ExtractUMeshTriangles(UMesh* M, AActor* Actor, TArray<FShadowTriangle>& OutTris)
{
    TArray<FVector> PosedVerts;
    PosedVerts.AddZeroed(M->Verts.Num());

    M->GetFrame(&PosedVerts(0), sizeof(FVector), GMath.UnitCoords, Actor);

    for (INT ti = 0; ti < M->Tris.Num(); ti++)
    {
        const FMeshTri& Tri = M->Tris(ti);

        FShadowTriangle T;
        T.V0 = Actor->Location + PosedVerts(Tri.iVertex[0]);
        T.V1 = Actor->Location + PosedVerts(Tri.iVertex[1]);
        T.V2 = Actor->Location + PosedVerts(Tri.iVertex[2]);

        OutTris.AddItem(T);
    }
}

// game loop
INT    UserGuaranteed        = 30;   // user preference for number of guaranteed lights
INT    Guaranteed            = UserGuaranteed; // Dynamically scales between a user-configured floor and a hardware ceiling
INT    ActivePoolSize        = Guaranteed * 3;   // Mathematically locked to UserGuaranteed * 2
INT    GSuccessFramesCounter = 0;   // Counts consecutive frames with clean headroom
INT    GFailureFramesCounter = 0;   // Counts consecutive frames running on a tight budget
INT    GRoundRobinCurrentIndex = 0;  // Sliding window pointer
LARGE_INTEGER GLastFrameEndTimestamp = {0};
double GOtherStuffDurationMS = 0.0;

void UXOpenGLRenderDevice::DrawShadowMaps(FSceneNode* Frame)
{
    PerFrameActorSplatCache.Empty();
    PerFrameStaticMeshCache.Empty();

    INT TotalLights = HeroLights.Num();
    if (TotalLights <= 0) return;

    Guaranteed = Clamp(Guaranteed, UserGuaranteed, TotalLights);
    
    // THE 2-FRAME REFRESH RULE: The total pool is locked to exactly treble the guaranteed size.
    // This mathematically guarantees that the round-robin remainder queue completely sweeps every 2 frames!
    ActivePoolSize = Guaranteed * 3;
    ActivePoolSize = Min(ActivePoolSize, TotalLights);

    INT HighPriorityCount = Min(Guaranteed, ActivePoolSize);
    INT RemainderCount    = ActivePoolSize - HighPriorityCount;
    INT RoundRobinCount   = Min(Guaranteed, RemainderCount);

    // Core Hardware Pass State Overrides
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquationSeparate(GL_MIN, GL_MAX);
    glBlendFunc(GL_ONE, GL_ONE);
    glDisable(GL_CULL_FACE);

    // ========================================================
    // PHASE 1: THE UNCONDITIONAL HIGH-PRIORITY PASS
    // ========================================================
    for (INT i = 0; i < HighPriorityCount; ++i)
    {
        if (HeroLights(i)) HeroLights(i)->UpdateShadowMap(Frame, this);
    }

    // ========================================================
    // PHASE 2: THE UNCONDITIONAL SLIDING ROUND-ROBIN PASS
    // ========================================================
    INT RoundRobinTicked = 0;

    if (RemainderCount > 0)
    {
        // Sanity Check: Keep the cursor pointer within valid bounds
        if (GRoundRobinCurrentIndex < HighPriorityCount || GRoundRobinCurrentIndex >= ActivePoolSize)
        {
            GRoundRobinCurrentIndex = HighPriorityCount;
        }

        while (RoundRobinTicked < RoundRobinCount)
        {
            if (HeroLights(GRoundRobinCurrentIndex))
            {
                HeroLights(GRoundRobinCurrentIndex)->UpdateShadowMap(Frame, this);
            }

            RoundRobinTicked++;
            GRoundRobinCurrentIndex++;

            if (GRoundRobinCurrentIndex >= ActivePoolSize)
            {
                GRoundRobinCurrentIndex = HighPriorityCount;
            }
        }
    }

    // ========================================================
    // PHASE 3: THE HEADROOM SENSOR & PREDICTIVE SCALING
    // ========================================================
    // We only execute performance timers and trend scaling on large maps that exceed our minimum floor
    if (TotalLights > (UserGuaranteed * 3))
    {
        LARGE_INTEGER Frequency, StartTime, CurrentTime;
        QueryPerformanceFrequency(&Frequency);
        QueryPerformanceCounter(&StartTime);

        // Exponential Moving Average Hysteresis Calculation
        if (GLastFrameEndTimestamp.QuadPart > 0)
        {
            double ImmediateOtherStuffMS = (double)(StartTime.QuadPart - GLastFrameEndTimestamp.QuadPart) / Frequency.QuadPart * 1000.0;
            const double Alpha = 0.15;
            GOtherStuffDurationMS = (Alpha * ImmediateOtherStuffMS) + ((1.0 - Alpha) * GOtherStuffDurationMS);
        }
        else
        {
            GOtherStuffDurationMS = 8.0;
        }

        // Measure our dynamic frame budget headroom
        double DynamicBudgetMS = Min(5.0, Max(0.0, 16.6 - GOtherStuffDurationMS));

        if (DynamicBudgetMS > 1.5)
        {
            // SUCCESS TREND: The current frame overhead is low and we have spare time!
            GFailureFramesCounter = 0;
            GSuccessFramesCounter++;

            // If we maintain a clean run for 4 consecutive frames, gradually expand our base capacities
            if (GSuccessFramesCounter >= 4)
            {
                Guaranteed = Min(Guaranteed + 5, TotalLights);
                GSuccessFramesCounter = 0;
                //debugf(TEXT("XOpenGL: Room detected! Gradually scaling up baseline settings to %d."), Guaranteed);
            }
        }
        else if (DynamicBudgetMS <= 0.5)
        {
            // FAILURE TREND: The scene is getting heavy and performance is dipping!
            GSuccessFramesCounter = 0;
            GFailureFramesCounter++;

            // If we run on a tight budget for 2 consecutive frames, contract sizes immediately
            if (GFailureFramesCounter >= 1)
            {
                INT OldCeiling = ActivePoolSize;
                
                // Asymmetrical Step: Scale down by 10 to shed load quickly and protect the framerate
                Guaranteed = Max(Guaranteed - 15, UserGuaranteed);
                INT NewCeiling = Guaranteed * 3;
                NewCeiling = Min(NewCeiling, TotalLights);

                // --- THE GARBAGE HYGIENE HOOK ---
                // Grab the demotees that just fell out of the active pool ceiling 
                // and clear their GPU textures to prevent ghost shadows
                for (INT k = NewCeiling; k < OldCeiling; ++k)
                {
                    if (k < HeroLights.Num() && HeroLights(k))
                    {
                        HeroLights(k)->ClearShadowMapTexture();
                    }
                }

                GFailureFramesCounter = 0;
                //debugf(TEXT("XOpenGL: Starvation detected! Gradually scaling down baseline settings to %d."), Guaranteed);
            }
        }

        QueryPerformanceCounter(&GLastFrameEndTimestamp);
    }
    else
    {
        GLastFrameEndTimestamp.QuadPart = 0;
        GSuccessFramesCounter = 0;
        GFailureFramesCounter = 0;
    }

    // Restore Context Restrictions for Main Viewport Scene Painting
    glDepthFunc(GL_LEQUAL);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    SceneFbo->Bind();
    glViewport(0, 0, SceneWidth, SceneHeight);
}

