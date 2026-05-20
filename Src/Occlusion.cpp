        
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

std::thread AtlasThread;
std::atomic<bool> AtlasFinished{false};
TArray<FPlane> Atlas;
FString AtlasPNG = TEXT("");
FString AtlasMeta = TEXT("");
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
    // 2. Node membership: is the hit node one of the origin surface’s nodes?
    //    (Strongest possible identity test.)
    for (INT i = 0; i < A.Nodes.Num(); ++i)
    {
        if (OriginSurf.Nodes(i) == HitNodeIndex)
            return true;
    }

    // 3. Plane equivalence:
    //    Compare the hit node’s plane to ANY node belonging to the origin surface.
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

    // If we've effectively reached Start, we’re done
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

bool BSPVisibilityRay(
    UModel* Model,
    INT OriginSurfIndex,
    const FVector& Start,
    const FVector& End)
{
    FLOAT skipMagnitude = 8.0;

    const FBspSurf& OriginSurf = Model->Surfs(OriginSurfIndex);
    bool isMover = (OriginSurf.Actor && OriginSurf.Actor->IsA(AMover::StaticClass()));

    FVector Dir          = (End - Start).SafeNormal();
    FVector CurrentStart = Start;

    bool bInitialCheck    = true;
    bool bEscapedOrigin   = false;
    bool bLastTranslucent = false;

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
                if (!BacktraceEmergesFromOrigin(Model, Start, CurrentStart, OriginSurfIndex))
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
                if (!BacktraceEmergesFromOrigin(Model, Start, CurrentStart, OriginSurfIndex))
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
                bSameSurface = isMover && Surf.Actor == OriginSurf.Actor;
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

// build an occlusion map for a given surface
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

        float Radius = Light->WorldLightRadius();
        if (Radius <= 0.f)
            continue;

        FVector L = Light->Location - WorldPos;
        float Dist = L.Size();
        if (Dist <= SMALL_NUMBER)
            continue;

        FVector Ldir = L / Dist;
        float NdotL = (Basis.Normal | Ldir);
        if (TwoSided)
            NdotL = fabs(NdotL);
        if (NdotL <= 0.f)
            continue;

        float x = Clamp(Dist / Radius, 0.0f, 1.0f);
        float Atten = (1.f - x) / (1.f + 4.f * x * x);
        if (Atten <= 0.f)
            continue;

        FPlane RGB = FGetHSV(
            Light->LightHue,
            Light->LightSaturation,
            Light->LightBrightness
        );

        FVector Color = RGB * NdotL * Atten;

        // Always accumulate unshadowed
        Unshadowed.X += Color.X;
        Unshadowed.Y += Color.Y;
        Unshadowed.Z += Color.Z;

        // Occlusion test
        float mult = 2.0f;
        if (TwoSided)
            mult = 10.0f;
        if (isMover)
            mult = 2.0f; // 40 for less likely to be in the surface leads to weirdness around edges

        // Start slightly off the surface toward the light
        FVector SamplePos = WorldPos + Basis.Normal * mult;

        bool bUnobstructed = BSPVisibilityRay(Model, iSurf, SamplePos, Light->Location);

        if (TwoSided)
        {
            SamplePos = WorldPos - Basis.Normal * mult;
            bUnobstructed = bUnobstructed || BSPVisibilityRay(Model, iSurf, SamplePos, Light->Location);
        }

        if (bUnobstructed)
        {
            Shadowed.X += Color.X;
            Shadowed.Y += Color.Y;
            Shadowed.Z += Color.Z;
        }
    }

    // Compute ratio per channel
    const float eps = 0.0001f;
    float r = Shadowed.X / (Unshadowed.X + eps);
    float g = Shadowed.Y / (Unshadowed.Y + eps);
    float b = Shadowed.Z / (Unshadowed.Z + eps);

    // Optional alpha = luminance
    float a = 0.2126f*r + 0.7152f*g + 0.0722f*b;

    return FPlane(r, g, b, a);
}

// UNUSED
// gather static lighting contributions for a point on a surface, unoccluded but with distance attenuation and NdotL
FPlane UXOpenGLRenderDevice::EvaluateStaticLighting(
    const TArray<AActor*>* Lights,
    const FVector& WorldPos,
    const SurfaceBasis& Basis,
    UModel* Model)
{
    FPlane Accum(0,0,0,0);

    if (!Lights || Lights->Num() == 0)
        return Accum;

    for (INT i = 0; i < Lights->Num(); ++i)
    {
        AActor* Light = (*Lights)(i);
        if (!Light)
            continue;

        // Same radius as selection path
        float Radius = Light->WorldLightRadius();
        if (Radius <= 0.f)
            continue;

        FVector LightPos = Light->Location;
        FVector L = LightPos - WorldPos;
        float Dist = L.Size();
        if (Dist <= SMALL_NUMBER)
            continue;

        FVector Ldir = L / Dist;

        float NdotL = (Basis.Normal | Ldir);
        if (NdotL <= 0.f)
            continue;

        // BSP occlusion
        FCheckResult Hit;
        UBOOL bUnobstructed = Model->LineCheck(
            Hit,
            nullptr,
            LightPos,
            WorldPos,
            FVector(0,0,0),
            0
        );
        if (!bUnobstructed)
            continue;

        // Same attenuation model as ComputeStaticLightsForFacet
        float x = Clamp(Dist / Radius, 0.0f, 1.0f);
        float Atten = (1.f - x) / (1.f + 4.f * x * x);
        if (Atten <= 0.f)
            continue;

        // HSV -> RGB (same base as your ranking)
        FPlane RGBColor = FGetHSV(Light->LightHue, Light->LightSaturation, Light->LightBrightness);

        FVector Color = RGBColor * NdotL * Atten;

        Accum.X += Color.X;
        Accum.Y += Color.Y;
        Accum.Z += Color.Z;
    }

    return Accum;
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

void GetAtlasPathsForLevel(
    const FString& LevelName,
    FString& OutPNG,
    FString& OutMeta)
{
    // Sanitize and lowercase
    FString Clean = SanitizeFilename(LevelName);  // replaces illegal chars + lowercases

    // Base directory: <SystemDir>/xopengl/lightmaps/
    FString BaseDir = FString(appBaseDir()) + TEXT("xopengl/lightmaps/");

    // Ensure directory exists
    GFileManager->MakeDirectory(*BaseDir, true);

    // Final paths (NO _Atlas)
    OutPNG  = BaseDir + Clean + TEXT(".png");
    OutMeta = BaseDir + Clean + TEXT(".txt");
}

struct PNGWriteContext
{
    FArchive* Ar;
};

void PNGWriteCallback(void* context, void* data, int size)
{
    PNGWriteContext* ctx = (PNGWriteContext*)context;
    ctx->Ar->Serialize(data, size);
}

enum class EDDSType
{
    BC1,
    BC3
};

// ------------------------------------------------------------
// Helper: Extract a 4×4 RGBA block with edge clamping
// ------------------------------------------------------------
void Extract4x4RGBA(BYTE* out, const TArray<BYTE>& src, INT bx, INT by, INT width, INT height)
{
    for (INT y = 0; y < 4; y++)
    {
        INT sy = Clamp(by + y, 0, height - 1);
        for (INT x = 0; x < 4; x++)
        {
            INT sx = Clamp(bx + x, 0, width - 1);
            memcpy(out + (y*4 + x)*4, &src((sy*width + sx)*4), 4);
        }
    }
}

// ------------------------------------------------------------
// DDS header structs
// ------------------------------------------------------------
struct DDS_PIXELFORMAT
{
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwFourCC;
    DWORD dwRGBBitCount;
    DWORD dwRBitMask;
    DWORD dwGBitMask;
    DWORD dwBBitMask;
    DWORD dwABitMask;
};

struct DDS_HEADER
{
    DWORD dwSize;
    DWORD dwFlags;
    DWORD dwHeight;
    DWORD dwWidth;
    DWORD dwPitchOrLinearSize;
    DWORD dwDepth;
    DWORD dwMipMapCount;
    DWORD dwReserved1[11];
    DDS_PIXELFORMAT ddspf;
    DWORD dwCaps;
    DWORD dwCaps2;
    DWORD dwCaps3;
    DWORD dwCaps4;
    DWORD dwReserved2;
};

// ------------------------------------------------------------
// Fill header for BC3 / DXT5
// ------------------------------------------------------------
static void FillDDSHeader(DDS_HEADER& H, INT width, INT height, EDDSType type)
{
    memset(&H, 0, sizeof(H));

    H.dwSize  = 124;
    H.dwFlags = 0x1 | 0x2 | 0x4 | 0x1000; // CAPS | HEIGHT | WIDTH | PIXELFORMAT
    H.dwHeight = height;
    H.dwWidth  = width;

    INT blocksWide  = (width  + 3) / 4;
    INT blocksHigh  = (height + 3) / 4;

    INT blockSize = (type == EDDSType::BC1 ? 8 : 16);
    H.dwPitchOrLinearSize = blocksWide * blocksHigh * blockSize;

    H.ddspf.dwSize  = 32;
    H.ddspf.dwFlags = 0x4; // DDPF_FOURCC

    if (type == EDDSType::BC1)
        H.ddspf.dwFourCC = ('D') | ('X' << 8) | ('T' << 16) | ('1' << 24);
    else
        H.ddspf.dwFourCC = ('D') | ('X' << 8) | ('T' << 16) | ('5' << 24);

    H.dwCaps = 0x1000; // DDSCAPS_TEXTURE
}

// ------------------------------------------------------------
// Main function: write DDS BC3 atlas
// ------------------------------------------------------------
void DumpAtlasToDDS(const TArray<FPlane>& Atlas, INT AtlasWidth, INT AtlasHeight, const FString& AtlasDDS, EDDSType type)
{
    const INT PixelCount = AtlasWidth * AtlasHeight;

    // Convert FPlane (RGBA16F) -> 8-bit RGBA
    TArray<BYTE> RGBA;
    RGBA.AddZeroed(PixelCount * 4);

    for (INT i = 0; i < PixelCount; ++i)
    {
        const FPlane& P = Atlas(i);
        RGBA(i*4 + 0) = BYTE(Clamp(P.X, 0.f, 1.f) * 255.f);
        RGBA(i*4 + 1) = BYTE(Clamp(P.Y, 0.f, 1.f) * 255.f);
        RGBA(i*4 + 2) = BYTE(Clamp(P.Z, 0.f, 1.f) * 255.f);
        RGBA(i*4 + 3) = BYTE(Clamp(P.W, 0.f, 1.f) * 255.f);
    }

    // Open file
    FArchive* Ar = GFileManager->CreateFileWriter(*AtlasDDS);
    if (!Ar)
    {
        debugf(TEXT("XOpenGL: Failed to open DDS for writing: %s"), *AtlasDDS);
        return;
    }

    // Write magic "DDS "
    DWORD magic = 0x20534444;
    Ar->Serialize(&magic, 4);

    // Write header
    DDS_HEADER header;
    FillDDSHeader(header, AtlasWidth, AtlasHeight, type);
    Ar->Serialize(&header, sizeof(header));

    // Compress and write BC1/BC3 blocks
    BYTE rgbaBlock[64];
    BYTE dxtBlock[16]; // max size

    INT stbMode = (type == EDDSType::BC1 ? 0 : 1);
    INT blockSize = (type == EDDSType::BC1 ? 8 : 16);

    for (INT by = 0; by < AtlasHeight; by += 4)
    {
        for (INT bx = 0; bx < AtlasWidth; bx += 4)
        {
            Extract4x4RGBA(rgbaBlock, RGBA, bx, by, AtlasWidth, AtlasHeight);

            stb_compress_dxt_block(dxtBlock, rgbaBlock, stbMode, STB_DXT_NORMAL);

            Ar->Serialize(dxtBlock, blockSize);
        }
    }

    Ar->Close();
    delete Ar;

    debugf(TEXT("XOpenGL: Wrote DDS atlas (BC3): %s"), *AtlasDDS);
}

void DumpAtlasToDisk(
    const TArray<FPlane>& Atlas,
    INT AtlasWidth,
    INT AtlasHeight,
    const FString& AtlasPNG)
{
    if (AtlasWidth <= 0 || AtlasHeight <= 0)
        return;

    // Convert RGBA16F -> 8-bit RGBA
    const INT PixelCount = AtlasWidth * AtlasHeight;

    TArray<BYTE> PNGPixels;
    PNGPixels.AddZeroed(PixelCount * 4);

    debugf(TEXT("XOpenGL: AtlasSize = %dx%d"), AtlasWidth, AtlasHeight);

    for (INT i = 0; i < PixelCount; ++i)
    {
        const FPlane& P = Atlas(i);
        PNGPixels(i*4 + 0) = BYTE(Clamp(P.X, 0.f, 1.f) * 255.f);
        PNGPixels(i*4 + 1) = BYTE(Clamp(P.Y, 0.f, 1.f) * 255.f);
        PNGPixels(i*4 + 2) = BYTE(Clamp(P.Z, 0.f, 1.f) * 255.f);
        PNGPixels(i*4 + 3) = BYTE(Clamp(P.W, 0.f, 1.f) * 255.f);
    }

    // Open file for writing
    PNGWriteContext Ctx;
    Ctx.Ar = GFileManager->CreateFileWriter(*AtlasPNG);

    if (!Ctx.Ar)
    {
        debugf(TEXT("XOpenGL: Failed to open PNG for writing: %s"), *AtlasPNG);
        return;
    }

    // Write PNG using stb_image_write
    INT ok = stbi_write_png_to_func(
        PNGWriteCallback,
        &Ctx,
        AtlasWidth,
        AtlasHeight,
        4,
        PNGPixels.GetData(),
        AtlasWidth * 4
    );

    Ctx.Ar->Close();
    delete Ctx.Ar;

    if (!ok)
        debugf(TEXT("XOpenGL: stbi_write_png_to_func failed"));
    else
        debugf(TEXT("XOpenGL: Wrote PNG atlas: %s"), *AtlasPNG);
}

void DumpAtlasMetadata(
    const TArray<FPendingLightmap>& PendingLightmaps,
    const FString& AtlasMeta)
{
    FArchive* Ar = GFileManager->CreateFileWriter(*AtlasMeta);
    if (!Ar)
    {
        debugf(TEXT("XOpenGL: Failed to write atlas metadata: %s"), *AtlasMeta);
        return;
    }

    for (INT i = 0; i < PendingLightmaps.Num(); ++i)
    {
        const FPendingLightmap& LM = PendingLightmaps(i);

        FString Line = FString::Printf(
            TEXT("%d %f %f %f %f %f %f %f %f %d %d\n"),
            LM.SurfIndex,
            LM.MinU, LM.MaxU,
            LM.MinV, LM.MaxV,
            LM.AtlasMinU, LM.AtlasMaxU,
            LM.AtlasMinV, LM.AtlasMaxV,
            LM.Width, LM.Height
        );

        Ar->Serialize(TCHAR_TO_ANSI(*Line), Line.Len());
    }

    Ar->Close();
    delete Ar;

    debugf(TEXT("XOpenGL: Wrote lightmap metadata: %s"), *AtlasMeta);
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
    // Primary: height descending
    if (A.Height < B.Height) return +1;   // B first
    if (A.Height > B.Height) return -1;   // A first

    // Secondary: surf index ascending
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

// get shadow factor for a single surface point by point by testing visibility to each light and accumulating contribution
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
        // Collect dynamic lights for this facet
        ComputeDynamicLightsForFacet(Level, iSurf, Lights);

        // Append static lights
        if (TArray<AActor*>* StaticLightList = StaticLightsForFacet.Find(iSurf))
        {
            for (INT t = 0; t < StaticLightList->Num(); ++t)
            {
                Lights.AddItem((*StaticLightList)(t));
            }
        }
    }
    else
    {
        // For movers, collect all lights in the level
        for (INT ai = 0; ai < Level->Actors.Num(); ++ai)
        {
            AActor* A = Level->Actors(ai);
            if (A && A->IsA(ALight::StaticClass()))
                Lights.AddItem(A);
        }
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
            // We’ll loop here until it’s safe to process this row
            for (;;)
            {
                ULevel* FrameLevel = GFrameLevel.load(std::memory_order_acquire);

                // Case 1: engine is between frames: pause on this row
                if (FrameLevel == nullptr)
                {
                    if (OcclusionJob.bAbort.load(std::memory_order_relaxed))
                        return;

                    std::this_thread::yield();
                    continue; // stay on the same y, don’t enter BSP, don’t touch the level - continue goes back to the for (;;)
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

                // Still the same level: we’re good, break out and do the row
                if (FrameLevel == Level)
                    break;

                // Not the same anymore: back out of the danger zone
                GOcclusionInBSP.fetch_sub(1, std::memory_order_release);

                // If it’s nullptr now, we just slipped between frames: pause and retry this row
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

        FSurfaceLightmap LM;
        appMemzero(&LM, sizeof(LM));
        SI->HDLightmap = LM;
        SI->HasHDLightmap = true;

        // After Pixels has been filled (W x H)
        INT AtlasWidth  = AtlasW; // class member
        INT AtlasHeight = AtlasH; // class member

        INT DestX = Pending.AtlasX; // interior (already +1 from dry run)
        INT DestY = Pending.AtlasY;

        // Copy interior into atlas
        for (INT y = 0; y < H; ++y)
        {
            FPlane* Dest = &Atlas((DestY + y) * AtlasWidth + DestX);
            FPlane* Src  = &Pixels(y * W);
            appMemcpy(Dest, Src, W * sizeof(FPlane));
        }

        // Duplicate top and bottom rows
        {
            // Top border row: copy from first interior row
            FPlane* SrcTop = &Atlas((DestY + 0) * AtlasWidth + DestX);
            FPlane* DstTop = &Atlas((DestY - 1) * AtlasWidth + DestX);
            appMemcpy(DstTop, SrcTop, W * sizeof(FPlane));

            // Bottom border row: copy from last interior row
            FPlane* SrcBot = &Atlas((DestY + H - 1) * AtlasWidth + DestX);
            FPlane* DstBot = &Atlas((DestY + H) * AtlasWidth + DestX);
            appMemcpy(DstBot, SrcBot, W * sizeof(FPlane));
        }

        // Duplicate left and right columns (including borders)
        for (INT y = -1; y < H + 1; ++y)
        {
            INT Ay = DestY + y;

            // Left border: copy from x = 0
            FPlane* SrcL = &Atlas(Ay * AtlasWidth + DestX);
            FPlane* DstL = &Atlas(Ay * AtlasWidth + (DestX - 1));
            *DstL = *SrcL;

            // Right border: copy from x = W-1
            FPlane* SrcR = &Atlas(Ay * AtlasWidth + (DestX + W - 1));
            FPlane* DstR = &Atlas(Ay * AtlasWidth + (DestX + W));
            *DstR = *SrcR;
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

void DryRunAtlas(INT& OutW, INT& OutH)
{
    if (PendingLightmaps.Num() == 0)
        return;

    // Compute total pixel count and max padded LM width
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

    // Choose atlas width:
    const INT MinAtlasWidth = 256;

    OutW = IdealSize;
    OutW = Max(OutW, MaxLMWidth);
    OutW = Max(OutW, MinAtlasWidth);

    // Dry-run packer to get needed height
    INT SimCursorX   = 0;
    INT SimCursorY   = 0;
    INT SimRowHeight = 0;

    for (INT i = 0; i < PendingLightmaps.Num(); ++i)
    {
        FPendingLightmap& LM = PendingLightmaps(i);
        if (LM.Width <= 0 || LM.Height <= 0)
            continue;

        const INT PaddedW = LM.Width  + 2;
        const INT PaddedH = LM.Height + 2;

        if (SimCursorX + PaddedW > OutW)
        {
            SimCursorX   = 0;
            SimCursorY  += SimRowHeight;
            SimRowHeight = 0;
        }

        // Store *pixel* placement (interior, skip 1px border)
        LM.AtlasX = SimCursorX + 1;
        LM.AtlasY = SimCursorY + 1;

        SimRowHeight = Max(SimRowHeight, PaddedH);
        SimCursorX   += PaddedW;
    }

    OutH = SimCursorY + SimRowHeight;

    // Round height up to next multiple of 16
    OutH = ((OutH + 15) / 16) * 16;
}

void UXOpenGLRenderDevice::BuildPerSurfaceStaticLight(ULevel* Level, const FString& AtlasPNGIncoming, const FString& AtlasMetaIncoming)
{
    AtlasPNG = AtlasPNGIncoming;
    AtlasMeta = AtlasMetaIncoming;
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

    Atlas.Empty();
    Atlas.AddZeroed(AtlasW * AtlasH);

    // Fill the job’s shared queue
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

    // Spawn worker threads (they’ll run WorkerLoop())
    int numThreads = std::thread::hardware_concurrency() - 1; // leave room for the game
    OcclusionJob.StartThreads(numThreads);

    GOcclusionState = EOcclusionState::Building;
} // end function BuildPerSurfaceStaticLight

void UXOpenGLRenderDevice::BuildingPoll()
{
    if (OcclusionJob.IsRunning())
    {
        StatusMessage = FString::Printf(
            TEXT("Generating occlusion maps… %d / %d\n(This is a one-time process for this level)"),
            ProgressDone.load(), ProgressTotal
        );
    }
    else
    {
        OcclusionJob.StopAndJoin();

        ProgressTotal = 0;
        if (PendingLightmaps.Num() > 0)
        {
            StatusMessage   = TEXT("Assembling Atlas");

            AtlasFinished.store(false, std::memory_order_relaxed);

            const FString PNG  = AtlasPNG;
            const FString Meta = AtlasMeta;

            DumpAtlasToDisk(Atlas, AtlasW, AtlasH, AtlasPNG);
            //DumpAtlasToDDS(Atlas, AtlasWidth, AtlasHeight, AtlasPNG, EDDSType::BC1);
            DumpAtlasMetadata(PendingLightmaps, AtlasMeta);
            AtlasFinished.store(true, std::memory_order_release);

            // Now upload the texture
            glGenTextures(1, &GStaticLightmapAtlasTex);
            glBindTexture(GL_TEXTURE_2D, GStaticLightmapAtlasTex);

            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F,
                         AtlasW, AtlasH,
                         0, GL_RGBA, GL_FLOAT,
                         Atlas.GetData());

            glGenerateMipmap(GL_TEXTURE_2D);

            if (UsingBindlessTextures)
            {
                GStaticLightmapAtlasHandle = glGetTextureHandleARB(GStaticLightmapAtlasTex);
                glMakeTextureHandleResidentARB(GStaticLightmapAtlasHandle);
            }

            GOcclusionState = EOcclusionState::Ready;
            StatusMessage   = TEXT("");

            // cleanup
            PendingLightmaps.Empty();  // drops per-lightmap pixel buffers etc.
            Atlas.Empty();             // releases all FPlane elements
            Atlas.Shrink();            // returns excess capacity to the allocator
        }
        else
        {
            GOcclusionState = EOcclusionState::Failed;
        }
    }
}

// Simple UE1-style file-exists helper.
static UBOOL FileExistsUE1(const FString& Path)
{
    return GFileManager->FileSize(*Path) >= 0;
}

bool UXOpenGLRenderDevice::LoadStaticLightmapAtlas(ULevel* Level, const FString& AtlasPNG, const FString& AtlasMeta)
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

    // Make sure files exist
    if (!FileExistsUE1(AtlasMeta) || !FileExistsUE1(AtlasPNG))
    {
        debugf(TEXT("XOpenGL: Atlas files missing: %s / %s"), *AtlasPNG, *AtlasMeta);
        return false;
    }

    // Load metadata text
    FString MetaText;
    if (!appLoadFileToString(MetaText, *AtlasMeta))
    {
        debugf(TEXT("XOpenGL: Failed to load atlas metadata: %s"), *AtlasMeta);
        return false;
    }

    struct FAtlasEntry
    {
        INT   SurfIndex;
        FLOAT MinU, MaxU;
        FLOAT MinV, MaxV;
        FLOAT AtlasMinU, AtlasMaxU;
        FLOAT AtlasMinV, AtlasMaxV;
    };
    TArray<FAtlasEntry> Entries;

    // Split MetaText into lines manually (UE1-style)
    FString Remaining = MetaText;
    while (Remaining.Len() > 0)
    {
        INT NewlinePos = Remaining.InStr(TEXT("\n"));
        FString Line;

        if (NewlinePos == INDEX_NONE)
        {
            Line = Remaining;
            Remaining = TEXT("");
        }
        else
        {
            Line = Remaining.Left(NewlinePos);
            Remaining = Remaining.Mid(NewlinePos + 1);
        }

        if (Line.Len() == 0)
            continue;

        // Parse one line: SurfIndex MinU MaxU MinV MaxV AtlasMinU AtlasMaxU AtlasMinV AtlasMaxV Width Height
        INT SurfIndex = 0;
        FLOAT MinU = 0, MaxU = 0;
        FLOAT MinV = 0, MaxV = 0;
        FLOAT AtlasMinU = 0, AtlasMaxU = 0;
        FLOAT AtlasMinV = 0, AtlasMaxV = 0;
        INT Width = 0, Height = 0;

        INT Parsed = swscanf(
            *Line,
            TEXT("%d %f %f %f %f %f %f %f %f %d %d"),
            &SurfIndex,
            &MinU, &MaxU,
            &MinV, &MaxV,
            &AtlasMinU, &AtlasMaxU,
            &AtlasMinV, &AtlasMaxV,
            &Width, &Height
        );

        if (Parsed == 11)
        {
            FAtlasEntry E;
            E.SurfIndex  = SurfIndex;
            E.MinU  = MinU;
            E.MaxU  = MaxU;
            E.MinV  = MinV;
            E.MaxV  = MaxV;
            E.AtlasMinU  = AtlasMinU;
            E.AtlasMaxU  = AtlasMaxU;
            E.AtlasMinV  = AtlasMinV;
            E.AtlasMaxV  = AtlasMaxV;
            Entries.AddItem(E);
        }
        else
        {
            debugf(TEXT("XOpenGL: Bad metadata line: %s"), *Line);
        }
    }

    if (Entries.Num() == 0)
    {
        debugf(TEXT("XOpenGL: No valid atlas metadata entries in %s"), *AtlasMeta);
        return false;
    }

    // Load PNG as float RGBA via stb_image (or your equivalent)
    int W = 0, H = 0, Comp = 0;
    float* Pixels = stbi_loadf(TCHAR_TO_ANSI(*AtlasPNG), &W, &H, &Comp, 4);
    if (!Pixels)
    {
        debugf(TEXT("XOpenGL: Failed to load atlas PNG: %s"), *AtlasPNG);
        return false;
    }

    debugf(TEXT("XOpenGL: Loaded atlas PNG %s (%dx%d)"), *AtlasPNG, W, H);

    // Upload to GL
    glGenTextures(1, &GStaticLightmapAtlasTex);
    glBindTexture(GL_TEXTURE_2D, GStaticLightmapAtlasTex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA16F,
        W, H,
        0,
        GL_RGBA,
        GL_FLOAT,
        Pixels
    );

    glGenerateMipmap(GL_TEXTURE_2D);

    stbi_image_free(Pixels);

    // Bindless handle
    if (UsingBindlessTextures)
    {
        GStaticLightmapAtlasHandle = glGetTextureHandleARB(GStaticLightmapAtlasTex);
        glMakeTextureHandleResidentARB(GStaticLightmapAtlasHandle);
    }

    // Apply atlas UVs AND reconstruct per-vertex lightmap UVs
    for (INT i = 0; i < Entries.Num(); i++)
    {
        const FAtlasEntry& E = Entries(i);

        FSurfInfo* SI = SurfaceInfoMap.Find(E.SurfIndex);
        if (!SI)
            continue;

        SI->HasHDLightmap = true;

        // Rebuild the UT planar basis (same as bake-time)
        FBspSurf& Surf = Level->Model->Surfs(E.SurfIndex);
        UXOpenGLRenderDevice::SurfaceBasis Basis = BuildSurfaceBasis(SI, Level, Surf);
        SI->LightmapBasis = Basis;

        // Apply atlas rectangle to runtime struct
        FSurfaceLightmap& LM = SI->HDLightmap;
        LM.AtlasMinU = E.AtlasMinU;
        LM.AtlasMaxU = E.AtlasMaxU;
        LM.AtlasMinV = E.AtlasMinV;
        LM.AtlasMaxV = E.AtlasMaxV;
        LM.SurfMinU = E.MinU;
        LM.SurfMaxU = E.MaxU;
        LM.SurfMinV = E.MinV;
        LM.SurfMaxV = E.MaxV;
        if (SI->IsMover)
            LM.OriginOffset = SI->LightmapBasis.Origin - SI->Owner->Location;

        // Reconstruct per-vertex lightmap UVs using MinU/MaxU/MinV/MaxV from metadata
        ComputeFinalAtlasUVs(*SI, Basis, E.MinU, E.MaxU, E.MinV, E.MaxV, E.AtlasMinU, E.AtlasMaxU, E.AtlasMinV, E.AtlasMaxV);
    }


    debugf(TEXT("XOpenGL: Loaded static lightmap atlas %s, entries=%d"), *AtlasPNG, Entries.Num());
    return true;
}

void UXOpenGLRenderDevice::NewLevelOC()
{
    NextAllowedMessageTime = 0;

    // Stop occlusion job
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


    FString AtlasPNG, AtlasMeta;
    GetAtlasPathsForLevel(LastLevel->GetOuter()->GetName(), AtlasPNG, AtlasMeta);
    //FString liff = LastLevel->GetLevelInfo()->Title;
    if (FileExistsUE1(AtlasPNG) && FileExistsUE1(AtlasMeta))
    {
        if (LoadStaticLightmapAtlas(LastLevel, AtlasPNG, AtlasMeta))
        {
            GOcclusionState = EOcclusionState::Ready;
            return; // success
        }
        else
        {
            GOcclusionState = EOcclusionState::Failed;
        }
    }

    // nothing to load, or failed.  create.  or rather, kick off creation on worker threads and return immediately; when they finish we'll wrap up and assemble
    BuildPerSurfaceStaticLight(LastLevel, AtlasPNG, AtlasMeta);
}