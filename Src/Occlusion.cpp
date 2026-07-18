        
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include "XOpenGLDrv.h"
#include "XOpenGL.h"

#include "thirdparty/stb/stb_image.h" // header only, no implementation
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "thirdparty/stb/stb_image_write.h" // this file owns the writer implementation

#define STB_DXT_IMPLEMENTATION
#include "thirdparty/stb/stb_dxt.h"

#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>   // std::shuffle
#include <random>      // std::mt19937, std::random_device

FPlane* AtlasData = nullptr;
SIZE_T  AtlasSizeBytes = 0;
struct FMappedAtlas
{
    SIZE_T Size = 0;
#if _WIN32
    HANDLE FileHandle = INVALID_HANDLE_VALUE;
#else
    int FileDescriptor = -1;
#endif

    bool Create(SIZE_T InSize, const TCHAR* ScratchFilePath)
    {
        Size = InSize;

#if _WIN32
        // Clean up mixed slashes just in case
        FString FixedPath = FString(ScratchFilePath).Replace(TEXT("/"), TEXT("\\"));

        FileHandle = CreateFile(
            *FixedPath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, 
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY, 
            nullptr
        );

        if (FileHandle == INVALID_HANDLE_VALUE)
        {
            debugf(TEXT("XOpenGL: CreateFile failed! Error: %d"), GetLastError());
            return false;
        }

        // Force physical size allocation via 64-bit offsets
        LARGE_INTEGER LiSize;
        LiSize.QuadPart = (LONGLONG)Size;

        if (!SetFilePointerEx(FileHandle, LiSize, nullptr, FILE_BEGIN) || !SetEndOfFile(FileHandle))
        {
            debugf(TEXT("XOpenGL: File expansion failed! Error: %d"), GetLastError());
            CloseHandle(FileHandle);
            FileHandle = INVALID_HANDLE_VALUE;
            return false;
        }

        return true;
#else
        // Linux standard file creation code goes here...
        // can set to return true if we ever fill this in
        return false;
#endif
    }

    void Destroy()
    {
#if _WIN32
        if (FileHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(FileHandle);
            FileHandle = INVALID_HANDLE_VALUE;
        }
#endif
    }
};
FMappedAtlas MappedAtlas;
UBOOL bAtlasMapped = false;
#if _WIN32
#include <windows.h>

class FWin32CriticalSection : public FSynchronize
{
private:
    CRITICAL_SECTION Mutex;
public:
    FWin32CriticalSection()  { InitializeCriticalSection(&Mutex); }
    ~FWin32CriticalSection() { DeleteCriticalSection(&Mutex); }
    
    // Abstract interface overrides required by FSynchronize
    virtual void Lock() override   { EnterCriticalSection(&Mutex); }
    virtual void Unlock() override { LeaveCriticalSection(&Mutex); }
};

// Now we can safely instantiate a concrete, globally accessible lock object!
FWin32CriticalSection* FileWriteMutex = nullptr;
#endif

std::thread AtlasThread;
std::atomic<bool> AtlasFinished{false};
FString AtlasName = TEXT("");
INT AtlasW, AtlasH;

struct FPendingLightmap
{
    INT SurfIndex;

    // Original per-surface LM resolution
    INT Width;
    INT Height;

    // UV extents in surface-local lightmap space
    float MinU, MaxU;
    float MinV, MaxV;

    // Geometric basis for reconstructing world positions
    UXOpenGLRenderDevice::SurfaceBasis Basis;

    // --- Atlas packing results (filled in AFTER packing) ---

    // Pixel-space placement inside the atlas
    INT AtlasX = 0;
    INT AtlasY = 0;

    // Normalized UVs inside the atlas (0..1)
    float AtlasMinU = 0.f;
    float AtlasMaxU = 0.f;
    float AtlasMinV = 0.f;
    float AtlasMaxV = 0.f;

    TArray<DWORD> LocalRejectionMasks;
};
TArray<FPendingLightmap> PendingLightmaps;

struct FOcclusionJob
{
    UXOpenGLRenderDevice* Owner = nullptr;

	std::atomic<bool> bAbort{false};
	ULevel* LevelAtStart = nullptr;

	std::queue<int> PendingSurfaces;
	std::mutex      QueueMutex;

	std::vector<std::thread> Threads;
    std::atomic<int> ActiveWorkers{0};

    FOcclusionJob::~FOcclusionJob()
    {
        StopAndJoin();
    }
    void Start(UXOpenGLRenderDevice* InOwner, ULevel* Level);
	void StartThreads(INT NumThreads);
    void WorkerLoop();
	void StopAndJoin();
    bool IsRunning() const { return ActiveWorkers.load(std::memory_order_acquire) > 0; }
};
FOcclusionJob OcclusionJob;

UXOpenGLRenderDevice::SurfaceBasis UXOpenGLRenderDevice::BuildSurfaceBasis(FSurfInfo* SI, ULevel* Level, const FBspSurf& Surf)
{
    SurfaceBasis B;

    // Always use world-space vectors from Level->Model
    FVector U = Level->Model->Vectors(Surf.vTextureU);
    FVector V = Level->Model->Vectors(Surf.vTextureV);
    FVector N = Level->Model->Vectors(Surf.vNormal);
    FVector Origin = Level->Model->Points(Surf.pBase);

    // Normalize and orthogonalize
    if (!U.IsNearlyZero()) U = U.SafeNormal(); else U = FVector(1, 0, 0);
    if (!V.IsNearlyZero()) V = V.SafeNormal(); else V = FVector(0, 1, 0);
    if (!N.IsNearlyZero()) N = N.SafeNormal(); else N = (U ^ V).SafeNormal();

    // Recompute V to ensure orthogonality
    V = (N ^ U).SafeNormal();

    B.TangentU = U;
    B.TangentV = V;
    B.Normal = N;
    B.Origin = Origin;

    return B;
}

struct FStaticLightContrib
{
    FPlane Shadowed;     // sum_i atten_i * vis_i * color_i
    FPlane Unshadowed;   // sum_i atten_i * color_i
};

INT GetHitSurfIndex(UModel* Model, const FCheckResult& Hit)
{
    // BSP hits come through the model primitive; Item is the node index.
    if (!Hit.Primitive)
        return INDEX_NONE;

    INT NodeIndex = Hit.Item;
    if (NodeIndex < 0 || NodeIndex >= Model->Nodes.Num())
        return INDEX_NONE;

    const FBspNode& Node = Model->Nodes(NodeIndex);
    return Node.iSurf;
}

static UBOOL SameSurface(
    const UModel* Model,
    INT OriginSurfIndex,
    INT HitSurfIndex,
    INT HitNodeIndex)
{
    if (OriginSurfIndex == HitSurfIndex)
        return true;

    if (OriginSurfIndex < 0 || HitSurfIndex < 0)
        return false;

    const FBspSurf& OriginSurf = Model->Surfs(OriginSurfIndex);
    const FBspSurf& HitSurf = Model->Surfs(HitSurfIndex);

    //const TCHAR* StartTexture = OriginSurf.Texture ? OriginSurf.Texture->GetName() : TEXT("None");
    //const TCHAR* BackTexture = HitSurf.Texture ? HitSurf.Texture->GetName() : TEXT("None");

    // 1. Texture identity
    //if (appStricmp(StartTexture, BackTexture) != 0)
    //    return false;
    if (OriginSurf.Texture != HitSurf.Texture)
        return false;

    return true; // dumb: just checking texture.  good enough

    /*
    // 2. Node membership: is the hit node one of the origin surface's nodes?
    //    (Strongest possible identity test.)
    for (INT i = 0; i < A.Nodes.Num(); ++i)
    {
        if (OriginSurf.Nodes(i) == HitNodeIndex)
            return true;
    }

    // 3. Plane equivalence:
    //    Compare the hit node's plane to ANY node belonging to the origin surface.
    const FBspNode& HitNode = Model->Nodes(HitNodeIndex);
    const FPlane& HitPlane  = HitNode.Plane;

    for (INT i = 0; i < OriginSurf.Nodes.Num(); ++i)
    {
        INT OriginNodeIndex = OriginSurf.Nodes(i);
        if (OriginNodeIndex >= 0 && OriginNodeIndex < Model->Nodes.Num())
        {
            const FBspNode& OriginNode = Model->Nodes(OriginNodeIndex);
            if (OriginNode.Plane == HitPlane)
                return true;
        }
    }

    return false;
    */
}

static UBOOL BacktraceEmergesFromOrigin(
    UModel* Model,
    const FVector& Start, // the origin surface (meant to be just outside it with normal offset)
    const FVector& Escaped, // where we escaped solidity
    INT OriginSurfIndex)
{
    const FLOAT skipMagnitude = 8.0;

    FCheckResult Hit;

    // Bounded backtrace: A -> B only
    UBOOL bHit = !Model->LineCheck(
        Hit, nullptr,
        Start,
        Escaped,
        FVector(0,0,0),
        0
    );

    // No more hits in the segment -> nothing between us and origin.  Shouldn't happen we wouldn't *emerge* from the origin if it wasn't there to collide with on a back ray
    if (!bHit)
        return true; // treat as emerged through origin

    // If we've effectively reached Start, we're done
    if ((Hit.Location - Start).Size() <= skipMagnitude * 2)
        return true;

    const FBspSurf& OriginSurf = Model->Surfs(OriginSurfIndex);
    UBOOL isMover = (OriginSurf.Actor && OriginSurf.Actor->IsA(AMover::StaticClass()));
    if (isMover && (Hit.Location - Start).Size() <= skipMagnitude * 4) // bit more leeway for movers
        return true;

    INT NodeIndex = Hit.Item;
    if (NodeIndex < 0 || NodeIndex >= Model->Nodes.Num())
        return true; // invalid -> assume origin

    FBspNode& Node = Model->Nodes(NodeIndex);
    // Skip non-vis-blocking / non-CSG.  Could iterate to the next, but unlikely to be one or we would have hit it and started backtracing then
    if (Node.NodeFlags & (NF_NotVisBlocking | NF_NotCsg | NF_IsNew))
        return true;

    // Now we have a real CSG surface
    INT SurfIndex = Node.iSurf;
    if (SurfIndex < 0 || SurfIndex >= Model->Surfs.Num())
        return true; // malformed -> assume origin

    const FBspSurf& HitSurf = Model->Surfs(SurfIndex);
    //bool hitMover = (HitSurf.Actor && HitSurf.Actor->IsA(AMover::StaticClass()));
    //if (hitMover)
    //    return true; // treat movers as non-blocking for emergence.  Could be more precise and check if it's the same mover or something, but this is just a backcheck to prevent false occlusion when we should have emerged, so better to be lenient and avoid false occlusion

    DWORD PF = HitSurf.PolyFlags;

    if (PF & (PF_Translucent | PF_Invisible | PF_NotSolid | 
                PF_Masked | PF_AlphaTexture | PF_Portal | PF_Modulated))
        return true;

    // Compare to origin.  Currently only by texture but could try to get more exact with iSurfs and planes
    if (SameSurface(Model, OriginSurfIndex, SurfIndex, NodeIndex))
        return true; // emerged through origin

    return false; // hit a different real CSG surface -> occluded
}


static UBOOL EmergedFromTransparent(
    UModel* Model,
    const FVector& Start,
    const FVector& CurrentStart)
{
    FCheckResult BackHit;

    // Cast from Start -> CurrentStart
    Model->LineCheck(
        BackHit, nullptr,
        Start,
        CurrentStart,
        FVector(0,0,0),
        0
    );

    INT NodeIndex = BackHit.Item;
    if (NodeIndex < 0 || NodeIndex >= Model->Nodes.Num())
        return false; // invalid hit -> treat as non-transparent

    FBspNode& Node = Model->Nodes(NodeIndex);

    // 1. NodeFlags check (non-vis-blocking / non-CSG)
    if (Node.NodeFlags & (NF_NotVisBlocking | NF_NotCsg | NF_IsNew))
        return true;

    // 2. PolyFlags check (transparent / masked / invisible / portal)
    INT SurfIndex = Node.iSurf;
    if (SurfIndex >= 0 && SurfIndex < Model->Surfs.Num())
    {
        const FBspSurf& Surf = Model->Surfs(SurfIndex);
        DWORD PF = Surf.PolyFlags;

        if (PF & (PF_Translucent | PF_Invisible | PF_NotSolid |
                  PF_Masked | PF_AlphaTexture | PF_Portal))
        {
            return true;
        }
    }

    return false;
}

// used in creating the occlusion map, and also in picking hero lights in PerPixel
UBOOL UXOpenGLRenderDevice::BSPVisibilityRay(
    UModel* Model,
    INT OriginSurfIndex,
    const FVector& Start,
    const FVector& End)
{
    FLOAT skipMagnitude = 8.0f;

    FVector Dir          = (End - Start).SafeNormal();
    FVector CurrentStart = Start;

    bool bInitialCheck    = true;
    bool bEscapedOrigin   = false;
    bool bLastTranslucent = false;
    bool isMover          = false;
    FBspSurf* OriginSurf  = nullptr;

    if (OriginSurfIndex >= 0 && OriginSurfIndex < Model->Surfs.Num())
    {
        OriginSurf = &Model->Surfs(OriginSurfIndex); 
        isMover = (OriginSurf->Actor && OriginSurf->Actor->IsA(AMover::StaticClass()));
    }
    else
    {
        // Treat as starting out in free air, escaping the origin immediately
        bEscapedOrigin = true; 
    }

    bool occluded = true; // default pessimistic

    while (true)
    {
        // clamp to light
        float distToEnd = (End - CurrentStart) | Dir;
        if (distToEnd <= 0.0f)
        {
            occluded = false;
            break;
        }

        FCheckResult Hit;
        bool bHit = !Model->LineCheck(
            Hit, nullptr,
            End,
            CurrentStart,
            FVector(0,0,0),
            0);

        if (!bHit)
        {
            // test was from air, and no further occlusion to the light

            if (!bEscapedOrigin && !bInitialCheck)
            {
                // allow movers to escape through another surface.  help hidden by default doors (eg DM-Pressure)
                if (!isMover && !BacktraceEmergesFromOrigin(Model, Start, CurrentStart, OriginSurfIndex))
                {
                    occluded = true;
                    break;
                }
            }
            if (bLastTranslucent)
            {
                if (!EmergedFromTransparent(Model, Start, CurrentStart))
                {
                    occluded = true;
                    break;
                }
            }

            occluded = false;
            break;
        }

        // we have a hit.
        
        INT NodeIndex = Hit.Item;
        // NodeIndex == 0 means "inside a surface" -> keep tunneling if we are still waiting to emerge from origin or we went into glass
        bool bInsideSurface = (NodeIndex == 0 || NodeIndex >= Model->Nodes.Num());
        bool bTransSurf = false;
        bool bNonVisNode = false;
        bool bSameSurface = false;

        if (NodeIndex > 0 && NodeIndex < Model->Nodes.Num())
        {
            // the hit is on an outside surface, not the line originating from inside a surface

            // origin-emergence backcheck happens the first time we leave the origin (if we started in it at all)
            if (!bEscapedOrigin && !bInitialCheck)
            {
                // allow movers to escape through another surface.  help hidden by default doors (eg DM-Pressure)
                if (!isMover && !BacktraceEmergesFromOrigin(Model, Start, CurrentStart, OriginSurfIndex))
                {
                    occluded = true;
                    break;
                }
            }
            bEscapedOrigin = true;

            // if we were inside glass, verify we emerged from glass
            if (bLastTranslucent)
            {
                if (!EmergedFromTransparent(Model, Start, CurrentStart))
                {
                    occluded = true;
                    break;
                }
                bLastTranslucent = false;
            }

            // classify non-vis-blocking
            FBspNode& Node = Model->Nodes(NodeIndex);
            bNonVisNode = (Node.NodeFlags & (NF_NotVisBlocking | NF_NotCsg | NF_IsNew)) != 0;

            // classify transparency
            INT HitSurf = Node.iSurf;
            if (HitSurf >= 0 && HitSurf < Model->Surfs.Num())
            {
                const FBspSurf& Surf = Model->Surfs(HitSurf);
                DWORD PF = Surf.PolyFlags;
                bTransSurf = (PF & (PF_Translucent | PF_Invisible | PF_NotSolid |
                                    PF_Masked | PF_AlphaTexture | PF_Portal)) != 0;
                bSameSurface = isMover && OriginSurf && Surf.Actor == OriginSurf->Actor;
                //bSameSurface |= SameSurface(Model, OriginSurf, HitSurf, NodeIndex);
            }
            else
            {
                bInsideSurface = true; // malformed surf index -> treat as inside surface to be safe
            }
        }

        //
        // pass-through rules:
        //
        // 1. If inside a surface (NodeIndex == 0) -> MUST tunnel until air if we were in glass or the origin
        // 2. If transparent -> Start tunneling. Will continue until air.
        // 3. If non-vis-blocking -> Ditto
        // 4. If same surface as origin -> tunnel.
        //

        if ( (bInsideSurface && (bLastTranslucent || !bEscapedOrigin)) || bTransSurf || bNonVisNode || bSameSurface)
        {
            bInitialCheck    = false;
            bLastTranslucent |= bTransSurf || bNonVisNode;
            CurrentStart     = Hit.Location + Dir * skipMagnitude;
        }
        else
        {
            // solid, non-origin, non-transparent -> occluder
            occluded = true;
            break;
        }
    }

    return !occluded;
}

// =========================================================================
// GRID-LEVEL SINGLE LIGHT EVALUATOR (PER-SURFACE METADATA BAKER)
// Loops through every coordinate pixel on the surface for a single light source.
// Populates temporary, isolated absolute energy arrays via references.
// Returns the absolute total unoccluded energy accumulated across the entire grid surface.
// =========================================================================
FLOAT UXOpenGLRenderDevice::EvaluateSingleLightContribution(
    AActor* Light,
    INT iSurf,
    ULevel* Level,
    UBOOL TwoSided,
    UBOOL bIsMover,
    INT W, INT H,
    FLOAT minU, FLOAT maxU,
    FLOAT minV, FLOAT maxV,
    const SurfaceBasis& Basis,
    TArray<FPlane>& TempShadowedGrid,
    TArray<FPlane>& TempUnshadowedGrid)
{
    // Initialize our temporary light tracking arrays to clean absolute zeros
    TempShadowedGrid.Empty(W * H);
    TempUnshadowedGrid.Empty(W * H);
    TempShadowedGrid.AddZeroed(W * H);
    TempUnshadowedGrid.AddZeroed(W * H);

    // Track total physical radiometric energy received by this light source across the grid
    FLOAT TotalGridUnoccludedEnergy = 0.0f;

    // THREAD-SAFETY MECHANISM: GATE AT THE ENTRY OF SETUP
    // We'll loop here until it's safe to let this specific light execute its raycasts
    for (;;)
    {
        ULevel* FrameLevel = GFrameLevel.load(std::memory_order_acquire);

        // Case 1: Engine is between frames: pause and wait
        if (FrameLevel == nullptr)
        {
            if (OcclusionJob.bAbort.load(std::memory_order_relaxed))
                return 0.0f;

            std::this_thread::yield();
            continue; // Back to the for (;;), retry the same light index
        }

        // Case 2: Level changed entirely: abort this complete surface job
        if (FrameLevel != Level)
        {
            return 0.0f;
        }

        // Tentatively enter the danger zone for this light's full grid execution
        GOcclusionInBSP.fetch_add(1, std::memory_order_acquire);

        // Double-check after increment to catch tight races with Unlock/level changes
        FrameLevel = GFrameLevel.load(std::memory_order_acquire);

        // Still the identical level: we are verified good to break and process the light
        if (FrameLevel == Level)
            break;

        // Not matching anymore: back cleanly out of the danger zone immediately
        GOcclusionInBSP.fetch_sub(1, std::memory_order_release);

        // If it's nullptr now, we just slipped between frame margins: pause and retry
        if (FrameLevel == nullptr)
        {
            if (OcclusionJob.bAbort.load(std::memory_order_relaxed))
                return 0.0f;

            std::this_thread::yield();
            continue; // Retry same light index
        }

        // Otherwise, it was a totally different non-null level: hard abort
        return 0.0f;
    }

    if (!Light) { GOcclusionInBSP.fetch_sub(1, std::memory_order_release); return 0.0f; }

    UModel* Model = Level->Model;
    FakeSpotlightPair* SpotData = GetSpotlightData(Light);
    UBOOL bIsSpot = (SpotData != nullptr);
    FLOAT Radius = bIsSpot ? SpotData->ReachRadius : Light->WorldLightRadius();

    if (Radius <= 0.f) { GOcclusionInBSP.fetch_sub(1, std::memory_order_release); return 0.0f; }

    FVector TargetLightPos = bIsSpot ? SpotData->TopLight->Location : Light->Location;
    
    // Pre-calculate structural UV coordinate dimensions
    FLOAT USize = maxU - minU;
    FLOAT VSize = maxV - minV;

    // Multipliers utilized to bypass coplanar rounding gaps inside the raycaster
    FLOAT mult = 2.0f;
    if (TwoSided) mult = 10.0f;
    if (bIsMover)  mult = 2.0f;

    // DROP THE LOCK IMMEDIATELY AFTER SETUP COMPLETION!
    GOcclusionInBSP.fetch_sub(1, std::memory_order_release);

    if (!Light)
        return 0.0f;

    // --- HIGH-FREQUENCY 2D GRID SURFACE ITERATION BLOCK ---
    for (INT y = 0; y < H; ++y)
    {
        // same thread safety mechanism (brevity version)
        for (;;)
        {
            ULevel* FrameLevel = GFrameLevel.load(std::memory_order_acquire);
            if (FrameLevel == nullptr) { if (OcclusionJob.bAbort.load(std::memory_order_relaxed)) return 0.0f; std::this_thread::yield(); continue; }
            if (FrameLevel != Level) return 0.0f;

            GOcclusionInBSP.fetch_add(1, std::memory_order_acquire);
            FrameLevel = GFrameLevel.load(std::memory_order_acquire);
            if (FrameLevel == Level) break; // Line safe to raycast!

            GOcclusionInBSP.fetch_sub(1, std::memory_order_release);
            if (FrameLevel == nullptr) { if (OcclusionJob.bAbort.load(std::memory_order_relaxed)) return 0.0f; std::this_thread::yield(); continue; }
            return 0.0f;
        }

        FLOAT v = (y + 0.5f) / FLOAT(H);
        FLOAT V = minV + v * VSize;

        for (INT x = 0; x < W; ++x)
        {
            FLOAT u = (x + 0.5f) / FLOAT(W);
            FLOAT U = minU + u * USize;

            // Compute exact real-world 3D point coordinates
            FVector WorldPos = Basis.Origin + (Basis.TangentU * U) + (Basis.TangentV * V);
            INT PixelIndex = y * W + x;

            // Evaluate standard distance boundary thresholds
            FVector L = TargetLightPos - WorldPos;
            FLOAT Dist = L.Size();
            if (Dist <= SMALL_NUMBER)
                continue;

            FVector Ldir = L / Dist;
            FLOAT NdotL = (Basis.Normal | Ldir);
            if (TwoSided)
                NdotL = fabs(NdotL);
            if (NdotL <= 0.f)
                continue;

            // Match high-precision GPU linear attenuation falloffs
            FLOAT ClampedDist = Clamp(Dist / Radius, 0.0f, 1.0f);
            FLOAT Atten = 1.0f - ClampedDist;
            if (Atten <= 0.f)
                continue;

            // Quantize base illumination colors via core palettes
            FPlane RGB;
            if (Light->LightType == LT_TexturePaletteOnce || Light->LightType == LT_TexturePaletteLoop)
            {
                FLOAT AnimatedBrightness = Light->LightBrightness;
                RGB = FPlane(AnimatedBrightness / 255.0f, AnimatedBrightness / 255.0f, AnimatedBrightness / 255.0f, 1.0f);
            }
            else
            {
                FLOAT BaseBrightness = bIsSpot ? (FLOAT)SpotData->Brightness : Light->LightBrightness;
                RGB = FGetHSV(Light->LightHue, Light->LightSaturation, (BYTE)BaseBrightness);
            }

            // --- OPTIONAL SPOTLIGHT CONE INTERPOLATION ---
            FLOAT ConeFactor = 1.0f;
            if (bIsSpot)
            {
                FVector LightDirNorm = (WorldPos - TargetLightPos).SafeNormal();
                FLOAT CosAngle = LightDirNorm | SpotData->SpotDirection;

                if (CosAngle < SpotData->SpotCosOuter)
                    continue; // Completely outside the beam envelope

                if (CosAngle < SpotData->SpotCosInner)
                {
                    FLOAT Range = SpotData->SpotCosInner - SpotData->SpotCosOuter;
                    FLOAT CosineGradient = (CosAngle - SpotData->SpotCosOuter) / Max(Range, 0.001f);
                    FLOAT BaseCone = Clamp(CosineGradient, 0.0f, 1.0f);

                    // Zero-Transcendental contrast boost to balance vertical Lambertian flat-lines
                    ConeFactor = BaseCone * BaseCone * BaseCone * BaseCone * BaseCone * BaseCone;
                }

                ConeFactor *= 2.5f; // Hardcoded intensity booster constant
            }

            // Compute the absolute raw, unoccluded radiometric energy vector for this coordinate
            FVector AbsoluteColor = RGB * NdotL * Atten * ConeFactor;

            // Log raw unshadowed absolute values into the temporary isolated array buffer
            TempUnshadowedGrid(PixelIndex).X = AbsoluteColor.X;
            TempUnshadowedGrid(PixelIndex).Y = AbsoluteColor.Y;
            TempUnshadowedGrid(PixelIndex).Z = AbsoluteColor.Z;
            TempUnshadowedGrid(PixelIndex).W = 1.0f;

            // Accumulate absolute luminance intensity straight into the surface tracker
            TotalGridUnoccludedEnergy += (AbsoluteColor.X + AbsoluteColor.Y + AbsoluteColor.Z);

            // --- GEOMETRIC OCCLUSION RAYCAST TRACING ---
            FVector SamplePos = WorldPos + Basis.Normal * mult;
            UBOOL bUnobstructed = BSPVisibilityRay(Model, iSurf, SamplePos, TargetLightPos);

            if (TwoSided)
            {
                SamplePos = WorldPos - Basis.Normal * mult;
                bUnobstructed = bUnobstructed || BSPVisibilityRay(Model, iSurf, SamplePos, TargetLightPos);
            }

            // Commit absolute energies to our shadowed temporary array index ONLY if rays hit
            if (bUnobstructed)
            {
                TempShadowedGrid(PixelIndex).X = AbsoluteColor.X;
                TempShadowedGrid(PixelIndex).Y = AbsoluteColor.Y;
                TempShadowedGrid(PixelIndex).Z = AbsoluteColor.Z;
                TempShadowedGrid(PixelIndex).W = 1.0f;
            }
        } // end loop through x
        // Exit the danger zone for this specific row, allowing unlock sweeps to proceed natively
        GOcclusionInBSP.fetch_sub(1, std::memory_order_release);
    } // end loop through y

    return TotalGridUnoccludedEnergy;
}

static FString SanitizeFilename(const FString& In)
{
    FString Out = In;

    const TCHAR* Illegal = TEXT("\\/:*?\"<>|");
    for (INT i = 0; Illegal[i] != 0; ++i)
    {
        FString BadChar;
        BadChar += Illegal[i];

        Out = Out.Replace(*BadChar, TEXT("_"));
    }

    return Out.Locs();
}

FString GetAtlasNameForLevel(const FString& LevelName)
{
    // Sanitize and lowercase the map name string
    FString Clean = SanitizeFilename(LevelName); // Replaces illegal chars + lowercases

    // Base directory: <SystemDir>/xopengl/lightmaps/
    FString BaseDir = FString(appBaseDir()) + TEXT("xopengl/lightmaps/");

    // Ensure directory exists natively on disk
    GFileManager->MakeDirectory(*BaseDir, true);

    // Return the full path filename completely minus extensions!
    return BaseDir + Clean;
}

#pragma pack(push, 1)

// Strict 80-byte Packed KTX2 Combined Header Layout
struct FKTX2Header
{
    uint8_t  identifier[12];     
    uint32_t vkFormat;           // 137 = VK_FORMAT_BC3_UNORM_BLOCK (DXT5)
    uint32_t typeSize;           // 1
    uint32_t pixelWidth;
    uint32_t pixelHeight;
    uint32_t pixelDepth;
    uint32_t layerCount;
    uint32_t faceCount;
    uint32_t levelCount;
    uint32_t supercompressionScheme;

    uint32_t dfdByteOffset;      // Locked exactly to 104
    uint32_t dfdByteLength;      // Locked exactly to 60

    uint32_t kvdByteOffset;
    uint32_t kvdByteLength;
    uint64_t sgdByteOffset;
    uint64_t sgdByteLength;
};

// Strict 24-byte Level Index Layout
struct FKTX2LevelIndex
{
    uint64_t byteOffset;             // Locked exactly to 176 to satisfy alignment
    uint64_t byteLength;   
    uint64_t uncompressedByteLength; 
};

// Precise, 60-Byte Data Format Descriptor (DFD) Block for BC3
struct FKTX2_DFD_BC3
{
    uint32_t dfdTotalSize;           // 60
    uint32_t vendorAndType;          // 0 (vendorId: 17 bits, descriptorType: 15 bits)
    uint16_t versionNumber;          // 2
    uint16_t descriptorBlockSize;    // 56

    uint8_t  colorModel;             // 130 = KHR_DF_MODEL_BC3
    uint8_t  colorPrimaries;         // 1 = KHR_DF_FLAG_PRIMARIES_BT709
    uint8_t  transferFunction;       // 1 = KHR_DF_TRANSFER_LINEAR
    uint8_t  flagsChannel;           // 0 = KHR_DF_FLAG_ALPHA_STRAIGHT

    // Separated into explicit standalone primitive bytes to bypass compiler array shifts
    uint8_t  texelBlockDimensionW;   // 3 (4 - 1)
    uint8_t  texelBlockDimensionH;   // 3 (4 - 1)
    uint8_t  texelBlockDimensionD;   // 0
    uint8_t  texelBlockDimensionR;   // 0

    uint8_t  bytesPlane0;            // 16 (BC3 payload block allocation size)
    uint8_t  bytesPlane1to7[7];      // All remaining planes initialized cleanly to 0

    // Exactly 2 Samples * 4 elements each = 8 elements total (32 bytes)
    uint32_t sampleInfo[8];      
};

#pragma pack(pop)


const BYTE KTX2_Magic_Identifier[12] = { 0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A };

void DumpAtlasToDisk(const FString& AtlasName, const TArray<FPendingLightmap>& PendingLightmaps)
{
    if (AtlasW <= 0 || AtlasH <= 0 || !bAtlasMapped)
        return;
        
    struct FAtlasEntryBinary
    {
        INT   SurfIndex;
        FLOAT MinU, MaxU;
        FLOAT MinV, MaxV;
        FLOAT AtlasMinU, AtlasMaxU;
        FLOAT AtlasMinV, AtlasMaxV;

        // FIXED STRUCT ENCAPSULATION ENHANCEMENT
        INT   MaskCount;                 // How many DWORD blocks follow in memory
        DWORD LocalRejectionMasks[0];    // Flexible Array Member: maps memory contiguously!
    };

    FString OutKTX2Path = AtlasName;
    OutKTX2Path += TEXT(".ktx2");    
    FString TargetScratchFile = AtlasName;
    TargetScratchFile += TEXT("_scratch.tmp");
    TargetScratchFile = TargetScratchFile.Replace(TEXT("/"), TEXT("\\"));

    FArchive* Ar = GFileManager->CreateFileWriter(*OutKTX2Path);
    if (!Ar) 
        return;

    // --- STAGE A: CALCULATE DYNAMIC KVD VALUE LENGTHS ---
    const char* KvdKey = "UE1_AtlasMetadata";
    uint32_t KeyByteLength = 18; 
    
    // Calculate ValueByteLength using your unified struct footprint rules
    uint32_t ValueByteLength = 0;
    for (INT i = 0; i < PendingLightmaps.Num(); ++i)
    {
        INT SurfID = PendingLightmaps(i).SurfIndex;
        
        // Base size of your unified structural header fields (36 bytes + 4 bytes for MaskCount)
        ValueByteLength += sizeof(FAtlasEntryBinary);
        ValueByteLength += PendingLightmaps(i).LocalRejectionMasks.Num() * sizeof(DWORD);
    }
    
    // Total KVD Record Sizes and hardware alignment padding offsets stay identical!
    uint32_t RawKvdRecordSize = sizeof(uint32_t) + KeyByteLength + ValueByteLength;
    uint32_t KvdRecordPaddingSize = (4 - (RawKvdRecordSize % 4)) % 4;
    uint32_t TotalKvdBlockLength = RawKvdRecordSize + KvdRecordPaddingSize;

    // --- STAGE B: CALCULATE HARDWARE ALIGNMENT PADDING FOR TEXTURE DATA ---
    // The DFD ends exactly at byte 164. The KVD block starts at 164 and ends at (164 + TotalKvdBlockLength).
    uint64_t PostKvdOffset = 164 + TotalKvdBlockLength;
    
    // Texture data payload blocks must be strictly 16-byte aligned from the file head
    uint64_t TextureAlignmentPaddingSize = (16 - (PostKvdOffset % 16)) % 16;
    uint64_t AbsoluteTexturePayloadOffset = PostKvdOffset + TextureAlignmentPaddingSize;

    // --- 1. Populate and Serialize KTX2 Header Block (80 Bytes) ---
    FKTX2Header Header = {};
    appMemcpy(Header.identifier, KTX2_Magic_Identifier, 12);
    Header.vkFormat = 137; // VK_FORMAT_BC3_UNORM_BLOCK (DXT5)
    Header.typeSize = 1;
    Header.pixelWidth = (uint32_t)AtlasW;
    Header.pixelHeight = (uint32_t)AtlasH;
    Header.faceCount = 1;
    Header.levelCount = 1;
    Header.supercompressionScheme = 0;
    Header.dfdByteOffset = 104; 
    Header.dfdByteLength = sizeof(FKTX2_DFD_BC3);
    
    // Register the exact dictionary offsets inside the master header block
    Header.kvdByteOffset = 164;
    Header.kvdByteLength = TotalKvdBlockLength;
    Ar->Serialize(&Header, sizeof(FKTX2Header));

    // --- 2. Populate and Serialize Level Index Block (24 Bytes) ---
    FKTX2LevelIndex LevelIndex = {};
    // Lock the data offset directly to our dynamically computed 16-byte alignment spot
    LevelIndex.byteOffset = AbsoluteTexturePayloadOffset; 
    LevelIndex.byteLength = (uint64_t)((AtlasW + 3) / 4) * ((AtlasH + 3) / 4) * 16;
    LevelIndex.uncompressedByteLength = LevelIndex.byteLength;
    Ar->Serialize(&LevelIndex, sizeof(FKTX2LevelIndex));

    // --- 3. Populate and Serialize DFD Block (60 Bytes) ---
    FKTX2_DFD_BC3 DFD = {};
    // Fill out your standard Khronos format bytes descriptor states...
    DFD.dfdTotalSize = sizeof(FKTX2_DFD_BC3);
    DFD.descriptorBlockSize = 56;
    DFD.versionNumber = 2;
    DFD.colorModel = 130; // BC3
    DFD.colorPrimaries = 1;
    DFD.transferFunction = 1;
    DFD.bytesPlane0 = 16;
    Ar->Serialize(&DFD, sizeof(FKTX2_DFD_BC3));

    // --- 4. SERIALIZE INLINE KEY-VALUE METADATA BLOCK ---
    // A: Write the record size header (Key bytes + value bytes)
    uint32_t RecordHeaderValue = KeyByteLength + ValueByteLength;
    Ar->Serialize(&RecordHeaderValue, sizeof(uint32_t));
    
    // B: Write the Null-Terminated Dictionary Identification String
    Ar->Serialize((void*)KvdKey, KeyByteLength);

    // C: Stream your raw structs directly into the KTX2 container head
    if (PendingLightmaps.Num() > 0)
    {
        for (INT i = 0; i < PendingLightmaps.Num(); ++i)
        {
            const FPendingLightmap& Pending = PendingLightmaps(i);

            // Populate your unified structure variables cleanly
            FAtlasEntryBinary BaseEntry;
            BaseEntry.SurfIndex = Pending.SurfIndex;
            BaseEntry.MinU      = Pending.MinU;
            BaseEntry.MaxU      = Pending.MaxU;
            BaseEntry.MinV      = Pending.MinV;
            BaseEntry.MaxV      = Pending.MaxV;
            BaseEntry.AtlasMinU = Pending.AtlasMinU;
            BaseEntry.AtlasMaxU = Pending.AtlasMaxU;
            BaseEntry.AtlasMinV = Pending.AtlasMinV;
            BaseEntry.AtlasMaxV = Pending.AtlasMaxV;
        
            // Store the active mask count directly inside the struct element tracker
            BaseEntry.MaskCount = Pending.LocalRejectionMasks.Num();

            // 1. Write out the core structured parameters block (including MaskCount)
            Ar->Serialize(&BaseEntry, sizeof(FAtlasEntryBinary));

            // 2. Immediately write the flexible array elements trailing natively behind it
            if (BaseEntry.MaskCount > 0)
            {
                Ar->Serialize((void*)Pending.LocalRejectionMasks.GetData(), BaseEntry.MaskCount * sizeof(DWORD));
            }
        }
    }

    // D: Write 4-byte structural record padding if necessary
    if (KvdRecordPaddingSize > 0)
    {
        BYTE KvdPad[4] = {0, 0, 0, 0};
        Ar->Serialize(KvdPad, KvdRecordPaddingSize);
    }

    // E: Write the 16-byte GPU hardware alignment stream padding
    if (TextureAlignmentPaddingSize > 0)
    {
        BYTE AlignPad[16] = {0};
        Ar->Serialize(AlignPad, (INT)TextureAlignmentPaddingSize);
    }

    // =========================================================================
    // --- 5. STREAMING AND BC3 ENCODING LOOPS (COMPLETELY UNCHANGED) ---
    // =========================================================================
    BYTE rgba[64]; 
    BYTE bc3[16]; 

    SIZE_T RowStrideBytes = (SIZE_T)AtlasW * sizeof(FPlane);
    SIZE_T Stripe4RowsBytes = RowStrideBytes * 4;

    FlushFileBuffers(MappedAtlas.FileHandle);

    HANDLE LocalReadHandle = CreateFile(
        *TargetScratchFile, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr
    );

    if (LocalReadHandle == INVALID_HANDLE_VALUE)
    {
        Ar->Close(); delete Ar; return;
    }

    TArray<FPlane> LocalStripeCache;
    if (bAtlasMapped) { LocalStripeCache.Add(AtlasW * 4); }

    for (INT by = 0; by < AtlasH; by += 4)
    {
        FPlane* StripeWindow = nullptr;
        if (bAtlasMapped)
        {
            SIZE_T TargetFileOffset = (SIZE_T)by * RowStrideBytes;
            SIZE_T ReadLengthBytes = Stripe4RowsBytes;
            if (TargetFileOffset + ReadLengthBytes > AtlasSizeBytes)
                ReadLengthBytes = AtlasSizeBytes - TargetFileOffset;

            LARGE_INTEGER LiReadOffset; LiReadOffset.QuadPart = (LONGLONG)TargetFileOffset;
            SetFilePointerEx(LocalReadHandle, LiReadOffset, nullptr, FILE_BEGIN);

            DWORD BytesRead = 0;
            ReadFile(LocalReadHandle, &LocalStripeCache(0), ReadLengthBytes, &BytesRead, nullptr);
            StripeWindow = &LocalStripeCache(0);
        }
        else
        {
            StripeWindow = &AtlasData[by * AtlasW];
        }

        for (INT bx = 0; bx < AtlasW; bx += 4)
        {
            for (INT y = 0; y < 4; y++)
            {
                INT LocalY = y;
                if (by + y >= AtlasH) LocalY = (AtlasH - 1) - by;

                for (INT x = 0; x < 4; x++)
                {
                    INT sx = Clamp(bx + x, 0, AtlasW - 1);
                    const FPlane& P = StripeWindow[LocalY * AtlasW + sx];
                    INT idx = (y * 4 + x) * 4;
                    rgba[idx+0] = (BYTE)(Clamp(appFloor(P.X * 255.f + 0.5f), 0, 255));
                    rgba[idx+1] = (BYTE)(Clamp(appFloor(P.Y * 255.f + 0.5f), 0, 255));
                    rgba[idx+2] = (BYTE)(Clamp(appFloor(P.Z * 255.f + 0.5f), 0, 255));
                    rgba[idx+3] = (BYTE)(Clamp(appFloor(P.W * 255.f + 0.5f), 0, 255));
                }
            }
            stb_compress_dxt_block(bc3, rgba, 1, STB_DXT_NORMAL);
            Ar->Serialize(bc3, 16);
        }
    }

    CloseHandle(LocalReadHandle);
    LocalStripeCache.Empty();
    Ar->Close();
    delete Ar;
}

void UXOpenGLRenderDevice::ComputeFinalAtlasUVs(
    FSurfInfo& SI,
    const SurfaceBasis& Basis,
    FLOAT MinU, FLOAT MaxU,
    FLOAT MinV, FLOAT MaxV,
    FLOAT AtlasMinU, FLOAT AtlasMaxU,
    FLOAT AtlasMinV, FLOAT AtlasMaxV)
{
    SI.LightmapUVs.Empty();
    SI.LightmapUVs.AddZeroed(SI.Verts.Num());

    FLOAT InvUSize = 1.0f / (MaxU - MinU);
    FLOAT InvVSize = 1.0f / (MaxV - MinV);

    for (INT i = 0; i < SI.Verts.Num(); i++)
    {
        FVector& P = SI.Verts(i);

        FLOAT U = Basis.TangentU | (P - Basis.Origin);
        FLOAT V = Basis.TangentV | (P - Basis.Origin);

        FLOAT u = (U - MinU) * InvUSize;
        FLOAT v = (V - MinV) * InvVSize;

        FLOAT atlasU = AtlasMinU + u * (AtlasMaxU - AtlasMinU);
        FLOAT atlasV = AtlasMinV + v * (AtlasMaxV - AtlasMinV);

        SI.LightmapUVs(i) = FVector(atlasU, atlasV, 0);
    }
}

INT CDECL Compare(const FPendingLightmap& A, const FPendingLightmap& B)
{
    // Compute total padded footprint area
    INT AreaA = (A.Width  + 2) * (A.Height + 2);
    INT AreaB = (B.Width  + 2) * (B.Height + 2);

    // Primary: Area footprint descending (Packs largest boulders first, leaves gaps for small pebbles)
    if (AreaA < AreaB) return +1;   // B first
    if (AreaA > AreaB) return -1;   // A first

    // Secondary: Height descending (Tie-breaker for identical areas)
    if (A.Height < B.Height) return +1; // B first
    if (A.Height > B.Height) return -1; // A first

    // Tertiary: Static Surface Index ascending (Ensures strict math determinism regardless of array memory layout)
    if (A.SurfIndex < B.SurfIndex) return -1;  // A first
    if (A.SurfIndex > B.SurfIndex) return +1;  // B first

    return 0;
}

inline void ToLowerASCII(const char* src, char* dst)
{
    while (*src)
    {
        char c = *src++;
        if (c >= 'A' && c <= 'Z')
            c = c + ('a' - 'A');
        *dst++ = c;
    }
    *dst = 0;
}

bool IsHighlightTexture(const UTexture* Tex)
{
    if (!Tex)
        return false;

    const char* RawName = TCHAR_TO_ANSI(Tex->GetName());

    char LowerName[64];
    ToLowerASCII(RawName, LowerName);

    // Simple string compare against your highlight list
    return strcmp(LowerName, "sail1") == 0
        || strcmp(LowerName, "sail1a") == 0;
}

inline float Luma(const FPlane& p)
{
    // perceptual luminance
    return 0.299f * p.X + 0.587f * p.Y + 0.114f * p.Z;
}

//scale3x/gimp aa version
void ApplyAntialias(FPlane* pixels, INT W, INT H)
{
    if (W < 3 || H < 3) return;

    // Create a pristine copy of our source data to read from
    TArray<FPlane> original;
    original.AddZeroed(W * H);
    appMemcpy(original.GetData(), pixels, W * H * sizeof(FPlane));

    auto at = [&](INT x, INT y) -> const FPlane&
    {
        return original(Clamp(y, 0, H - 1) * W + Clamp(x, 0, W - 1));
    };

    // Allocate an intermediate 3x3 sub-pixel matrix block
    FPlane subPixels[9];

    for (INT y = 0; y < H; ++y)
    {
        for (INT x = 0; x < W; ++x)
        {
            // --- 1. Fetch the 3x3 Original Neighborhood Layout ---
            // [ A ][ B ][ C ]
            // [ D ][ E ][ F ]
            // [ G ][ H ][ I ]
            const FPlane& A = at(x - 1, y - 1);  const FPlane& B = at(x, y - 1);  const FPlane& C = at(x + 1, y - 1);
            const FPlane& D = at(x - 1, y);      const FPlane& E = at(x, y);      const FPlane& F = at(x + 1, y);
            const FPlane& G = at(x - 1, y + 1);  const FPlane& H = at(x, y + 1);  const FPlane& I = at(x + 1, y + 1);

            // Pre-populate all 9 sub-pixels with the central color E (the Scale3X baseline fallback)
            for (int i = 0; i < 9; ++i) subPixels[i] = E;

            // --- 2. Execute the Strict Scale3X Extrapolation Logic Matrix ---
            // Scale3X maps the 9 sub-pixels (labeled 0 through 8) relative to neighbors.
            // A sub-pixel matches a neighbor ONLY if its opposite cross-neighbors don't match.
            if (D == B && D != H && B != F) subPixels[0] = D; // Top-Left subpixel
            if ((D == B && D != H && B != F && E != A) || (B == F && B != D && F != H && E != C)) subPixels[1] = B; // Top-Center
            if (B == F && B != D && F != H) subPixels[2] = F; // Top-Right

            if ((D == B && D != H && B != F && E != G) || (D == H && D != B && H != F && E != A)) subPixels[3] = D; // Mid-Left
            // subPixels[4] is the center point and ALWAYS remains the original core color E
            if ((B == F && B != D && F != H && E != I) || (H == F && H != D && F != B && E != C)) subPixels[5] = F; // Mid-Right

            if (D == H && D != B && H != F) subPixels[6] = D; // Bottom-Left
            if ((D == H && D != B && H != F && E != G) || (H == F && H != D && F != B && E != I)) subPixels[7] = H; // Bottom-Center
            if (H == F && H != D && F != B) subPixels[8] = F; // Bottom-Right

            // --- 3. Subsample the 9 New Pixels into a Weighted Average ---
            // GIMP typically applies a box or Gaussian weight to downsample Scale3X sub-pixels.
            // Giving the center pixel higher weight preserves core sharpness, while corners smooth the jaggies.
            FPlane sum(0.f, 0.f, 0.f, 0.f);
            
            // Weight Distribution: Center = 4.f, Cross Edges = 2.f, Corners = 1.f
            FLOAT weights[9] = {
                1.0f, 2.0f, 1.0f,
                2.0f, 4.0f, 2.0f,
                1.0f, 2.0f, 1.0f
            };
            FLOAT totalWeight = 16.0f;

            for (INT i = 0; i < 9; ++i)
            {
                sum.X += subPixels[i].X * weights[i];
                sum.Y += subPixels[i].Y * weights[i];
                sum.Z += subPixels[i].Z * weights[i];
                sum.W += subPixels[i].W * weights[i];
            }

            FPlane finalPixel;
            finalPixel.X = sum.X / totalWeight;
            finalPixel.Y = sum.Y / totalWeight;
            finalPixel.Z = sum.Z / totalWeight;
            finalPixel.W = E.W; // Guarantee absolute alpha channel mask protection

            // Write back to our active surface buffer pointer
            pixels[y * W + x] = finalPixel;
        }
    }
}

// fxaa version
/*
void ApplyAntialias(FPlane* pixels, int W, int H)
{
    // Safety check for tiny surfaces
    if (W < 3 || H < 3) return;

    TArray<FPlane> original;
    original.AddZeroed(W * H);
    appMemcpy(original.GetData(), pixels, W * H * sizeof(FPlane));

    auto at = [&](int x, int y) -> const FPlane&
    {
        return original(Clamp(y, 0, H - 1) * W + Clamp(x, 0, W - 1));
    };

    // FXAA Threshold Tuning Constants
    const float FXAA_EDGE_THRESHOLD_MIN = 0.0312f; // Triggers on faint shadows
    const float FXAA_EDGE_THRESHOLD_MAX = 0.1250f; // High-contrast edge sensitivity

    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            const FPlane& M = at(x, y);
            float lumaM = Luma(M);

            // Fetch immediate cross neighbors
            float lumaN = Luma(at(x,   y-1));
            float lumaS = Luma(at(x,   y+1));
            float lumaE = Luma(at(x+1, y));
            float lumaW = Luma(at(x-1, y));

            // Find local luminance range
            float lumaMin = Min(lumaM, Min(Min(lumaN, lumaS), Min(lumaE, lumaW)));
            float lumaMax = Max(lumaM, Max(Max(lumaN, lumaS), Max(lumaE, lumaW)));
            float lumaRange = lumaMax - lumaMin;

            // Early exit if local contrast is too low to avoid blurring flat lighting regions
            if (lumaRange < Max(FXAA_EDGE_THRESHOLD_MIN, lumaMax * FXAA_EDGE_THRESHOLD_MAX))
                continue;

            // Fetch diagonal corner neighbors for edge direction calculation
            float lumaNW = Luma(at(x-1, y-1));
            float lumaNE = Luma(at(x+1, y-1));
            float lumaSW = Luma(at(x-1, y+1));
            float lumaSE = Luma(at(x+1, y+1));

            // Combine lumas to compute structural gradients
            float edgeVert = Abs((lumaNW + lumaSW) - 2.0f * lumaW) +
                             Abs((lumaN  + lumaS)  - 2.0f * lumaM) +
                             Abs((lumaNE + lumaSE) - 2.0f * lumaE);

            float edgeHoriz = Abs((lumaNW + lumaNE) - 2.0f * lumaN) +
                              Abs((lumaW  + lumaE)  - 2.0f * lumaM) +
                              Abs((lumaSW + lumaSE) - 2.0f * lumaS);

            // Determine if the jagged edge line runs horizontally or vertically
            bool isHorizontal = (edgeHoriz >= edgeVert);

            // Calculate directional gradients perpendicular to the edge direction
            float luma1 = isHorizontal ? lumaN : lumaW;
            float luma2 = isHorizontal ? lumaS : lumaE;
            
            float gradient1 = Abs(luma1 - lumaM);
            float gradient2 = Abs(luma2 - lumaM);

            // Trace the highest contrast delta path
            bool is1Sign = gradient1 >= gradient2;
            float subPixelOffset = 0.0f;

            // Compute sub-pixel blend weight using a 3x3 low-pass filter matrix
            float lumaL = (lumaN + lumaS + lumaE + lumaW) * 0.25f;
            float pixelBlend = Max(0.0f, Abs(lumaL - lumaM) / lumaRange);
            subPixelOffset = Clamp(pixelBlend * pixelBlend * 0.75f, 0.0f, 0.5f);

            // Calculate final fractional pixel sampling offset coordinate mapping
            FPlane blended;
            if (isHorizontal)
            {
                const FPlane& Nbr = at(x, y + (is1Sign ? -1 : 1));
                blended.X = Lerp(M.X, Nbr.X, subPixelOffset);
                blended.Y = Lerp(M.Y, Nbr.Y, subPixelOffset);
                blended.Z = Lerp(M.Z, Nbr.Z, subPixelOffset);
            }
            else
            {
                const FPlane& Nbr = at(x + (is1Sign ? -1 : 1), y);
                blended.X = Lerp(M.X, Nbr.X, subPixelOffset);
                blended.Y = Lerp(M.Y, Nbr.Y, subPixelOffset);
                blended.Z = Lerp(M.Z, Nbr.Z, subPixelOffset);
            }
            blended.W = M.W; // Preserve original alpha channel mask

            pixels[y * W + x] = blended;
        }
    }
}
*/
// build an occlusion map for a given surface
void UXOpenGLRenderDevice::ProcessNodeSurface(INT plm, ULevel* Level)
{
    UModel* Model = Level->Model;

    if (plm < 0 || plm >= PendingLightmaps.Num())
        return;

    FPendingLightmap& Pending = PendingLightmaps(plm);
    INT iSurf = Pending.SurfIndex;
    INT W = Pending.Width;
    INT H = Pending.Height;
    FLOAT minU = Pending.MinU;
    FLOAT maxU = Pending.MaxU;
    FLOAT minV = Pending.MinV;
    FLOAT maxV = Pending.MaxV;
    const SurfaceBasis& Basis = Pending.Basis;


    FBspSurf& Surf = Model->Surfs(iSurf);
    AActor* Owner = Surf.Actor;
    bool isMover = (Owner && Owner->IsA(AMover::StaticClass()));

    // Build light list
    TArray<AActor*> Lights;

    if (!isMover)
    {
        // Retrieve stored static light list for this surf (if any)
        if (TArray<AActor*>* StaticLightList = StaticLightsForFacet.Find(iSurf))
        {
            Lights = *StaticLightList;
        }
    }
    else
    {
        // For movers, use all static lights in the level
        Lights = StaticLevelLights;
    }
    if (Lights.Num() == 0)
    {
        // none of these returns should happen (would have prevented being added to PendingLightmaps
        // but just in case, set width and height to 0 so atlas assembly ignores them
        Pending.Width = 0;
        Pending.Height = 0;
        return;
    }

    // Get or create FSurfInfo for this surf
    FSurfInfo* SI = SurfaceInfoMap.Find(iSurf);
    if (!SI)
    {
        Pending.Width = 0;
        Pending.Height = 0;
        return;
    }

    if (SI->Verts.Num() < 3)
    {
        Pending.Width = 0;
        Pending.Height = 0;
        return;
    }

    bool TwoSided = (Surf.PolyFlags & PF_TwoSided) != 0;

    // Build basis once per surf
    if (!SI->HasHDLightmap)
    {
        float USize = Max(0.001f, maxU - minU);
        float VSize = Max(0.001f, maxV - minV);

        // MASTER ACCUMULATION GRIDS FOR LINEAR RADIATION MIXING
        TArray<FPlane> MasterShadowedGrid;
        TArray<FPlane> MasterUnshadowedGrid;
        MasterShadowedGrid.AddZeroed(W * H);
        MasterUnshadowedGrid.AddZeroed(W * H);

        TArray<FPlane> TempShadowedGrid;
        TArray<FPlane> TempUnshadowedGrid;

        TArray<DWORD> LocalRejectionBitmask;
        // Calculate how many DWORD blocks we need to cover this surface's local light list width
        INT NumDwordsNeeded = (Lights.Num() + 31) / 32; 
        LocalRejectionBitmask.AddZeroed(NumDwordsNeeded);

        // Loop lights
        for (INT l = 0; l < Lights.Num(); ++l)
        {
            AActor* Light = Lights(l);
            if (!Light)
                continue;

            FLOAT LightEnergyOnSurface = EvaluateSingleLightContribution(
                Light, iSurf, Level, TwoSided, isMover, W, H, minU, maxU, minV, maxV, Basis,
                TempShadowedGrid, TempUnshadowedGrid
            );

            // --- THE ENERGY-BASED EARLY REJECTION GATE ---
            if (LightEnergyOnSurface > 0.001f)
            {
                // The light contributes physical energy to this surface.
                // Accumulate absolute raw color parameters down into the master buffers!
                for (INT p = 0; p < W * H; ++p)
                {
                    MasterShadowedGrid(p).X   += TempShadowedGrid(p).X;
                    MasterShadowedGrid(p).Y   += TempShadowedGrid(p).Y;
                    MasterShadowedGrid(p).Z   += TempShadowedGrid(p).Z;

                    MasterUnshadowedGrid(p).X += TempUnshadowedGrid(p).X;
                    MasterUnshadowedGrid(p).Y += TempUnshadowedGrid(p).Y;
                    MasterUnshadowedGrid(p).Z += TempUnshadowedGrid(p).Z;
                }
            }
            else if (!isMover)
            {
                // Absolute O(1) Local List Bitmapping
                // We set the bit matching the exact local index of the light.
                INT DwordIdx = l / 32;
                INT BitIdx   = l % 32;
                LocalRejectionBitmask(DwordIdx) |= (1 << BitIdx);
            }
        } // End of outer Lights loop

        // compositing pass: resolve intensities and compile final ratios
        TArray<FPlane> Pixels;
        Pixels.AddZeroed(W * H);

        const float eps = 0.0001f;
        float vanillaLightmapIntensity = 2.0f;
        float hdLightmapIntensity      = 2.0f; 
        float TotalIntensityScale      = hdLightmapIntensity * vanillaLightmapIntensity;

        for (INT p = 0; p < W * H; ++p)
        {
            // Scale absolute radiometric energy totals before performing ratio divisions
            MasterUnshadowedGrid(p).X *= TotalIntensityScale;
            MasterUnshadowedGrid(p).Y *= TotalIntensityScale;
            MasterUnshadowedGrid(p).Z *= TotalIntensityScale;

            MasterShadowedGrid(p).X   *= TotalIntensityScale;
            MasterShadowedGrid(p).Y   *= TotalIntensityScale;
            MasterShadowedGrid(p).Z   *= TotalIntensityScale;

            // Safely extract linear ratio proportions per channel
            float FinalR = (MasterUnshadowedGrid(p).X > eps) ? (MasterShadowedGrid(p).X / MasterUnshadowedGrid(p).X) : 0.0f;
            float FinalG = (MasterUnshadowedGrid(p).Y > eps) ? (MasterShadowedGrid(p).Y / MasterUnshadowedGrid(p).Y) : 0.0f;
            float FinalB = (MasterUnshadowedGrid(p).Z > eps) ? (MasterShadowedGrid(p).Z / MasterUnshadowedGrid(p).Z) : 0.0f;

            FinalR = Clamp(FinalR, 0.0f, 1.0f);
            FinalG = Clamp(FinalG, 0.0f, 1.0f);
            FinalB = Clamp(FinalB, 0.0f, 1.0f);
        
            // Calculate your standard alpha luminance factor exactly as before
            float a = 0.2126f * FinalR + 0.7152f * FinalG + 0.0722f * FinalB;

            Pixels(p) = FPlane(FinalR, FinalG, FinalB, a);
        }

        ApplyAntialias(reinterpret_cast<FPlane*>(Pixels.GetData()), W, H);

        FSurfaceLightmap LM;
        appMemzero(&LM, sizeof(LM));
        SI->HDLightmap = LM;
        SI->HasHDLightmap = true;

        // After Pixels has been filled (W x H)
        INT AtlasWidth  = AtlasW;
        INT AtlasHeight = AtlasH;
        INT DestX = Pending.AtlasX;
        INT DestY = Pending.AtlasY;
        SIZE_T RowStride = (SIZE_T)AtlasWidth * sizeof(FPlane);

        if (bAtlasMapped)
        {
#if _WIN32
            SIZE_T RowBytes = W * sizeof(FPlane);

            // --- UNBROKEN COMPILER-SAFE LOCK ---
            // This safely accepts our custom pointer because it satisfies the FSynchronize inheritance check!
            if (FileWriteMutex)
            {
                FScopeLock Lock(FileWriteMutex); 

                // 1. Copy the interior rows to disk
                for (INT y = 0; y < H; ++y)
                {
                    SIZE_T FileOffset = (SIZE_T)(DestY + y) * RowStride + ((SIZE_T)DestX * sizeof(FPlane));
                    LARGE_INTEGER LiOffset;
                    LiOffset.QuadPart = (LONGLONG)FileOffset;

                    SetFilePointerEx(MappedAtlas.FileHandle, LiOffset, nullptr, FILE_BEGIN);
                    DWORD BytesWritten = 0;
                    WriteFile(MappedAtlas.FileHandle, &Pixels(y * W), RowBytes, &BytesWritten, nullptr);
                }

                // 2. Duplicate top and bottom border rows
                {
                    SIZE_T TopDstOffset = (SIZE_T)(DestY - 1) * RowStride + ((SIZE_T)DestX * sizeof(FPlane));
                    LARGE_INTEGER LiTop;
                    LiTop.QuadPart = (LONGLONG)TopDstOffset;
                    
                    SetFilePointerEx(MappedAtlas.FileHandle, LiTop, nullptr, FILE_BEGIN);
                    DWORD BytesWritten = 0;
                    WriteFile(MappedAtlas.FileHandle, &Pixels(0 * W), RowBytes, &BytesWritten, nullptr);

                    SIZE_T BotDstOffset = (SIZE_T)(DestY + H) * RowStride + ((SIZE_T)DestX * sizeof(FPlane));
                    LARGE_INTEGER LiBot;
                    LiBot.QuadPart = (LONGLONG)BotDstOffset;
                    
                    SetFilePointerEx(MappedAtlas.FileHandle, LiBot, nullptr, FILE_BEGIN);
                    WriteFile(MappedAtlas.FileHandle, &Pixels((H - 1) * W), RowBytes, &BytesWritten, nullptr);
                }

                // 3. Duplicate left and right columns (including borders)
                for (INT y = -1; y < H + 1; ++y)
                {
                    INT Ay = DestY + y;
                    INT ClampY = Clamp(y, 0, H - 1);
                    
                    FPlane BorderPixelLeft = Pixels(ClampY * W + 0);
                    SIZE_T LeftOffset = (SIZE_T)Ay * RowStride + ((SIZE_T)(DestX - 1) * sizeof(FPlane));
                    LARGE_INTEGER LiLeft;
                    LiLeft.QuadPart = (LONGLONG)LeftOffset;
                    
                    SetFilePointerEx(MappedAtlas.FileHandle, LiLeft, nullptr, FILE_BEGIN);
                    DWORD BytesWritten = 0;
                    WriteFile(MappedAtlas.FileHandle, &BorderPixelLeft, sizeof(FPlane), &BytesWritten, nullptr);

                    FPlane BorderPixelRight = Pixels(ClampY * W + (W - 1));
                    SIZE_T RightOffset = (SIZE_T)Ay * RowStride + ((SIZE_T)(DestX + W) * sizeof(FPlane));
                    LARGE_INTEGER LiRight;
                    LiRight.QuadPart = (LONGLONG)RightOffset;
                    
                    SetFilePointerEx(MappedAtlas.FileHandle, LiRight, nullptr, FILE_BEGIN);
                    WriteFile(MappedAtlas.FileHandle, &BorderPixelRight, sizeof(FPlane), &BytesWritten, nullptr);
                }
            } // Lock is cleanly destroyed here, letting the next worker thread take over
#endif
        }
        else
        {
            // --- OLD WAY: Fallback path directly targeting standard heap memory ---
            FPlane* TargetAtlas = nullptr;
            void* LocalViewBase = nullptr; // Track file view for conditional unmapping
            TargetAtlas = AtlasData;

            for (INT y = 0; y < H; ++y)
            {
                FPlane* Dest = &TargetAtlas[(DestY + y) * AtlasWidth + DestX];
                const FPlane* Src  = &Pixels(y * W);
                appMemcpy(Dest, Src, W * sizeof(FPlane));
            }

            // Duplicate top and bottom rows
            {
                FPlane* SrcTop = &TargetAtlas[(DestY + 0) * AtlasWidth + DestX];
                FPlane* DstTop = &TargetAtlas[(DestY - 1) * AtlasWidth + DestX];
                appMemcpy(DstTop, SrcTop, W * sizeof(FPlane));

                FPlane* SrcBot = &TargetAtlas[(DestY + H - 1) * AtlasWidth + DestX];
                FPlane* DstBot = &TargetAtlas[(DestY + H) * AtlasWidth + DestX];
                appMemcpy(DstBot, SrcBot, W * sizeof(FPlane));
            }

            // Duplicate left and right columns (including borders)
            for (INT y = -1; y < H + 1; ++y)
            {
                INT Ay = DestY + y;

                FPlane* SrcL = &TargetAtlas[Ay * AtlasWidth + DestX];
                FPlane* DstL = &TargetAtlas[Ay * AtlasWidth + (DestX - 1)];
                *DstL = *SrcL;

                FPlane* SrcR = &TargetAtlas[Ay * AtlasWidth + (DestX + W - 1)];
                FPlane* DstR = &TargetAtlas[Ay * AtlasWidth + (DestX + W)];
                *DstR = *SrcR;
            }
        }

        // Done with per-surface pixels
        Pixels.Empty();
        Pixels.Shrink();

        Pending.AtlasMinU = float(DestX) / AtlasWidth;
        Pending.AtlasMaxU = float(DestX + W) / AtlasWidth;
        Pending.AtlasMinV = float(DestY) / AtlasHeight;
        Pending.AtlasMaxV = float(DestY + H) / AtlasHeight;

        SI->LightmapBasis        = Basis;

        SI->HDLightmap.AtlasMinU = Pending.AtlasMinU;
        SI->HDLightmap.AtlasMinV = Pending.AtlasMinV;
        SI->HDLightmap.AtlasMaxU = Pending.AtlasMaxU;
        SI->HDLightmap.AtlasMaxV = Pending.AtlasMaxV;
        SI->HDLightmap.SurfMinU  = Pending.MinU;
        SI->HDLightmap.SurfMaxU  = Pending.MaxU;
        SI->HDLightmap.SurfMinV  = Pending.MinV;
        SI->HDLightmap.SurfMaxV  = Pending.MaxV;
        if (SI->IsMover)
            SI->HDLightmap.OriginOffset = SI->LightmapBasis.Origin - SI->Owner->Location;

        SI->LocalRejectionMasks = LocalRejectionBitmask;
        Pending.LocalRejectionMasks = LocalRejectionBitmask;

        ComputeFinalAtlasUVs(*SI,
                                Pending.Basis,
                                Pending.MinU, Pending.MaxU,
                                Pending.MinV, Pending.MaxV,
                                Pending.AtlasMinU, Pending.AtlasMaxU,
                                Pending.AtlasMinV, Pending.AtlasMaxV);

    }
    else {
        return;
    }
}

void FOcclusionJob::Start(UXOpenGLRenderDevice* InOwner, ULevel* Level)
{
    Owner = InOwner;
    LevelAtStart = Level;
    bAbort.store(false, std::memory_order_relaxed);
}

void FOcclusionJob::StartThreads(INT NumThreads)
{
    // join any previous threads if needed (or ensure StopAndJoin was called)
    Threads.clear();
    Threads.reserve(NumThreads);

    ActiveWorkers.store(NumThreads, std::memory_order_relaxed);

    for (INT i = 0; i < NumThreads; ++i)
    {
        Threads.emplace_back([this]()
        {
            WorkerLoop();
        });
    }
}

void FOcclusionJob::WorkerLoop()
{
    // will decrement active threadcount when this function returns, no matter from where
    struct FWorkerGuard
    {
        std::atomic<int>& Counter;
        ~FWorkerGuard() { Counter.fetch_sub(1, std::memory_order_relaxed); }
    } Guard{ ActiveWorkers };

    for (;;)
    {
        // Allow shutdown at any time
        if (bAbort.load(std::memory_order_relaxed))
            return;

        // Wait for a safe frame window
        ULevel* FrameLevel = Owner->GFrameLevel.load(std::memory_order_acquire);

        if (FrameLevel == nullptr)
        {
            // Between frames - pause
            std::this_thread::yield();
            continue;
        }

        if (FrameLevel != LevelAtStart)
        {
            // Level changed - job is no longer valid
            return;
        }

        // Pull next surface index
        int plm = -1;
        {
            std::lock_guard<std::mutex> lock(QueueMutex);
            if (PendingSurfaces.empty()) {
                return;
            }

            plm = PendingSurfaces.front();
            PendingSurfaces.pop();
        }

        // Process the surface safely
        Owner->ProcessNodeSurface(plm, LevelAtStart);

        Owner->ProgressDone.fetch_add(1, std::memory_order_relaxed);
    }
}

void FOcclusionJob::StopAndJoin()
{
    // Tell workers to exit
    bAbort.store(true, std::memory_order_relaxed);

    // Join all threads
    for (auto& T : Threads)
    {
        if (T.joinable())
            T.join();
    }

    Threads.clear();

    // Clear queue
    {
        std::lock_guard<std::mutex> lock(QueueMutex);
        while (!PendingSurfaces.empty())
            PendingSurfaces.pop();
    }
}

struct FSkylineSegment
{
    INT X;      // Starting horizontal pixel position
    INT Width;  // Width of this specific horizon tier
    INT Y;      // Active top height of this segment
};

void DryRunAtlas(INT& OutW, INT& OutH)
{
    if (PendingLightmaps.Num() == 0)
        return;

    // --- STAGE 1: Compute total pixel count and max padded LM width ---
    INT TotalPixels = 0;
    INT MaxLMWidth  = 0;
    for (INT i = 0; i < PendingLightmaps.Num(); ++i)
    {
        FPendingLightmap& LM = PendingLightmaps(i);
        if (LM.Width <= 0 || LM.Height <= 0)
            continue;

        INT PaddedW = LM.Width  + 2;
        INT PaddedH = LM.Height + 2;

        TotalPixels += PaddedW * PaddedH;
        MaxLMWidth  = Max(MaxLMWidth, PaddedW);
    }

    // Compute an "ideal" square-ish size from total pixels
    float IdealSizeF = appSqrt((FLOAT)TotalPixels);
    INT   IdealSize  = 1;
    while (IdealSize < (INT)IdealSizeF)
        IdealSize <<= 1;

    const INT MinAtlasWidth = 256;

    OutW = IdealSize;
    OutW = Max(OutW, MaxLMWidth);
    OutW = Max(OutW, MinAtlasWidth);

    // --- STAGE 2: SKYLINE PACKING ALGORITHM PASS ---
    // We use a flat TArray to track our horizon segments. It initializes with 
    // a single entry covering the entire width of the atlas at height 0.
    TArray<FSkylineSegment> Skyline;
    FSkylineSegment InitialSegment = { 0, OutW, 0 };
    Skyline.AddItem(InitialSegment);

    INT MaxAtlasHeightReached = 0;

    for (INT i = 0; i < PendingLightmaps.Num(); ++i)
    {
        FPendingLightmap& LM = PendingLightmaps(i);
        if (LM.Width <= 0 || LM.Height <= 0)
            continue;

        const INT PaddedW = LM.Width  + 2;
        const INT PaddedH = LM.Height + 2;

        // Find the absolute best segment to fit this lightmap. 
        // We look for a segment that minimizes the resulting height (Low-Waste heuristic).
        INT BestSegmentIdx = -1;
        INT BestY = 0x7FFFFFFF;

        for (INT j = 0; j < Skyline.Num(); ++j)
        {
            const FSkylineSegment& Seg = Skyline(j);
            
            // Check if the lightmap can fit horizontally starting at this segment's X
            if (Seg.X + PaddedW <= OutW)
            {
                // Find the maximum height of the horizon across the *entire width* of the lightmap
                INT CurrentMaxY = Seg.Y;
                INT WidthEvaluated = Seg.Width;
                INT SearchIdx = j;

                while (WidthEvaluated < PaddedW && SearchIdx < Skyline.Num() - 1)
                {
                    SearchIdx++;
                    const FSkylineSegment& NextSeg = Skyline(SearchIdx);
                    CurrentMaxY = Max(CurrentMaxY, NextSeg.Y);
                    WidthEvaluated += NextSeg.Width;
                }

                // If the spanning width is genuinely sufficient, evaluate its height score
                if (WidthEvaluated >= PaddedW && CurrentMaxY < BestY)
                {
                    BestY = CurrentMaxY;
                    BestSegmentIdx = j;
                }
            }
        }

        // Fallback safety (should never trigger given OutW >= MaxLMWidth bounds checks)
        if (BestSegmentIdx == -1)
        {
            OutW <= 0; OutH = 0; return;
        }

        // Extract the target placement coordinate positions
        INT PlacementX = Skyline(BestSegmentIdx).X;
        INT PlacementY = BestY;

        // Store *pixel* placement (interior, skip your 1px safety border)
        LM.AtlasX = PlacementX + 1;
        LM.AtlasY = PlacementY + 1;

        // Track the global maximum height ceiling reached by the packer
        MaxAtlasHeightReached = Max(MaxAtlasHeightReached, PlacementY + PaddedH);

        // --- STAGE 3: MUTATE SKYLINE HORIZON SPLITS ---
        // Construct our new horizontal tier segment
        FSkylineSegment NewSeg = { PlacementX, PaddedW, PlacementY + PaddedH };

        // Split and update the segment array in place.
        // We find all previous segments covered by our new width and update them.
        INT InsertPos = BestSegmentIdx;
        
        // Remove or resize segments that are entirely covered by the new placement width
        INT WidthRemaining = PaddedW;
        while (InsertPos < Skyline.Num())
        {
            FSkylineSegment& Target = Skyline(InsertPos);
            if (Target.X < NewSeg.X + NewSeg.Width)
            {
                INT RightEdgeOverlap = (Target.X + Target.Width) - (NewSeg.X + NewSeg.Width);
                if (RightEdgeOverlap > 0)
                {
                    // This segment extends past our right edge! Resize it to start where our new block ends.
                    Target.X = NewSeg.X + NewSeg.Width;
                    Target.Width = RightEdgeOverlap;
                    break;
                }
                else
                {
                    // Entirely consumed by the width of the new block, erase it from the horizon list
                    Skyline.Remove(InsertPos);
                }
            }
            else
            {
                break;
            }
        }

        // Insert our clean newly minted horizon tier into the registry slot
        Skyline.Insert(BestSegmentIdx, 1);
        Skyline(BestSegmentIdx) = NewSeg;

        // Clean up: Merge any adjacent segments sharing identical heights to keep the array tiny
        for (INT k = 0; k < Skyline.Num() - 1; ++k)
        {
            if (Skyline(k).Y == Skyline(k + 1).Y)
            {
                Skyline(k).Width += Skyline(k + 1).Width;
                Skyline.Remove(k + 1);
                k--; // Re-evaluate index point
            }
        }
    }

    OutH = MaxAtlasHeightReached;

    // Round height up to next multiple of 16 to satisfy your BC3 encoding padding requirements
    OutH = ((OutH + 15) / 16) * 16;
    
    Skyline.Empty();
}

void UXOpenGLRenderDevice::BuildPerSurfaceStaticLight(ULevel* Level, const FString& AtlasNameIncoming)
{
    AtlasName = AtlasNameIncoming;
    UModel* Model = Level->Model;

    TUnorderedSet<int> UniqueSurfaces;

    for (INT ni = 0; ni < Model->Nodes.Num(); ++ni)
    {
        INT iSurf = Model->Nodes(ni).iSurf;
        if (iSurf >= 0 && iSurf < Model->Surfs.Num())
            UniqueSurfaces.Set(iSurf);
    }

    PendingLightmaps.Empty();

    INT MaxClamp = 512;
    for (TUnorderedSet<INT>::TIterator It(UniqueSurfaces); It; ++It)
    {
        INT surf = It.Key();
        FBspSurf& Surf = Model->Surfs(surf);
        FSurfInfo* SI = SurfaceInfoMap.Find(surf);
        if (!SI || SI->Verts.Num() < 3 || SI->TriIdx.Num() <= 0)
            continue;

        // Build basis
        SurfaceBasis Basis = BuildSurfaceBasis(SI, Level, Surf);

        // Compute UV extents
        float minU = FLT_MAX, maxU = -FLT_MAX;
        float minV = FLT_MAX, maxV = -FLT_MAX;

        for (INT i = 0; i < SI->Verts.Num(); ++i)
        {
            FVector Local = SI->Verts(i) - Basis.Origin;
            float U = (Basis.TangentU | Local);
            float V = (Basis.TangentV | Local);
            minU = Min(minU, U); maxU = Max(maxU, U);
            minV = Min(minV, V); maxV = Max(maxV, V);
        }

        float USize = Max(0.001f, maxU - minU);
        float VSize = Max(0.001f, maxV - minV);

        const float Density = 0.25f;

        INT W = Clamp(appRound(USize * Density), 8, MaxClamp);
        INT H = Clamp(appRound(VSize * Density), 8, MaxClamp);

        FPendingLightmap LM;
        LM.SurfIndex = surf;
        LM.MinU = minU;
        LM.MaxU = maxU;
        LM.MinV = minV;
        LM.MaxV = maxV;
        LM.Width = W;
        LM.Height = H;
        LM.Basis = Basis;

        PendingLightmaps.AddItem(LM);
    }

    Sort(&PendingLightmaps(0), PendingLightmaps.Num());
    while (true)
    {
        // Dry run
        DryRunAtlas(AtlasW, AtlasH);

        if (AtlasW <= 0 || AtlasH <= 0)
            return;

        if (AtlasW <= 8192 && (INT64)AtlasW * AtlasH <= 43260000) // slightly more than the largest success I have seen (8192x5280)
            break;

        MaxClamp /= 2;
        if (MaxClamp < 8)
            break; // boned

        // Apply clamp
        for (INT i = 0; i < PendingLightmaps.Num(); ++i)
        {
            FPendingLightmap& LM = PendingLightmaps(i);
            LM.Width = Clamp(LM.Width, 8, MaxClamp);
            LM.Height = Clamp(LM.Height, 8, MaxClamp);
        }
    }

    AtlasSizeBytes = (SIZE_T)AtlasW * (SIZE_T)AtlasH * sizeof(FPlane);
    bAtlasMapped = false;

    // Generate a safe scratch file path in your xopengl directory
    FString ScratchFile = (AtlasName + TEXT("_scratch.tmp"));

    // Try memory-mapped disk file
    if (MappedAtlas.Create(AtlasSizeBytes, *ScratchFile))
    {
        // Notice: AtlasData is no longer a persistent global pointer! 
        // Workers and Savers will map their own windowed local pointers instead.
        bAtlasMapped = true;

        // --- INITIALIZE LOCK TRACK ---
#if _WIN32
        if (!FileWriteMutex)
        {
            FileWriteMutex = new FWin32CriticalSection();
        }
#endif
    }
    else
    {
        // Fallback: heap (Keep this in case disk creation fails)
        AtlasData = (FPlane*)appMalloc(AtlasSizeBytes, TEXT("OcclusionAtlas"));
        if (!AtlasData)
        {
            GOcclusionState = EOcclusionState::Failed;
            return;
        }
        appMemzero(AtlasData, AtlasSizeBytes);
    }

    // Fill the job's shared queue
    {
        std::lock_guard<std::mutex> lock(OcclusionJob.QueueMutex);
        while (!OcclusionJob.PendingSurfaces.empty())
            OcclusionJob.PendingSurfaces.pop();

        // Build a temp vector of indices
        std::vector<int> temp;
        temp.reserve(PendingLightmaps.Num());
        for (INT plm = 0; plm < PendingLightmaps.Num(); ++plm)
            temp.push_back(plm);

        // Shuffle the vector
        std::shuffle(temp.begin(), temp.end(),
                     std::mt19937(std::random_device{}()));

        // Refill queue in shuffled order
        for (int idx : temp)
            OcclusionJob.PendingSurfaces.push(idx);
    }

    ProgressTotal = PendingLightmaps.Num();
    ProgressDone.store(0, std::memory_order_relaxed);

    // Start the job (binds owner + level + resets abort)
    OcclusionJob.Start(this, Level);

    // Spawn worker threads (they'll run WorkerLoop())
    int numThreads = std::thread::hardware_concurrency() - 1; // leave room for the game
    OcclusionJob.StartThreads(numThreads);

    GOcclusionState = EOcclusionState::Building;
} // end function BuildPerSurfaceStaticLight

void UXOpenGLRenderDevice::BuildingPoll()
{
    if (OcclusionJob.IsRunning())
    {
        StatusMessage = FString::Printf(
            TEXT("Generating occlusion maps... %d / %d\n(This is a one-time process for this level)"),
            ProgressDone.load(), ProgressTotal
        );
    }
    else
    {
        // Force strict execution synchronization: block until all threads are dead
        OcclusionJob.StopAndJoin();

        // Save off the total and load the final un-raced work count left by the threads
        INT RequiredTotal = PendingLightmaps.Num();
        INT FinalDone     = ProgressDone.load(std::memory_order_acquire);
        
        ProgressTotal = 0;

        // If FinalDone matches the required total, it is a 100% complete run.
        // If it falls short by even a single surface, it was aborted or cut short
        if (RequiredTotal > 0 && FinalDone == RequiredTotal)
        {
            // Genuinely Finished: Run the full asset assembly and disk dumping passes
            StatusMessage   = TEXT("Assembling Atlas");

            AtlasFinished.store(false, std::memory_order_relaxed);
#if _WIN32
            if (bAtlasMapped)
            {
                // Forces the Windows OS file cache to physically commit all multi-threaded
                // WriteFile blocks down to the actual disk sectors all at once.
                FlushFileBuffers(MappedAtlas.FileHandle);
            }
#else
            // Non-Windows platforms use a standard heap allocation (AtlasData),
            // so there is no OS file cache handle that needs flushing here!
#endif

            DumpAtlasToDisk(AtlasName, PendingLightmaps);
            AtlasFinished.store(true, std::memory_order_release);
           
            // Now upload the texture
            // invokes the single master loader. It streams the metadata,
            // configures the UVs, parses the layout, and uploads the data straight to VRAM.
            LoadStaticLightmapAtlas(LastLevel, AtlasName);
            GOcclusionState = EOcclusionState::Ready;
            StatusMessage   = TEXT("");
        }
        else
        {
            GOcclusionState = EOcclusionState::Failed;
        }

        // By placing this right here at the base of the outer else scope, 
        // we guarantee it executes for both finished maps and early aborts
        // except BuildingPoll is not called again if GOcclusionState != EOcclusionState::Building
        // so duplicated some of the cleanup in CleanupOCThreads() :-/
        PendingLightmaps.Empty();  
        
#if _WIN32
        if (bAtlasMapped)
        {
            MappedAtlas.Destroy();
            
            // Fixed path assignment format using our step-by-step operator+=
            FString TargetScratchFile = AtlasName;
            TargetScratchFile += TEXT("_scratch.tmp");
            
            if (FileWriteMutex)
            {
                delete FileWriteMutex; 
                FileWriteMutex = nullptr;
            }
            DeleteFileW(*TargetScratchFile);
        }
#endif
        if (AtlasData)
        {
            appFree(AtlasData);
            AtlasData = nullptr;
        }

        AtlasData      = nullptr;
        AtlasSizeBytes = 0;
        bAtlasMapped   = false; 
    }
}

void UXOpenGLRenderDevice::CleanupOCThreads()
{
    // Stop occlusion job
    OcclusionJob.bAbort.store(true, std::memory_order_seq_cst);
    OcclusionJob.StopAndJoin();

    // Stop any in-flight atlas worker
    if (AtlasThread.joinable())
        AtlasThread.join();
    AtlasFinished.store(false, std::memory_order_relaxed);

    // Now safe to reset state / destroy old atlas / start new build
    GOcclusionState = EOcclusionState::Idle;

    PendingLightmaps.Empty();

#if _WIN32
    if (bAtlasMapped)
    {
        MappedAtlas.Destroy();
            
        // Fixed path assignment format using our step-by-step operator+=
        FString TargetScratchFile = AtlasName;
        TargetScratchFile += TEXT("_scratch.tmp");
            
        if (FileWriteMutex)
        {
            delete FileWriteMutex; 
            FileWriteMutex = nullptr;
        }
        DeleteFileW(*TargetScratchFile);
    }
#endif

    if (AtlasData)
    {
        appFree(AtlasData);
        AtlasData = nullptr;
    }

    AtlasData      = nullptr;
    AtlasSizeBytes = 0;
    bAtlasMapped   = false; 
    OcclusionJob.bAbort.store(false, std::memory_order_relaxed);
}

// Simple UE1-style file-exists helper.
static UBOOL FileExistsUE1(const FString& Path)
{
    return GFileManager->FileSize(*Path) >= 0;
}

UBOOL UXOpenGLRenderDevice::LoadStaticLightmapAtlas(ULevel* Level, const FString& AtlasName)
{
    // Clean up any existing atlas (belt-and-suspenders; NewLevelOC also does this)
    if (GStaticLightmapAtlasHandle != 0)
    {
        glMakeTextureHandleNonResidentARB(GStaticLightmapAtlasHandle);
        GStaticLightmapAtlasHandle = 0;
    }

    if (GStaticLightmapAtlasTex != 0)
    {
        glDeleteTextures(1, &GStaticLightmapAtlasTex);
        GStaticLightmapAtlasTex = 0;
    }

    // --- Clean paths appended directly from the extensionless AtlasName root ---
    FString AtlasKTX2 = AtlasName;
    AtlasKTX2 += TEXT(".ktx2");

    // Unified File Existence Check (Only 1 file required per map now!)
    if (!FileExistsUE1(AtlasKTX2))
    {
        debugf(TEXT("XOpenGL: Static lightmap texture container missing: %s"), *AtlasKTX2);
        return false;
    }

    // --- HIGH-SPEED NATIVE SINGLE-FILE INGEST PASS ---
    FArchive* Ar = GFileManager->CreateFileReader(*AtlasKTX2);
    if (!Ar)
    {
        debugf(TEXT("XOpenGL: Failed to open texture package for metadata parsing: %s"), *AtlasKTX2);
        return false; 
    }

    // 1. Read master 80-byte header block
    FKTX2Header HeaderFile;
    Ar->Serialize(&HeaderFile, sizeof(FKTX2Header));

    // 2. Validate magic numbers and Khronos specification blocks
    if (appMemcmp(HeaderFile.identifier, KTX2_Magic_Identifier, 12) != 0 ||
        HeaderFile.vkFormat != 137 || HeaderFile.levelCount != 1)
    {
        debugf(TEXT("XOpenGL: Corrupt or invalid KTX2 file signature format: %s"), *AtlasKTX2);
        Ar->Close(); delete Ar; return false;
    }

    // 3. Skip past the Level Index metadata table element (24 bytes)
    FKTX2LevelIndex LevelIdxTable;
    Ar->Serialize(&LevelIdxTable, sizeof(FKTX2LevelIndex));

    // 4. JUMP DIRECTLY TO THE EMBEDDED METADATA BLOCK
    // KTX2 spec dictates kvdByteOffset points directly to the record dictionary head
    if (HeaderFile.kvdByteLength <= 0 || HeaderFile.kvdByteOffset == 0)
    {
        debugf(TEXT("XOpenGL: KTX2 file lacks required embedded dictionary data records: %s"), *AtlasKTX2);
        Ar->Close(); delete Ar; return false;
    }
    Ar->Seek((INT)HeaderFile.kvdByteOffset);

    // Read the inline 4-byte record value size indicator block header
    INT KvdRecordSize = 0;
    *Ar << KvdRecordSize;

    // Read the 18-byte Null-Terminated string token key ("UE1_AtlasMetadata\0")
    char ExtractedKey[18];
    Ar->Serialize(ExtractedKey, 18);

    if (strcmp(ExtractedKey, "UE1_AtlasMetadata") != 0)
    {
        debugf(TEXT("XOpenGL: Key mismatch in container header dictionary! Found: %s"), ExtractedKey);
        Ar->Close(); delete Ar; return false;
    }

    // Compute the true length of our struct records allocation block payload size
    struct FAtlasEntry
    {
        INT   SurfIndex;
        FLOAT MinU, MaxU;
        FLOAT MinV, MaxV;
        FLOAT AtlasMinU, AtlasMaxU;
        FLOAT AtlasMinV, AtlasMaxV;
        INT   MaskCount;
        // DWORD LocalRejectionMasks; // Contiguously following on the stream cursor
    };

    uint32_t RealPayloadBytes = KvdRecordSize - 18;

    // --- RUNTIME PLATFORM STRUCT MAPPING ---
    if (RealPayloadBytes > 0 && RealPayloadBytes < 50000000) 
    {
        // Allocate a temporary heap buffer to pull the raw chunk from disk
        TArray<BYTE> RawBlockBuffer;
        RawBlockBuffer.AddZeroed(RealPayloadBytes);
        Ar->Serialize(RawBlockBuffer.GetData(), RealPayloadBytes);

        BYTE* StreamCursor = reinterpret_cast<BYTE*>(RawBlockBuffer.GetData());
        BYTE* StreamEndGate = StreamCursor + RealPayloadBytes;
        INT Entries = 0;

        // Stream unpacker: Read through the variable-width blocks sequentially
        while (StreamCursor < StreamEndGate)
        {
            // 1. Cast the current address directly to your base struct layout
            FAtlasEntry* BaseHeader = reinterpret_cast<FAtlasEntry*>(StreamCursor);
            StreamCursor += sizeof(FAtlasEntry);

            // Fetch our long-lived permanent surface allocation handle
            FSurfInfo* SI = GetSurfInfoByID(BaseHeader->SurfIndex);
            if (SI != nullptr)
            {
                // Hydrate the baseline properties cleanly
                SI->HasHDLightmap = true;

                // RESTORED NATIVE RUNTIME GEOMETRY MATRIX BUILDS
                FBspSurf& Surf = Level->Model->Surfs(BaseHeader->SurfIndex);
                UXOpenGLRenderDevice::SurfaceBasis Basis = BuildSurfaceBasis(SI, Level, Surf);
                SI->LightmapBasis = Basis;

                FSurfaceLightmap& LM = SI->HDLightmap;
                LM.AtlasMinU = BaseHeader->AtlasMinU; 
                LM.AtlasMaxU = BaseHeader->AtlasMaxU;
                LM.AtlasMinV = BaseHeader->AtlasMinV; 
                LM.AtlasMaxV = BaseHeader->AtlasMaxV;
                LM.SurfMinU  = BaseHeader->MinU;       
                LM.SurfMaxU  = BaseHeader->MaxU;
                LM.SurfMinV  = BaseHeader->MinV;       
                LM.SurfMaxV  = BaseHeader->MaxV;
            
                if (SI->IsMover)
                    LM.OriginOffset = SI->LightmapBasis.Origin - SI->Owner->Location;

                // RESTORED NATIVE UV COMPOSITING MATHEMATICS
                ComputeFinalAtlasUVs(*SI, Basis, BaseHeader->MinU, BaseHeader->MaxU, BaseHeader->MinV, BaseHeader->MaxV, BaseHeader->AtlasMinU, BaseHeader->AtlasMaxU, BaseHeader->AtlasMinV, BaseHeader->AtlasMaxV);
            }

            // 2. Extract the flexible array elements trailing natively behind it
            if (BaseHeader->MaskCount > 0)
            {
                if (SI != nullptr)
                {
                    // Dynamic array sizing allocation safety layer
                    SI->LocalRejectionMasks.Empty(BaseHeader->MaskCount);
                    SI->LocalRejectionMasks.AddZeroed(BaseHeader->MaskCount);
                
                    SIZE_T MaskBytesWidth = BaseHeader->MaskCount * sizeof(DWORD);
                    appMemcpy(SI->LocalRejectionMasks.GetData(), StreamCursor, MaskBytesWidth);
                }
            
                // Move stream cursor past the flexible array data block contiguously
                StreamCursor += (BaseHeader->MaskCount * sizeof(DWORD));
            }
            else if (SI != nullptr)
            {
                SI->LocalRejectionMasks.Empty();
            }
            Entries++;
        }
        debugf(TEXT("XOpenGL: Successfully ingested single-file KTX2 lightmap package %s [%d entries loaded]"), *AtlasKTX2, Entries);
    }
    else
    {
        debugf(TEXT("XOpenGL: Corrupt or out-of-bounds dictionary entry allocation size: %u"), RealPayloadBytes);
        Ar->Close(); delete Ar; return false;
    }

    // 5. EXTRACT TEXTURE FOR GPU TRANSITIONS
    // Jump over to the 16-byte aligned hardware payload address target
    Ar->Seek((INT)LevelIdxTable.byteOffset);

    TArray<BYTE> CompressedBuffer;
    CompressedBuffer.AddZeroed((INT)LevelIdxTable.byteLength);
    Ar->Serialize(CompressedBuffer.GetData(), (INT)LevelIdxTable.byteLength);

    // We are completely finished reading from disk! Close the file archive stream cleanly.
    Ar->Close();
    delete Ar;

    // 6. STREAM COMPRESSED PAYLOAD STRAIGHT TO VRAM
    glGenTextures(1, &GStaticLightmapAtlasTex);
    glBindTexture(GL_TEXTURE_2D, GStaticLightmapAtlasTex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glCompressedTexImage2D(
        GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, 
        HeaderFile.pixelWidth, HeaderFile.pixelHeight, 0,
        (INT)LevelIdxTable.byteLength, CompressedBuffer.GetData()
    );

    CompressedBuffer.Empty();

    if (UsingBindlessTextures)
    {
        GStaticLightmapAtlasHandle = glGetTextureHandleARB(GStaticLightmapAtlasTex);
        glMakeTextureHandleResidentARB(GStaticLightmapAtlasHandle);
    }

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) { debugf(TEXT("KTX2 upload GL error: %d"), err); }

    // populate the occlusion aware per surface light list

    // Wipe out any old tracking states from the previous map
    StaticLightsForFacetOC.Empty();

    INT TotalIntersectingLights = 0;
    INT TotalRejectedLights = 0;
    INT TotalSurfacesProcessed = 0;
    INT TotalOcclusionCapableSurfaces = 0;

    for (TMap<INT, TArray<AActor*>>::TIterator It(StaticLightsForFacet); It; ++It)
    {
        INT SurfIndex = It.Key();
        const TArray<AActor*>& StandardList = It.Value();

        // create a fresh, blank destination array in the map
        TArray<AActor*>& FilteredList = StaticLightsForFacetOC.Set(SurfIndex, TArray<AActor*>());

        TotalSurfacesProcessed++;
        TotalIntersectingLights += StandardList.Num();

        FSurfInfo* pSI = GetSurfInfoByID(SurfIndex);
        if (pSI != nullptr && pSI->LocalRejectionMasks.Num() > 0)
        {
            // Pre-allocate memory capacity wide open to prevent incremental reallocations
            FilteredList.Empty(StandardList.Num());

            TotalOcclusionCapableSurfaces++;
            INT SurfRejectedCount = 0;

            for (INT l = 0; l < StandardList.Num(); ++l)
            {
                INT DwordIndex = l / 32;
                INT BitIndex   = l % 32;

                UBOOL bIsOccluded = FALSE;
                if (DwordIndex < pSI->LocalRejectionMasks.Num())
                {
                    // Check if the bit for this specific local index position is a 1
                    if (pSI->LocalRejectionMasks(DwordIndex) & (1 << BitIndex))
                    {
                        bIsOccluded = TRUE; 
                    }
                }

                // Only add the light to the new list if it is NOT occluded!
                if (!bIsOccluded)
                {
                    FilteredList.AddItem(StandardList(l));
                }
                else {
                    SurfRejectedCount++;
                    TotalRejectedLights++;
                }
            }

            // Uncomment this to trace line-item performance on specific hotspots
            // debugf(TEXT("XOpenGL: SurfIndex %5d | Standard Lights: %2d | Kept: %2d | Rejected: %2d"), SurfIndex, StandardList.Num(), FilteredList.Num(), SurfRejectedCount);
            
            // Shrink the array to release unused capacity padding bytes
            FilteredList.Shrink(); 
        }
        else
        {
            // Fallback: If no mask is resident, do a clean, direct full copy
            FilteredList = StandardList;
        }
    }

    /*if (TotalIntersectingLights > 0)
    {
        FLOAT RejectionPct = ((FLOAT)TotalRejectedLights / (FLOAT)TotalIntersectingLights) * 100.0f;
        FLOAT AvgTotalPerSurf = (FLOAT)TotalIntersectingLights / (FLOAT)TotalSurfacesProcessed;
        FLOAT AvgKeptPerSurf = (FLOAT)(TotalIntersectingLights - TotalRejectedLights) / (FLOAT)TotalSurfacesProcessed;

        debugf(TEXT("XOpenGL: ========================================================"));
        debugf(TEXT("XOpenGL: ==== BSP Occlusion Filter Optimization Report ===="));
        debugf(TEXT("XOpenGL: Total Facet Map Surfaces Evaluated:   %d"), TotalSurfacesProcessed);
        debugf(TEXT("XOpenGL: Surfaces with Active Occlusion Masks: %d"), TotalOcclusionCapableSurfaces);
        debugf(TEXT("XOpenGL: Total Lights Touching Surface Bounds: %d (Avg %.2f per surf)"), TotalIntersectingLights, AvgTotalPerSurf);
        debugf(TEXT("XOpenGL: Total Lights Blocked via Raycaster:  %d (Avg %.2f per surf)"), TotalRejectedLights, (FLOAT)TotalRejectedLights / (FLOAT)TotalSurfacesProcessed);
        debugf(TEXT("XOpenGL: Total Lights Passed to Pixel Shaders: %d (Avg %.2f per surf)"), TotalIntersectingLights - TotalRejectedLights, AvgKeptPerSurf);
        debugf(TEXT("XOpenGL: Dynamic Raycast Rejection Ratio:   %.2f%% Fewer Shader Pass Ties!"), RejectionPct);
        debugf(TEXT("XOpenGL: ========================================================"));
    }*/

    return true;
}

void UXOpenGLRenderDevice::NewLevelOC()
{
    NextAllowedMessageTime = 0;

    for (TMap<INT, FSurfInfo>::TIterator It(SurfaceInfoMap); It; ++It)
    {
        FSurfInfo& SI = It.Value();
        SI.HasHDLightmap = false;
    }
    // bake lighting
    glFinish();  // ensure all in-flight draws using old handles are done
    if (GStaticLightmapAtlasHandle != 0)
    {
        glMakeTextureHandleNonResidentARB(GStaticLightmapAtlasHandle);
        GStaticLightmapAtlasHandle = 0;
    }

    if (GStaticLightmapAtlasTex != 0)
    {
        glDeleteTextures(1, &GStaticLightmapAtlasTex);
        GStaticLightmapAtlasTex = 0;
    }

    FString AtlasName = GetAtlasNameForLevel(LastLevel->GetOuter()->GetName());
    // Generate the modern extension for tracking existence on disk
    FString AtlasKTX2 = AtlasName;
    AtlasKTX2 += TEXT(".ktx2");

    // Check if our binary atlas data components are active and ready
    if (FileExistsUE1(AtlasKTX2))
    {
        if (LoadStaticLightmapAtlas(LastLevel, AtlasName))
        {
            GOcclusionState = EOcclusionState::Ready;
            return; // Success!
        }
        else
        {
            GOcclusionState = EOcclusionState::Failed;
        }
    }

    // nothing to load, or failed.  create.  or rather, kick off creation on worker threads and return immediately; when they finish we'll wrap up and assemble
    BuildPerSurfaceStaticLight(LastLevel, AtlasName);
}