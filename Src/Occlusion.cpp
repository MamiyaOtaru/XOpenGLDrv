        
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
bool bAtlasMapped = false;
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
	void StartThreads(int NumThreads);
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

static bool SameSurface(
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

static bool BacktraceEmergesFromOrigin(
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
    bool isMover = (OriginSurf.Actor && OriginSurf.Actor->IsA(AMover::StaticClass()));
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


static bool EmergedFromTransparent(
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
bool UXOpenGLRenderDevice::BSPVisibilityRay(
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

inline FVector GammaLiftLum(const FVector& v, float gamma)
{
    // Perceptual luminance
    float L = v.X * 0.299f + v.Y * 0.587f + v.Z * 0.114f;

    // Gamma-lift luminance
    float Lg = powf(L, 1.0f / gamma);

    // Preserve chroma
    float invL = (L > 0.0001f) ? (1.0f / L) : 0.0f;
    FVector chroma = v * invL;

    return chroma * Lg;
}

// get shadow factor for a single surface point by point by testing visibility to each light and accumulating contribution
FPlane UXOpenGLRenderDevice::EvaluateStaticShadowFactor(
    const TArray<AActor*>& Lights,
    INT iSurf,
    const FVector& WorldPos,
    const SurfaceBasis& Basis,
    UModel* Model,
    bool TwoSided,
    bool isMover)
{
    FPlane Shadowed(0,0,0,0);
    FPlane Unshadowed(0,0,0,0);

    if (Lights.Num() == 0)
        return FPlane(1,1,1,1); // no obstruction

    for (INT i = 0; i < Lights.Num(); ++i)
    {
        AActor* Light = Lights(i);
        if (!Light)
            continue;

        FakeSpotlightPair* SpotData = GetSpotlightData(Light);
        bool bIsSpot = (SpotData != nullptr);

        float Radius = bIsSpot ? SpotData->ReachRadius : Light->WorldLightRadius();
        if (Radius <= 0.f)
            continue;

        // For spotlights, rays emanate from the ceiling fixture (TopLight)
        // For normal point lights, use their own location vector
        FVector TargetLightPos = bIsSpot ? SpotData->TopLight->Location : Light->Location;

        // All vector operations now use TargetLightPos safely!
        FVector L = TargetLightPos - WorldPos;
        float Dist = L.Size();
        if (Dist <= SMALL_NUMBER)
            continue;

        FVector Ldir = L / Dist;
        float NdotL = (Basis.Normal | Ldir);
        if (TwoSided)
            NdotL = fabs(NdotL);
        if (NdotL <= 0.f)
            continue;

        // Match the GPU's linear falloff
        float x = Clamp(Dist / Radius, 0.0f, 1.0f);
        float Atten = 1.0f - x;

        if (Atten <= 0.f)
            continue;

        FPlane RGB;
        if (Light->LightType == LT_TexturePaletteOnce || Light->LightType == LT_TexturePaletteLoop)
        {
            FLOAT AnimatedBrightness = Light->LightBrightness;
            RGB = FPlane(AnimatedBrightness / 255.0f, AnimatedBrightness / 255.0f, AnimatedBrightness / 255.0f, 1.0f);
        }
        else
        {
            // For spotlights, use enhanced brightness profile values from our detection
            FLOAT BaseBrightness = bIsSpot ? (FLOAT)SpotData->Brightness : Light->LightBrightness;
            RGB = FGetHSV(
                Light->LightHue,
                Light->LightSaturation,
                (BYTE)BaseBrightness
            );
        }
        // --- SPOTLIGHT CONE FACTOR ---
        float ConeFactor = 1.0f;
        if (bIsSpot)
        {
            FVector LightDirNorm = (WorldPos - TargetLightPos).SafeNormal();
            float CosAngle = LightDirNorm | SpotData->SpotDirection;

            // Completely outside the spotlight beam? Reject the light loop.
            if (CosAngle < SpotData->SpotCosOuter)
                continue;

            // Penumbra smooth edge interpolation in Pure Cosine Space
            if (CosAngle < SpotData->SpotCosInner)
            {
                float Range = SpotData->SpotCosInner - SpotData->SpotCosOuter;
                float CosineGradient = (CosAngle - SpotData->SpotCosOuter) / Max(Range, 0.001f);
                float BaseCone = Clamp(CosineGradient, 0.0f, 1.0f);

                // --- ZERO-TRANSCENDENTAL CONTRAST SQUEEZE ---
                // Raising the pure cosine gradient to a power of 5 or 6 perfectly
                // counteracts both the cosine distortion and the vertical Lambertian flat-line,
                // matching your acos^4 look at a fraction of the CPU cycle cost!
                ConeFactor = BaseCone * BaseCone * BaseCone * BaseCone * BaseCone * BaseCone; 
            }

            // Apply your uniform distance intensity booster
            ConeFactor *= 2.5f; 
        }

        // Apply all spatial factors
        FVector Color = RGB * NdotL * Atten * ConeFactor;

        // Always accumulate unshadowed
        Unshadowed.X += Color.X;
        Unshadowed.Y += Color.Y;
        Unshadowed.Z += Color.Z;

        // Occlusion test
        float mult = 2.0f;
        if (TwoSided)
            mult = 10.0f;
        if (isMover)
            mult = 2.0f; 

        // Start slightly off the surface toward the light source position
        FVector SamplePos = WorldPos + Basis.Normal * mult;

        // FIX: Trace visibility check line directly back up to the ceiling fixture center!
        bool bUnobstructed = BSPVisibilityRay(Model, iSurf, SamplePos, TargetLightPos);

        if (TwoSided)
        {
            SamplePos = WorldPos - Basis.Normal * mult;
            bUnobstructed = bUnobstructed || BSPVisibilityRay(Model, iSurf, SamplePos, TargetLightPos);
        }

        if (bUnobstructed)
        {
            Shadowed.X += Color.X;
            Shadowed.Y += Color.Y;
            Shadowed.Z += Color.Z;
        }
    }

    // Compute ratio per channel safely
    const float eps = 0.0001f;
    
    float vanillaLightmapIntensity = 2.0;
    float hdLightmapIntensity = 2.0; 
    Unshadowed.X *= hdLightmapIntensity * vanillaLightmapIntensity;   Unshadowed.Y *= hdLightmapIntensity * vanillaLightmapIntensity;   Unshadowed.Z *= hdLightmapIntensity * vanillaLightmapIntensity;
    Shadowed.X   *= hdLightmapIntensity * vanillaLightmapIntensity;   Shadowed.Y   *= hdLightmapIntensity * vanillaLightmapIntensity;   Shadowed.Z   *= hdLightmapIntensity * vanillaLightmapIntensity;

    float FinalR = Shadowed.X / (Unshadowed.X + eps);
    float FinalG = Shadowed.Y / (Unshadowed.Y + eps);
    float FinalB = Shadowed.Z / (Unshadowed.Z + eps);

    FinalR = Clamp(FinalR, 0.0f, 1.0f);
    FinalG = Clamp(FinalG, 0.0f, 1.0f);
    FinalB = Clamp(FinalB, 0.0f, 1.0f);
    
    float a = 0.2126f*FinalR + 0.7152f*FinalG + 0.0722f*FinalB;

    return FPlane(FinalR, FinalG, FinalB, a);
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
    };

    FString OutKTX2Path = AtlasName;
    OutKTX2Path += TEXT(".ktx2");    
    FString TargetScratchFile = AtlasName;
    TargetScratchFile += TEXT("_scratch.tmp");
    TargetScratchFile = TargetScratchFile.Replace(TEXT("/"), TEXT("\\"));

    FArchive* Ar = GFileManager->CreateFileWriter(*OutKTX2Path);
    if (!Ar) 
        return;

    // --- STAGE A: CALCULATE DYNAMIC KVD KEY/VALUE LENGTHS ---
    const char* KvdKey = "UE1_AtlasMetadata";
    uint32_t KeyByteLength = 18; // Length of "UE1_AtlasMetadata" + 1 null terminator
    uint32_t ValueByteLength = (uint32_t)PendingLightmaps.Num() * sizeof(FAtlasEntryBinary);
    
    // Total space for the key, value, and the length header itself
    uint32_t RawKvdRecordSize = sizeof(uint32_t) + KeyByteLength + ValueByteLength;
    
    // KTX2 requires every individual KVD record to be 4-byte aligned
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
        TArray<FAtlasEntryBinary> BinaryBlock;
        BinaryBlock.AddZeroed(PendingLightmaps.Num());

        for (INT i = 0; i < PendingLightmaps.Num(); ++i)
        {
            const FPendingLightmap& LM = PendingLightmaps(i);
            BinaryBlock(i).SurfIndex  = LM.SurfIndex;
            BinaryBlock(i).MinU       = LM.MinU;
            BinaryBlock(i).MaxU       = LM.MaxU;
            BinaryBlock(i).MinV       = LM.MinV;
            BinaryBlock(i).MaxV       = LM.MaxV;
            BinaryBlock(i).AtlasMinU  = LM.AtlasMinU;
            BinaryBlock(i).AtlasMaxU  = LM.AtlasMaxU;
            BinaryBlock(i).AtlasMinV  = LM.AtlasMinV;
            BinaryBlock(i).AtlasMaxV  = LM.AtlasMaxV;
        }
        Ar->Serialize(&BinaryBlock(0), ValueByteLength);
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
    float MinU, float MaxU,
    float MinV, float MaxV,
    float AtlasMinU, float AtlasMaxU,
    float AtlasMinV, float AtlasMaxV)
{
    SI.LightmapUVs.Empty();
    SI.LightmapUVs.AddZeroed(SI.Verts.Num());

    float InvUSize = 1.0f / (MaxU - MinU);
    float InvVSize = 1.0f / (MaxV - MinV);

    for (INT i = 0; i < SI.Verts.Num(); i++)
    {
        FVector& P = SI.Verts(i);

        float U = Basis.TangentU | (P - Basis.Origin);
        float V = Basis.TangentV | (P - Basis.Origin);

        float u = (U - MinU) * InvUSize;
        float v = (V - MinV) * InvVSize;

        float atlasU = AtlasMinU + u * (AtlasMaxU - AtlasMinU);
        float atlasV = AtlasMinV + v * (AtlasMaxV - AtlasMinV);

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

//scalefx version
void ApplyAntialias(FPlane* pixels, int W, int H)
{
    // ScaleFX demands a minimum 7x7 neighborhood to track long shallow slopes
    if (W < 7 || H < 7) return;

    SIZE_T PixelCount = (SIZE_T)W * H;
    
    // Allocate our temporary pass buffer natively
    TArray<FPlane> original;
    original.AddZeroed(PixelCount);
    appMemcpy(original.GetData(), pixels, PixelCount * sizeof(FPlane));

    // Allocate an explicit vector tracking matrix array for Pass 1 data
    // X = Horizontal Gradient, Y = Vertical Gradient, Z = Local Contrast Range
    TArray<FPlane> EdgeVectors;
    EdgeVectors.AddZeroed(PixelCount);

    auto at = [&](int x, int y) -> const FPlane&
    {
        return original(Clamp(y, 0, H - 1) * W + Clamp(x, 0, W - 1));
    };

    const float SCALEFX_THRESHOLD = 0.08f; // Triggers easily on soft shadow transitions

    // ==========================================
    // PASS 1: LONG-RANGE EDGE VECTOR ANALYSIS
    // ==========================================
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            float m = Luma(at(x, y));

            // Sample a broad 7x7 cross layout pattern to calculate long-range vectors
            float l3 = Luma(at(x-3, y)); float l2 = Luma(at(x-2, y)); float l1 = Luma(at(x-1, y));
            float r3 = Luma(at(x+3, y)); float r2 = Luma(at(x+2, y)); float r1 = Luma(at(x+1, y));
            float t3 = Luma(at(x, y-3)); float t2 = Luma(at(x, y-2)); float t1 = Luma(at(x, y-1));
            float b3 = Luma(at(x, y+3)); float b2 = Luma(at(x, y+2)); float b1 = Luma(at(x, y+1));

            // Compute ScaleFX directional gradients
            float gradH = (r1 - l1) * 4.0f + (r2 - l2) * 2.0f + (r3 - l3);
            float gradV = (b1 - t1) * 4.0f + (b2 - t2) * 2.0f + (b3 - t3);

            float lumaMin = Min(m, Min(Min(Min(l1, r1), Min(t1, b1)), Min(Min(l2, r2), Min(t2, b2))));
            float lumaMax = Max(m, Max(Max(Max(l1, r1), Max(t1, b1)), Max(Max(l2, r2), Max(t2, b2))));
            float range   = lumaMax - lumaMin;

            // Store the vector properties safely inside our tracking array
            FPlane& EV = EdgeVectors(y * W + x);
            EV.X = gradH;
            EV.Y = gradV;
            EV.Z = range;
        }
    }

    // ==========================================
    // PASS 2: STRAIGHT LINE SUBPIXEL BLENDING
    // ==========================================
    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            const FPlane& EV = EdgeVectors(y * W + x);
            float contrastRange = EV.Z;

            // Bypass flat areas instantly to preserve core lightmap sharpness
            if (contrastRange < SCALEFX_THRESHOLD)
                continue;

            float gradH = EV.X;
            float gradV = EV.Y;

            // Calculate the exact mathematical angle of the straight shadow line
            float absGradH = Abs(gradH);
            float absGradV = Abs(gradV);
            float sumGrad  = absGradH + absGradV;

            if (sumGrad < 0.001f) continue;

            // Determine fractional vector components for blending
            float weightH = absGradH / sumGrad;
            float weightV = absGradV / sumGrad;

            // Trace directions
            int stepX = (gradH > 0.f) ? 1 : -1;
            int stepY = (gradV > 0.f) ? 1 : -1;

            // Fetch cross-boundary samples along the perpendicular vector path
            const FPlane& center = at(x, y);
            const FPlane& sideH  = at(x + stepX, y);
            const FPlane& sideV  = at(x, y + stepY);
            const FPlane& diag   = at(x + stepX, y + stepY);

            // Execute a true linear sub-pixel interpolation match
            // This is ScaleFX's exact line-smoothing trick: it uses the calculated
            // gradient angles to blend smoothly across shallow steps like an 8x1 line.
            FPlane blended;
            blended.X = center.X * (1.0f - weightH * 0.5f - weightV * 0.5f) +
                        sideH.X  * (weightH * 0.35f) +
                        sideV.X  * (weightV * 0.35f) +
                        diag.X   * (weightH * 0.15f + weightV * 0.15f);

            blended.Y = center.Y * (1.0f - weightH * 0.5f - weightV * 0.5f) +
                        sideH.Y  * (weightH * 0.35f) +
                        sideV.Y  * (weightV * 0.35f) +
                        diag.Y   * (weightH * 0.15f + weightV * 0.15f);

            blended.Z = center.Z * (1.0f - weightH * 0.5f - weightV * 0.5f) +
                        sideH.Z  * (weightH * 0.35f) +
                        sideV.Z  * (weightV * 0.35f) +
                        diag.Z   * (weightH * 0.15f + weightV * 0.15f);

            blended.W = center.W; // Absolute alpha channel mask protection

            // Write the final anti-aliased pixels back to the main buffer
            pixels[y * W + x] = blended;
        }
    }

    // Clean up temporary workspace structures
    EdgeVectors.Empty();
    original.Empty();
}

//scale3x/gimp aa version
/*
void ApplyAntialias(FPlane* pixels, int W, int H)
{
    if (W < 3 || H < 3) return;

    // Create a pristine copy of our source data to read from
    TArray<FPlane> original;
    original.AddZeroed(W * H);
    appMemcpy(original.GetData(), pixels, W * H * sizeof(FPlane));

    auto at = [&](int x, int y) -> const FPlane&
    {
        return original(Clamp(y, 0, H - 1) * W + Clamp(x, 0, W - 1));
    };

    // Allocate an intermediate 3x3 sub-pixel matrix block
    FPlane subPixels[9];

    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
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
            float weights[9] = {
                1.0f, 2.0f, 1.0f,
                2.0f, 4.0f, 2.0f,
                1.0f, 2.0f, 1.0f
            };
            float totalWeight = 16.0f;

            for (int i = 0; i < 9; ++i)
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
*/

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
void UXOpenGLRenderDevice::ProcessNodeSurface(int plm, ULevel* Level)
{
    UModel* Model = Level->Model;

    if (plm < 0 || plm >= PendingLightmaps.Num())
        return;

    FPendingLightmap& Pending = PendingLightmaps(plm);
    int iSurf = Pending.SurfIndex;
    int W = Pending.Width;
    int H = Pending.Height;
    float minU = Pending.MinU;
    float maxU = Pending.MaxU;
    float minV = Pending.MinV;
    float maxV = Pending.MaxV;
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

        TArray<FPlane> Pixels;
        Pixels.AddZeroed(W * H);

        for (INT y = 0; y < H; ++y)
        {
            // We'll loop here until it's safe to process this row
            for (;;)
            {
                ULevel* FrameLevel = GFrameLevel.load(std::memory_order_acquire);

                // Case 1: engine is between frames: pause on this row
                if (FrameLevel == nullptr)
                {
                    if (OcclusionJob.bAbort.load(std::memory_order_relaxed))
                        return;

                    std::this_thread::yield();
                    continue; // stay on the same y, don't enter BSP, don't touch the level - continue goes back to the for (;;)
                }

                // Case 2: level changed: abort this surface/job
                if (FrameLevel != Level)
                {
                    return;
                }

                // Tentatively enter the danger zone for this row
                GOcclusionInBSP.fetch_add(1, std::memory_order_acquire);

                // Re-check after increment to catch races with Unlock/level change
                FrameLevel = GFrameLevel.load(std::memory_order_acquire);

                // Still the same level: we're good, break out and do the row
                if (FrameLevel == Level)
                    break;

                // Not the same anymore: back out of the danger zone
                GOcclusionInBSP.fetch_sub(1, std::memory_order_release);

                // If it's nullptr now, we just slipped between frames: pause and retry this row
                if (FrameLevel == nullptr)
                {
                    if (OcclusionJob.bAbort.load(std::memory_order_relaxed))
                        return;

                    std::this_thread::yield();
                    continue; // retry same y
                }

                // Otherwise it was a different non-null level: abort
                return;
            }

            for (INT x = 0; x < W; ++x)
            {
                float u = (x + 0.5f) / float(W);
                float v = (y + 0.5f) / float(H);
                float U = minU + u * USize;
                float V = minV + v * VSize;

                FVector WorldPos =
                    Basis.Origin +
                    Basis.TangentU * U +
                    Basis.TangentV * V;

                FPlane Color = EvaluateStaticShadowFactor(Lights, iSurf, WorldPos, Basis, Model, TwoSided, isMover);
                /*if (IsHighlightTexture(Surf.Texture)) {
                    Color.X = 1;
                    Color.Y = 0;
                    Color.Z = 1;
                    Color.W = 1;
                }
                else {
                    Color.X = 0;
                    Color.Y = 0;
                    Color.Z = 0;
                    Color.W = 1;
                }*/
                Pixels(y * W + x) = Color;
            }
            // exit danger zone for row, allow unlock to proceed if waiting
            GOcclusionInBSP.fetch_sub(1, std::memory_order_release);
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

void FOcclusionJob::StartThreads(int NumThreads)
{
    // join any previous threads if needed (or ensure StopAndJoin was called)
    Threads.clear();
    Threads.reserve(NumThreads);

    ActiveWorkers.store(NumThreads, std::memory_order_relaxed);

    for (int i = 0; i < NumThreads; ++i)
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

    int MaxClamp = 512;
    for (TUnorderedSet<int>::TIterator It(UniqueSurfaces); It; ++It)
    {
        int surf = It.Key();
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
        // Fallback: heap (Keep this exactly as you have it just in case disk creation fails)
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

        // =====================================================================
        // --- UNIFIED CLEANUP RUNS REGARDLESS OF ABORT OR COMPLETION ---
        // =====================================================================
        // By placing this right here at the base of the outer else scope, 
        // we guarantee it executes for both finished maps and early aborts
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
    };
    uint32_t RealPayloadBytes = KvdRecordSize - 18;
    INT EntryCount = RealPayloadBytes / sizeof(FAtlasEntry);

    TArray<FAtlasEntry> Entries;
    if (EntryCount > 0 && EntryCount < 100000) 
    {
        Entries.AddZeroed(EntryCount);
        // Bulk-serialize our raw structural data matrices directly out of the texture head!
        Ar->Serialize(&Entries(0), RealPayloadBytes);
    }
    else
    {
        debugf(TEXT("XOpenGL: Corrupt or out-of-bounds dictionary entry allocation size: %d"), EntryCount);
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

    if (Entries.Num() == 0)
    {
        debugf(TEXT("XOpenGL: No valid atlas lightmap allocations mapped inside %s"), *AtlasKTX2);
        return false;
    }

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

    // =========================================================================
    // --- STAGE C: RUNTIME PLATFORM STRUCT MAPPING ---
    // =========================================================================
    for (INT i = 0; i < Entries.Num(); i++)
    {
        const FAtlasEntry& E = Entries(i);
        FSurfInfo* SI = SurfaceInfoMap.Find(E.SurfIndex);
        if (!SI) continue;

        SI->HasHDLightmap = true;

        FBspSurf& Surf = Level->Model->Surfs(E.SurfIndex);
        UXOpenGLRenderDevice::SurfaceBasis Basis = BuildSurfaceBasis(SI, Level, Surf);
        SI->LightmapBasis = Basis;

        FSurfaceLightmap& LM = SI->HDLightmap;
        LM.AtlasMinU = E.AtlasMinU; LM.AtlasMaxU = E.AtlasMaxU;
        LM.AtlasMinV = E.AtlasMinV; LM.AtlasMaxV = E.AtlasMaxV;
        LM.SurfMinU  = E.MinU;       LM.SurfMaxU  = E.MaxU;
        LM.SurfMinV  = E.MinV;       LM.SurfMaxV  = E.MaxV;
        if (SI->IsMover)
            LM.OriginOffset = SI->LightmapBasis.Origin - SI->Owner->Location;

        ComputeFinalAtlasUVs(*SI, Basis, E.MinU, E.MaxU, E.MinV, E.MaxV, E.AtlasMinU, E.AtlasMaxU, E.AtlasMinV, E.AtlasMaxV);
    }

    debugf(TEXT("XOpenGL: Successfully ingested single-file KTX2 lightmap package %s [%d entries loaded]"), *AtlasKTX2, Entries.Num());
    return true;
}

void UXOpenGLRenderDevice::NewLevelOC()
{
    NextAllowedMessageTime = 0;

    // Stop occlusion job
    OcclusionJob.bAbort.store(true, std::memory_order_seq_cst);
    OcclusionJob.StopAndJoin();
    OcclusionJob.bAbort.store(false, std::memory_order_relaxed);

    // Stop any in-flight atlas worker
    if (AtlasThread.joinable())
        AtlasThread.join();
    AtlasFinished.store(false, std::memory_order_relaxed);

    // Now safe to reset state / destroy old atlas / start new build
    GOcclusionState = EOcclusionState::Idle;
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
        // Inside LoadStaticLightmapAtlas, you pass your clean extensionless AtlasName parameter!
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