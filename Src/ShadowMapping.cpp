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
    // Keeping X and Y flat ensures the ray stays perfectly centered horizontally.
    FVector AdjustedTarget = TargetPoint + FVector(0.f, 0.f, 50.f);

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
// Attempt to exclude the bottom point light in a fake spotlight pair
// ------------------------------------------------------------
static bool IsFakeSpotlightFloorLight(
    ALight* L1,
    const TArray<AActor*>& AllLights,
    UModel* Model)
{
    // --- 1. Must be near the floor ---
    FVector Down = L1->Location + FVector(0,0,-48);
    bool HasFloor = !UXOpenGLRenderDevice::BSPVisibilityRay(Model, -1, L1->Location, Down);

    if (!HasFloor)
        return false; // not near the ground

    // --- 2. Find a vertically stacked partner above ---
    for (INT i = 0; i < AllLights.Num(); ++i)
    {
        ALight* L2 = Cast<ALight>(AllLights(i));
        if (!L2 || L2 == L1) continue;

        // Rough XY alignment.  12 is too tight for Deck
        if (Abs(L1->Location.X - L2->Location.X) > 16.f) continue;
        if (Abs(L1->Location.Y - L2->Location.Y) > 16.f) continue;

        // Must be above
        if (L2->Location.Z <= L1->Location.Z) continue;

        // --- 3. Colors must match closely ---
        FPlane C1 = FGetHSV(L1->LightHue, L1->LightSaturation, L1->LightBrightness);
        FPlane C2 = FGetHSV(L2->LightHue, L2->LightSaturation, L2->LightBrightness);

        float ColorDist =
            Abs(C1.X - C2.X) +
            Abs(C1.Y - C2.Y) +
            Abs(C1.Z - C2.Z);

        if (ColorDist > 20.f) continue;

        // --- 4. Must have unobstructed line-of-sight ---
        if (!UXOpenGLRenderDevice::BSPVisibilityRay(Model, -1, L1->Location, L2->Location))
            continue;

        // --- 5. Upper light must NOT be near the floor ---
        FVector Down2 = L2->Location + FVector(0,0,-128);
        bool UpperHasFloor = !UXOpenGLRenderDevice::BSPVisibilityRay(Model, -1, L2->Location, Down2);

        if (UpperHasFloor)
            continue; // upper is TOO near floor
        
        // If we reach here, L1 is a fake spotlight floor light
        return true;
    }

    return false;
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
        if (IsFakeSpotlightFloorLight(L, StaticLevelLights, Level->Model))
            continue;

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

// cached topology stuff for ULodMesh
UBOOL LoadTopologyFromDisk(FString MeshName, UXOpenGLRenderDevice::FMeshConnectivity& OutTopology)
{
    guard(UXOpenGLRenderDevice::LoadTopologyFromDisk);

    // Points straight to your local asset cache folder
    // e.g., "..\System\ShadowCache\Commando.bin"
    FString CachePath = FString::Printf(TEXT("..\\System\\xopengl\\topology\\%s.bin"), *MeshName);

    // Create a native engine file reader archive
    FArchive* Ar = GFileManager->CreateFileReader(*CachePath);
    if (!Ar)
    {
        return FALSE; // Cache Miss: File doesn't exist yet
    }

    // STAGE 1: Read the first 4 bytes (The total index count written by PowerShell)
    INT IndexCount = 0;
    *Ar << IndexCount;

    // Security check to guarantee we don't allocate corrupt out-of-bounds heap segments
    if (IndexCount > 0 && IndexCount < 200000) 
    {
        // Allocate space inside the TArray smoothly all at once
        OutTopology.TriangleIndices.AddZeroed(IndexCount);

        // STAGE 2: Bulk-copy the entire binary file payload straight into memory!
        // This takes virtually zero CPU cycles because it avoids any string manipulation.
        Ar->Serialize(&OutTopology.TriangleIndices(0), IndexCount * sizeof(INT));
    }

    // Cleanly unbind and close the file stream archive handle
    delete Ar;

    debugf(TEXT("SUCCESSFULLY LOADED GEOMETRIC SHADOW CACHE FOR: %s (%d Indices)"), *MeshName, IndexCount);
    return TRUE;
    
    unguard;
}

BOOL UXOpenGLRenderDevice::HasMappedTopology(AActor* Actor)
{
    FString MeshKey = Actor->Mesh->GetName();
    INT* pStatus = GMappedMeshes.Find(MeshKey);
    if (pStatus == NULL)
    {
        // Attempt to bulk-copy the pre-calculated binary index stream from disk
        FMeshConnectivity NewTopology;
        if (LoadTopologyFromDisk(MeshKey, NewTopology))
        {
            GDiscoveredTopologies.Set(*MeshKey, NewTopology);
            GMappedMeshes.Set(*MeshKey, 444); // Locked in as 100% complete shadow geometry!
            //debugf(TEXT("Shadow Topology Cache Hit for mesh %s."), *MeshKey);
            return true;
        }
        else
        {
            // Cache Miss: Mark with a fallback flag so we don't try to read disk every frame
            GMappedMeshes.Set(*MeshKey, 1); 
            //debugf(TEXT("Shadow Topology Cache Miss for mesh %s. Falling back to capsule splats."), *MeshKey);
            return false;
        }
    }
    else
    {
        return (*pStatus == 444);
    }
}

// call from GenerateCapsulesForMesh in the event we ever get new
// player ULodMesh assets for which we need to build offline topology (autoAligner.ps1)
void DumpEngineBasePose(ULodMesh* L)
{
    guard(UXOpenGLRenderDevice::DumpEngineBasePose);
    
    if (!L || L->Verts.Num() == 0) return;

    // Build path: "..\System\Commando_Engine_Verts.txt"
    FString CleanMeshName = L->GetName();
    FString FilePath = FString::Printf(TEXT("..\\System\\%s_Engine_Verts.txt"), *CleanMeshName);

    FArchive* Ar = GFileManager->CreateFileReader(*FilePath);
    if (Ar)
    {
        // Prevent file thrashing if it's already been dumped
        delete Ar;
        return; 
    }

    Ar = GFileManager->CreateFileWriter(*FilePath);
    if (!Ar) return;

    // Frame 0 spans from 0 to MemoryStride-1
    const INT MemoryStride = (L->FrameVerts > 0) ? L->FrameVerts : L->ModelVerts;

    for (INT v = 0; v < MemoryStride; ++v)
    {
        // Decompress the raw 11-11-10 integer bitfield vector data out of memory
        FVector P = L->Verts(v).Vector();

        // UNIVERSAL MATHEMATICAL BRIDGE: Replaces the hardcoded /8, /8, /4 script math
        // by dynamically evaluating the mesh asset's native Scale fields.
        // happens to work for commando, not for skaarj
        //P.X *= L->Scale.X * 2.0f;
        //P.Y *= L->Scale.Y * 2.0f;
        //P.Z *= L->Scale.Z * 2.0f;

        // Write out the perfectly scaled coordinates to match the modeling workspace
        FString Line = FString::Printf(TEXT("%f %f %f\n"), P.X, P.Y, P.Z);
        Ar->Serialize((void*)*Line, Line.Len() * sizeof(TCHAR));
    }

    delete Ar;
    debugf(TEXT("DUMPED BASE POSE FOR %s TO DISK. PROCEED WITH TEXT ALIGNMENT CHECKS."), *CleanMeshName);
    
    unguard;
}

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

    //DumpEngineBasePose(L);

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
FVector UXOpenGLRenderDevice::TransformMeshSpaceToWorld(const FVector& P, ULodMesh* L, AActor* Actor)
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

void UXOpenGLRenderDevice::ExtractMappedAnimatedTriangles(
    ULodMesh* L, 
    AActor* Actor, 
    const FMeshConnectivity& Blueprint, 
    TArray<FShadowTriangle>& OutTris)
{
    if (!L || !Actor || Blueprint.TriangleIndices.Num() == 0 || L->Verts.Num() == 0) return;

    const INT MemoryStride = (L->FrameVerts > 0) ? L->FrameVerts : L->ModelVerts;
    
    TArray<FVector> PosedVerts;
    PosedVerts.AddZeroed(MemoryStride);

    FMeshAnimSeq* Seq = L->GetAnimSeq(Actor->AnimSequence);
    check(Seq); // Guaranteed to be valid via the caller's bStaticMesh check

    INT FrameA_Index = 0, FrameB_Index = 0;
    FLOAT Alpha = 0.0f;

    // --- Core Animation Interp Sampling ---
    if (Actor->AnimFrame >= 0.0f)
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
    else
    {
        FrameA_Index = Seq->StartFrame;
        FrameB_Index = Seq->StartFrame;
        Alpha = 0.0f;
    }

    INT FrameA_Offset = FrameA_Index * MemoryStride;
    INT FrameB_Offset = FrameB_Index * MemoryStride;

    // Decompress the vertices for the active frame interpolation state
    for (INT i = 0; i < MemoryStride; i++)
    {
        INT VertA_Addr = FrameA_Offset + i;
        INT VertB_Addr = FrameB_Offset + i;

        if (VertA_Addr >= L->Verts.Num() || VertB_Addr >= L->Verts.Num())
            continue;

        PosedVerts(i) = L->Verts(VertA_Addr).Vector() + 
                       (L->Verts(VertB_Addr).Vector() - L->Verts(VertA_Addr).Vector()) * Alpha;
    }

    // Apply the spatial transformation matrix directly to the animated points
    for (INT i = 0; i < MemoryStride; i++)
    {
        PosedVerts(i) = TransformMeshSpaceToWorld(PosedVerts(i), L, Actor);
    }

    // --- Blueprint Unrolling ---
    INT NumIndices = Blueprint.TriangleIndices.Num();
    for (INT i = 0; i < NumIndices; i += 3)
    {
        if (i + 2 >= NumIndices) break;

        INT v0 = Blueprint.TriangleIndices(i);
        INT v1 = Blueprint.TriangleIndices(i+1);
        INT v2 = Blueprint.TriangleIndices(i+2);

        if (v0 >= MemoryStride || v1 >= MemoryStride || v2 >= MemoryStride)
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

// -----------------------------------------------------------------------------
// Explicitly chains a child local joint matrix onto an outcoded parent matrix
// -----------------------------------------------------------------------------
inline FCoords CombineBones(const FCoords& ChildLocal, const FCoords& ParentGlobal)
{
    FCoords Temp;

    // 1. FIXED UN-TRANSPOSED ROW PROJECTION
    // Projects the child's local translation components down each individual parent axis row in sequence.
    // This rotates the joint offset vector perfectly into parent space without introducing any slanted axis drift!
    Temp.Origin.X = ParentGlobal.Origin.X + (ChildLocal.Origin.X * ParentGlobal.XAxis.X + ChildLocal.Origin.Y * ParentGlobal.YAxis.X + ChildLocal.Origin.Z * ParentGlobal.ZAxis.X);
    Temp.Origin.Y = ParentGlobal.Origin.Y + (ChildLocal.Origin.X * ParentGlobal.XAxis.Y + ChildLocal.Origin.Y * ParentGlobal.YAxis.Y + ChildLocal.Origin.Z * ParentGlobal.ZAxis.Y);
    Temp.Origin.Z = ParentGlobal.Origin.Z + (ChildLocal.Origin.X * ParentGlobal.XAxis.Z + ChildLocal.Origin.Y * ParentGlobal.YAxis.Z + ChildLocal.Origin.Z * ParentGlobal.ZAxis.Z);

    // 2. VERIFIED ROW-BY-COLUMN ORIENTATION AXES CHAIN
    // Result.XAxis = Parent * Child.XAxis
    Temp.XAxis.X = ChildLocal.XAxis.X * ParentGlobal.XAxis.X +
                   ChildLocal.XAxis.Y * ParentGlobal.YAxis.X +
                   ChildLocal.XAxis.Z * ParentGlobal.ZAxis.X;
    Temp.XAxis.Y = ChildLocal.XAxis.X * ParentGlobal.XAxis.Y +
                   ChildLocal.XAxis.Y * ParentGlobal.YAxis.Y +
                   ChildLocal.XAxis.Z * ParentGlobal.ZAxis.Y;
    Temp.XAxis.Z = ChildLocal.XAxis.X * ParentGlobal.XAxis.Z +
                   ChildLocal.XAxis.Y * ParentGlobal.YAxis.Z +
                   ChildLocal.XAxis.Z * ParentGlobal.ZAxis.Z;

    // Result.YAxis = Parent * Child.YAxis
    Temp.YAxis.X = ChildLocal.YAxis.X * ParentGlobal.XAxis.X +
                   ChildLocal.YAxis.Y * ParentGlobal.YAxis.X +
                   ChildLocal.YAxis.Z * ParentGlobal.ZAxis.X;
    Temp.YAxis.Y = ChildLocal.YAxis.X * ParentGlobal.XAxis.Y +
                   ChildLocal.YAxis.Y * ParentGlobal.YAxis.Y +
                   ChildLocal.YAxis.Z * ParentGlobal.ZAxis.Y;
    Temp.YAxis.Z = ChildLocal.YAxis.X * ParentGlobal.XAxis.Z +
                   ChildLocal.YAxis.Y * ParentGlobal.YAxis.Z +
                   ChildLocal.YAxis.Z * ParentGlobal.ZAxis.Z;

    // Result.ZAxis = Parent * Child.ZAxis
    Temp.ZAxis.X = ChildLocal.ZAxis.X * ParentGlobal.XAxis.X +
                   ChildLocal.ZAxis.Y * ParentGlobal.YAxis.X +
                   ChildLocal.ZAxis.Z * ParentGlobal.ZAxis.X;
    Temp.ZAxis.Y = ChildLocal.ZAxis.X * ParentGlobal.XAxis.Y +
                   ChildLocal.ZAxis.Y * ParentGlobal.YAxis.Y +
                   ChildLocal.ZAxis.Z * ParentGlobal.ZAxis.Y;
    Temp.ZAxis.Z = ChildLocal.ZAxis.X * ParentGlobal.XAxis.Z +
                   ChildLocal.ZAxis.Y * ParentGlobal.YAxis.Z +
                   ChildLocal.ZAxis.Z * ParentGlobal.ZAxis.Z;

    return Temp;
}

FVector TransformPoint(const FCoords& C, const FVector& P)
{
    FVector R;
    R.X = P.X * C.XAxis.X + P.Y * C.YAxis.X + P.Z * C.ZAxis.X + C.Origin.X;
    R.Y = P.X * C.XAxis.Y + P.Y * C.YAxis.Y + P.Z * C.ZAxis.Y + C.Origin.Y;
    R.Z = P.X * C.XAxis.Z + P.Y * C.YAxis.Z + P.Z * C.ZAxis.Z + C.Origin.Z;
    return R;
}

// operator-isolated Quaternion to Row-Major FCoords converter
inline FCoords QuaternionToRowMajorCoords(const FQuat& Q, const FVector& Translation)
{
    FCoords Local;
    Local.Origin = Translation;

    // Pre-calculate squared quaternion components for high-precision unrolling
    FLOAT xx = Q.X * Q.X; FLOAT yy = Q.Y * Q.Y; FLOAT zz = Q.Z * Q.Z;
    FLOAT xy = Q.X * Q.Y; FLOAT xz = Q.X * Q.Z; FLOAT yz = Q.Y * Q.Z;
    FLOAT wx = Q.W * Q.X; FLOAT wy = Q.W * Q.Y; FLOAT wz = Q.W * Q.Z;

    // Build pure, un-transposed Row-Major basis tracking rows cleanly
    Local.XAxis.X = 1.0f - 2.0f * (yy + zz);
    Local.XAxis.Y = 2.0f * (xy - wz);
    Local.XAxis.Z = 2.0f * (xz + wy);

    Local.YAxis.X = 2.0f * (xy + wz);
    Local.YAxis.Y = 1.0f - 2.0f * (xx + zz);
    Local.YAxis.Z = 2.0f * (yz - wx);

    Local.ZAxis.X = 2.0f * (xz - wy);
    Local.ZAxis.Y = 2.0f * (yz + wx);
    Local.ZAxis.Z = 1.0f - 2.0f * (xx + yy);

    return Local;
}

inline FQuat SlerpQuatNew(const FQuat &quat1, const FQuat &quat2, float slerp)
{
    FQuat result;
    float omega, cosom, sininv, scale0, scale1;

    // Get cosine of angle between quats
    cosom = quat1.X * quat2.X +
            quat1.Y * quat2.Y +
            quat1.Z * quat2.Z +
            quat1.W * quat2.W;

    // --- FIX 1: THE NEIGHBORHOOD SHORT-PATH CHECK ---
    // If the dot product is negative, the quaternions are pointing in opposite 
    // directions on the hypersphere. We invert one to force the short-path slerp!
    FQuat targetQuat2 = quat2;
    if (cosom < 0.f)
    {
        cosom = -cosom;
        targetQuat2.X = -quat2.X;
        targetQuat2.Y = -quat2.Y;
        targetQuat2.Z = -quat2.Z;
        targetQuat2.W = -quat2.W;
    }

    // --- FIX 2: RE-SCALE CALIBRATED UPPER LIMIT CLAMP ---
    if (cosom < 0.9995f)
    {	
        omega = appAcos(cosom);
        sininv = 1.f / appSin(omega);
        scale0 = appSin((1.f - slerp) * omega) * sininv;
        scale1 = appSin(slerp * omega) * sininv;
        
        result.X = scale0 * quat1.X + scale1 * targetQuat2.X;
        result.Y = scale0 * quat1.Y + scale1 * targetQuat2.Y;
        result.Z = scale0 * quat1.Z + scale1 * targetQuat2.Z;
        result.W = scale0 * quat1.W + scale1 * targetQuat2.W;
        return result;
    }
    else
    {
        // Close angles linearize safely to prevent zero division crashes
        result.X = quat1.X + (targetQuat2.X - quat1.X) * slerp;
        result.Y = quat1.Y + (targetQuat2.Y - quat1.Y) * slerp;
        result.Z = quat1.Z + (targetQuat2.Z - quat1.Z) * slerp;
        result.W = quat1.W + (targetQuat2.W - quat1.W) * slerp;
        result.Normalize();
        return result;
    }
}

// Find the key index just before the given time
inline INT FindKeyBefore(const AnalogTrack& Track, FLOAT Time)
{
    const INT NumKeys = Track.KeyTime.Num();

    if (NumKeys == 0)
        return 0;

    if (Time <= Track.KeyTime(0))
        return 0;

    if (Time >= Track.KeyTime(NumKeys - 1))
        return NumKeys - 1;

    for (INT i = 0; i < NumKeys - 1; i++)
    {
        if (Track.KeyTime(i) <= Time && Time < Track.KeyTime(i + 1))
            return i;
    }

    return NumKeys - 1;
}

// Sample rotation (FRotator) from an AnalogTrack at a given time
inline FQuat SampleQuat(const AnalogTrack& Track, FLOAT Time)
{
    const INT NumKeys = Track.KeyQuat.Num();
    if (NumKeys == 0)
        return FQuat(0,0,0,1);

    INT A = FindKeyBefore(Track, Time);
    INT B = Min(A + 1, NumKeys - 1);

    FLOAT TimeA = Track.KeyTime(A);
    FLOAT TimeB = Track.KeyTime(B);

    FLOAT Alpha = (TimeB > TimeA) ? (Time - TimeA) / (TimeB - TimeA) : 0.f;

    const FQuat& QA = Track.KeyQuat(A);
    const FQuat& QB = Track.KeyQuat(B);

    // SLERP (use your fixed quaternion math!)
    FQuat Q = SlerpQuatNew(QA,QB, Alpha);
    Q.Normalize();

    return Q;
}

// Sample translation (FVector) from an AnalogTrack at a given time
inline FVector SamplePos(const AnalogTrack& Track, FLOAT Time)
{
    const INT NumPos  = Track.KeyPos.Num();
    const INT NumTime = Track.KeyTime.Num();

    // No translation keys ? no movement
    if (NumPos == 0 || NumTime == 0)
        return FVector(0,0,0);

    // Only one translation key ? constant offset
    if (NumPos == 1)
        return Track.KeyPos(0);

    // Clamp time before first key
    if (Time <= Track.KeyTime(0))
        return Track.KeyPos(0);

    // Clamp time after last key
    if (Time >= Track.KeyTime(NumPos - 1))
        return Track.KeyPos(NumPos - 1);

    // Find key A such that KeyTime[A] <= Time < KeyTime[A+1]
    INT A = 0;
    for (INT i = 0; i < NumPos - 1; i++)
    {
        if (Track.KeyTime(i) <= Time && Time < Track.KeyTime(i + 1))
        {
            A = i;
            break;
        }
    }

    INT B = A + 1;

    FLOAT TimeA = Track.KeyTime(A);
    FLOAT TimeB = Track.KeyTime(B);

    FLOAT Alpha = (TimeB > TimeA) ? (Time - TimeA) / (TimeB - TimeA) : 0.f;

    return Track.KeyPos(A) + (Track.KeyPos(B) - Track.KeyPos(A)) * Alpha;
}

void UXOpenGLRenderDevice::ExtractSkeletalMeshTriangles(
    USkeletalMesh* S,
    AActor* Actor,
    TArray<FShadowTriangle>& OutTris)
{
    guard(UXOpenGLRenderDevice::ExtractSkeletalMeshTriangles);

    if (!S || !Actor || S->Faces.Num() == 0) return;

    const INT TotalVerts = S->Points.Num();
    const INT NumBones   = S->RefSkeleton.Num();
    if (TotalVerts == 0 || NumBones == 0) return;

    // Workspace
    TArray<FVector> PosedLocalVerts;
    PosedLocalVerts.AddZeroed(TotalVerts);

    TArray<FCoords> BindPose;
    BindPose.AddZeroed(NumBones);

    TArray<FCoords> AnimatedPose;
    AnimatedPose.AddZeroed(NumBones);

    FVector OX, OY, OZ;
    GetAxes(S->RotOrigin, OX, OY, OZ);
    FCoords MeshRot(FVector(0,0,0), OX, OY, OZ);

    FVector AX, AY, AZ;
    GetAxes(Actor->Rotation, AX, AY, AZ);
    AX *= -1.f; // if you still need the handedness fix here
    FCoords ActorRot(FVector(0,0,0), AX, AY, AZ);
    
    // =========================================================================
    // STAGE 1: BUILD BIND-POSE BONES
    // =========================================================================
    for (INT b = 0; b < NumBones; b++)
    {
        const FMeshBone& Bone = S->RefSkeleton(b);

        // A. Read raw right-handed file values natively
        FQuat   Q = Bone.BonePos.Orientation;         
        FVector T = Bone.BonePos.Position.Vector(); 

        FCoords Local = QuaternionToRowMajorCoords(Q, T);
        
        if (b == 0)
            BindPose(b) = Local;
        else
            BindPose(b) = CombineBones(Local, BindPose(Bone.ParentIndex));
    }

    // =========================================================================
    // STAGE 2: BUILD ANIMATED BONES (LIVE TRACKS)
    // =========================================================================
    UAnimation* AnimPackage = Actor->SkelAnim ? Actor->SkelAnim : S->DefaultAnimation;
    MotionChunk* ActiveMove = nullptr;
    const FMeshAnimSeq* Seq = nullptr;

    if (AnimPackage)
    {
        Seq        = AnimPackage->GetAnimSeq(Actor->AnimSequence);
        ActiveMove = AnimPackage->GetMovement(Actor->AnimSequence);
    }

    UBOOL bHasActiveMotion = (ActiveMove && Seq && Seq->NumFrames > 0);

    if (bHasActiveMotion)
    {
        FLOAT TotalDuration = (FLOAT)Seq->NumFrames / (Seq->Rate > 0.f ? Seq->Rate : 30.f);
        FLOAT ProgressAlpha = Actor->AnimFrame;
        if (ProgressAlpha < 0.0f) ProgressAlpha = 0.0f;
        if (ProgressAlpha > 1.0f) ProgressAlpha = 1.0f;

        FLOAT AnimTime = ProgressAlpha * TotalDuration;

        for (INT b = 0; b < NumBones; b++)
        {
            const FMeshBone& Bone = S->RefSkeleton(b);

            FQuat   Q = Bone.BonePos.Orientation;
            FVector T = Bone.BonePos.Position.Vector();

            INT TrackIndex = -1;
            // indirect
            //for (INT ti = 0; ti < ActiveMove->BoneIndices.Num(); ti++)
            //{
            //    if (ActiveMove->BoneIndices(ti) == b)
            //    {
            //        TrackIndex = ti;
            //        break;
            //    }
            // semi direct
            if (b < ActiveMove->BoneIndices.Num()) TrackIndex = ActiveMove->BoneIndices(b);
            // super direct
            //TrackIndex = b;
 
            if (TrackIndex >= 0 && TrackIndex < ActiveMove->AnimTracks.Num())
            {
                AnalogTrack& Track = ActiveMove->AnimTracks(TrackIndex);

                FQuat   AnimQ = SampleQuat(Track, AnimTime);
                FVector AnimT = SamplePos(Track, AnimTime);

                if (Track.KeyQuat.Num() > 0)
                {
                    Q = AnimQ; // Locked absolute overwrite
                }

                if (Track.KeyPos.Num() > 0)
                {
                    T = AnimT;
                }
            }

            FCoords Local = QuaternionToRowMajorCoords(Q, T);
            
            if (b == 0)
                AnimatedPose(b) = Local;
            else
                AnimatedPose(b) = CombineBones(Local, AnimatedPose(Bone.ParentIndex));

            // some logging
            /*
            if (b == 0 || b == 37 || b == 42 || b == 41 || b == 46)
            {
                const FMeshBone& Bone = S->RefSkeleton(b);
                AnalogTrack& Track = ActiveMove->AnimTracks(b);

                // Isolate the raw file keys before they touch any functions
                FQuat   RawFileQ = Q;
                FVector RawFileT = T;

                // Read the fully accumulated matrix rows out of AnimatedPose(b)
                FCoords FinalM = AnimatedPose(b);

                FQuat AnimQ = SampleQuat(Track, AnimTime);

                debugf(TEXT("LEG_TRACE | Bone[%2d] | AnimTime: %6.3f"), b, AnimTime);
                debugf(TEXT("  -> RawFileT : (X=%9.4f, Y=%9.4f, Z=%9.4f)"), RawFileT.X, RawFileT.Y, RawFileT.Z);
                debugf(TEXT("  -> RawFileQ : (X=%9.4f, Y=%9.4f, Z=%9.4f, W=%9.4f)"), AnimQ.X, AnimQ.Y, AnimQ.Z, AnimQ.W);
                debugf(TEXT("  -> FinalPos : (X=%9.4f, Y=%9.4f, Z=%9.4f)"), FinalM.Origin.X, FinalM.Origin.Y, FinalM.Origin.Z);
                debugf(TEXT("  -> FinalXRow: [X=%9.4f, Y=%9.4f, Z=%9.4f]"), FinalM.XAxis.X, FinalM.XAxis.Y, FinalM.XAxis.Z);
                debugf(TEXT("  -> FinalYRow: [X=%9.4f, Y=%9.4f, Z=%9.4f]"), FinalM.YAxis.X, FinalM.YAxis.Y, FinalM.YAxis.Z);
                debugf(TEXT("  -> FinalZRow: [X=%9.4f, Y=%9.4f, Z=%9.4f]"), FinalM.ZAxis.X, FinalM.ZAxis.Y, FinalM.ZAxis.Z);
            }*/
        }
    }
    else
    {
        for (INT b = 0; b < NumBones; b++)
            AnimatedPose(b) = BindPose(b);
    }

    // debug draw bones
    /*
    for (INT b = 0; b < NumBones; b++)
    {
        // TARGET SAMPLES: Read natively from your stable AnimatedPose tree
        // Both Stage 2 and Stage 3 are completely pristine and un-mutated!
        const FCoords& Pose = AnimatedPose(b); 
        FVector RawLocalP = Pose.Origin;

        // A. Scale the completed native local tree position relative to its asset footprint
        RawLocalP -= S->Origin;
        RawLocalP *= S->Scale;

        // B. Apply MeshRot cleanly to handle the built-in horizontal asset turning
        RawLocalP = RawLocalP.TransformVectorBy(MeshRot);

        FVector P = RawLocalP;

        // D. Project the freshly stood-up coordinates out into world space map slots
        P *= Actor->DrawScale;
        P = P.TransformVectorBy(ActorRot);
        P += Actor->Location;

        // --- Emit box node around target coordinate P ---
        const FLOAT S_Val = 4.0f;

        FVector V0 = P + FVector(-S_Val, -S_Val, -S_Val);
        FVector V1 = P + FVector( S_Val, -S_Val, -S_Val);
        FVector V2 = P + FVector( S_Val,  S_Val, -S_Val);
        FVector V3 = P + FVector(-S_Val,  S_Val, -S_Val);

        FVector V4 = P + FVector(-S_Val, -S_Val,  S_Val);
        FVector V5 = P + FVector( S_Val, -S_Val,  S_Val);
        FVector V6 = P + FVector( S_Val,  S_Val,  S_Val);
        FVector V7 = P + FVector(-S_Val,  S_Val,  S_Val);

        auto EmitTri = [&](const FVector& A, const FVector& B, const FVector& C)
        {
            FShadowTriangle T;
            T.V0 = A;
            T.V1 = B;
            T.V2 = C;
            OutTris.AddItem(T);
        };

        // Front
        EmitTri(V0, V1, V2);
        EmitTri(V0, V2, V3);

        // Back
        EmitTri(V4, V5, V6);
        EmitTri(V4, V6, V7);

        // Left
        EmitTri(V0, V4, V7);
        EmitTri(V0, V7, V3);

        // Right
        EmitTri(V1, V5, V6);
        EmitTri(V1, V6, V2);

        // Top
        EmitTri(V3, V2, V6);
        EmitTri(V3, V6, V7);

        // Bottom
        EmitTri(V0, V1, V5);
        EmitTri(V0, V5, V4);
    }*/
    
    // =========================================================================
    // STAGE 3: SKIN USING BIND-POSE OFFSETS + ANIMATED BONES
    // =========================================================================
    for (INT b = 0; b < NumBones; b++)
    {
        const VBoneInfIndex& InfIdx = S->BoneWeightIdx(b);
        INT First = InfIdx.WeightIndex;
        INT Count = InfIdx.Number;

        FCoords BindM     = BindPose(b);
        FCoords BindM_Inv = BindM.Inverse();

        FCoords AnimM     = AnimatedPose(b);

        for (INT w = 0; w < Count; w++)
        {
            INT Addr = First + w;
            if (Addr >= S->BoneWeights.Num()) break;

            const VBoneInfluence& Infl = S->BoneWeights(Addr);
            INT VertIdx = Infl.PointIndex;
            if (VertIdx >= TotalVerts) continue;

            FLOAT Weight = Infl.BoneWeight / 65535.f;

            FVector P = S->Points(VertIdx);
            P.Y = -P.Y; // fix coronal plane inversion between bones and verts
            P.X = -P.X;
            // Bind-pose offset
            FVector LocalOffset = TransformPoint(BindM_Inv, P);

            // Animated position
            FVector Skinned = TransformPoint(AnimM, LocalOffset);

            PosedLocalVerts(VertIdx) += Skinned * Weight;
        }
    }

    // =========================================================================
    // STAGE 4: APPLY MESH TRANSFORMS
    // =========================================================================
    TArray<FVector> PosedWorldVerts;
    PosedWorldVerts.AddZeroed(TotalVerts);

    for (INT i = 0; i < TotalVerts; i++)
    {
        FVector P = PosedLocalVerts(i);

        if (P.IsZero())
            P = S->Points(i);

        P -= S->Origin;
        P *= S->Scale;
        P = P.TransformVectorBy(MeshRot);

        PosedWorldVerts(i) = P;
    }

    // =========================================================================
    // STAGE 5: APPLY ACTOR TRANSFORMS
    // =========================================================================
    for (INT i = 0; i < TotalVerts; i++)
    {
        FVector P = PosedWorldVerts(i);

        P *= Actor->DrawScale;
        P = P.TransformVectorBy(ActorRot);
        P += Actor->Location;

        PosedWorldVerts(i) = P;
    }

    // =========================================================================
    // STAGE 6: EMIT TRIANGLES
    // =========================================================================
    for (INT ti = 0; ti < S->Faces.Num(); ti++)
    {
        const FMeshFace& F = S->Faces(ti);

        INT v0 = S->Wedges(F.iWedge[0]).iVertex;
        INT v1 = S->Wedges(F.iWedge[1]).iVertex;
        INT v2 = S->Wedges(F.iWedge[2]).iVertex;

        FShadowTriangle T;
        T.V0 = PosedWorldVerts(v0);
        T.V1 = PosedWorldVerts(v1);
        T.V2 = PosedWorldVerts(v2);

        OutTris.AddItem(T);
    }

    unguard;
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

    INT    UserGuaranteed        = 0;   // user preference for number of guaranteed lights
    if (ShadowMaps == ShadowMaps_Low) UserGuaranteed = 3;
    else if (ShadowMaps == ShadowMaps_Medium) UserGuaranteed = 10;
    else if (ShadowMaps == ShadowMaps_High) UserGuaranteed = 30;
    else if (ShadowMaps == ShadowMaps_HolyShit) UserGuaranteed = 30;
    INT    Guaranteed            = UserGuaranteed; // Dynamically scales between a user-configured floor and a hardware ceiling
    INT    ActivePoolSize        = Guaranteed * 3;   // Mathematically locked to UserGuaranteed * 2

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
    if (ShadowMaps == ShadowMaps_HolyShit &&  TotalLights > (UserGuaranteed * 3))
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

