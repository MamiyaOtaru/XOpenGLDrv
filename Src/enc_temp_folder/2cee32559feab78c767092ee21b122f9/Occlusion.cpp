
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


UXOpenGLRenderDevice::SurfaceBasis UXOpenGLRenderDevice::BuildSurfaceBasis(UModel* Model, const FBspSurf& Surf)
{
    SurfaceBasis B;

    // Raw axes
    FVector U = Model->Vectors(Surf.vTextureU);
    FVector V = Model->Vectors(Surf.vTextureV);
    FVector N = Model->Vectors(Surf.vNormal);

    // Normalize and orthogonalize
    if (!U.IsNearlyZero())
        U = U.SafeNormal();
    else
        U = FVector(1,0,0);

    if (!V.IsNearlyZero())
        V = V.SafeNormal();
    else
        V = FVector(0,1,0);

    if (!N.IsNearlyZero())
        N = N.SafeNormal();
    else
        N = (U ^ V).SafeNormal();

    // Re-orthogonalize V to U if needed
    V = (N ^ U).SafeNormal();

    B.TangentU = U;
    B.TangentV = V;
    B.Normal   = N;

    // Pure geometric origin: pBase in world space
    B.Origin = Model->Points(Surf.pBase);

    return B;
}

struct FStaticLightContrib
{
    FPlane Shadowed;     // sum_i atten_i * vis_i * color_i
    FPlane Unshadowed;   // sum_i atten_i * color_i
};

// build an occlusion map
FPlane UXOpenGLRenderDevice::EvaluateStaticShadowFactor(
    const TArray<AActor*>* Lights,
    const FVector& WorldPos,
    const SurfaceBasis& Basis,
    UModel* Model)
{
    FPlane Shadowed(0,0,0,0);
    FPlane Unshadowed(0,0,0,0);

    if (!Lights || Lights->Num() == 0)
        return FPlane(1,1,1,1); // fully lit

    for (INT i = 0; i < Lights->Num(); ++i)
    {
        AActor* Light = (*Lights)(i);
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
        FCheckResult Hit;
        UBOOL bUnobstructed = Model->LineCheck(
            Hit, nullptr,
            Light->Location,
            WorldPos,
            FVector(0,0,0),
            0
        );

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

// build a baked lightmap
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

struct FPendingLightmap
{
    INT SurfIndex;

    // Original per-surface LM resolution
    INT Width;
    INT Height;

    // CPU-side pixels (RGBA16F stored in FPlane)
    TArray<FPlane> Pixels;

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
void Extract4x4RGBA(BYTE* out, const TArray<BYTE>& src, int bx, int by, int width, int height)
{
    for (int y = 0; y < 4; y++)
    {
        int sy = Clamp(by + y, 0, height - 1);
        for (int x = 0; x < 4; x++)
        {
            int sx = Clamp(bx + x, 0, width - 1);
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
static void FillDDSHeader(DDS_HEADER& H, int width, int height, EDDSType type)
{
    memset(&H, 0, sizeof(H));

    H.dwSize  = 124;
    H.dwFlags = 0x1 | 0x2 | 0x4 | 0x1000; // CAPS | HEIGHT | WIDTH | PIXELFORMAT
    H.dwHeight = height;
    H.dwWidth  = width;

    int blocksWide  = (width  + 3) / 4;
    int blocksHigh  = (height + 3) / 4;

    int blockSize = (type == EDDSType::BC1 ? 8 : 16);
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
void DumpAtlasToDDS(const TArray<FPlane>& Atlas, int AtlasWidth, int AtlasHeight, const FString& AtlasDDS, EDDSType type)
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

    int stbMode = (type == EDDSType::BC1 ? 0 : 1);
    int blockSize = (type == EDDSType::BC1 ? 8 : 16);

    for (int by = 0; by < AtlasHeight; by += 4)
    {
        for (int bx = 0; bx < AtlasWidth; bx += 4)
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
    int ok = stbi_write_png_to_func(
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
        const FVector& P = SI.Verts(i);

        float U = Basis.TangentU | (P - Basis.Origin);
        float V = Basis.TangentV | (P - Basis.Origin);

        float u = (U - MinU) * InvUSize;
        float v = (V - MinV) * InvVSize;

        float atlasU = AtlasMinU + u * (AtlasMaxU - AtlasMinU);
        float atlasV = AtlasMinV + v * (AtlasMaxV - AtlasMinV);

        SI.LightmapUVs(i) = FVector(atlasU, atlasV, 0);
    }
}

void UXOpenGLRenderDevice::BuildStaticLightmapAtlas(const FString& AtlasPNG, const FString& AtlasMeta)
{
    if (PendingLightmaps.Num() == 0)
        return;

    // Compute total pixel count and max padded LM width
    INT TotalPixels = 0;
    INT MaxLMWidth  = 0;
    for (INT i = 0; i < PendingLightmaps.Num(); ++i)
    {
        FPendingLightmap& LM = PendingLightmaps(i);

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

    INT AtlasWidth = IdealSize;
    AtlasWidth = Max(AtlasWidth, MaxLMWidth);
    AtlasWidth = Max(AtlasWidth, MinAtlasWidth);

    // Dry-run packer to get needed height
    INT SimCursorX   = 0;
    INT SimCursorY   = 0;
    INT SimRowHeight = 0;

    for (INT i = 0; i < PendingLightmaps.Num(); ++i)
    {
        FPendingLightmap& LM = PendingLightmaps(i);

        const INT PaddedW = LM.Width  + 2;
        const INT PaddedH = LM.Height + 2;

        if (SimCursorX + PaddedW > AtlasWidth)
        {
            SimCursorX   = 0;
            SimCursorY  += SimRowHeight;
            SimRowHeight = 0;
        }

        SimRowHeight = Max(SimRowHeight, PaddedH);
        SimCursorX   += PaddedW;
    }

    INT AtlasHeight = SimCursorY + SimRowHeight;

    // Round height up to next multiple of 16
    AtlasHeight = ((AtlasHeight + 15) / 16) * 16;

    debugf(TEXT("XOpenGL: Atlas dims = %dx%d (TotalPixels=%d, MaxLMWidth=%d)"),
           AtlasWidth, AtlasHeight, TotalPixels, MaxLMWidth);

    // Allocate atlas buffer (RGBA16F)
    TArray<FPlane> Atlas;
    Atlas.AddZeroed(AtlasWidth * AtlasHeight);

    // Real row-by-row packer (using padded sizes)
    INT CursorX   = 0;
    INT CursorY   = 0;
    INT RowHeight = 0;

    for (INT i = 0; i < PendingLightmaps.Num(); ++i)
    {
        FPendingLightmap& LM = PendingLightmaps(i);

        const INT PaddedW = LM.Width  + 2;
        const INT PaddedH = LM.Height + 2;

        if (CursorX + PaddedW > AtlasWidth)
        {
            CursorX   = 0;
            CursorY  += RowHeight;
            RowHeight = 0;
        }

        // With the dry-run height, this should never overflow
        if (CursorY + PaddedH > AtlasHeight)
        {
            debugf(TEXT("XOpenGL: Atlas overflow even after dry-run (BUG)"));
            break;
        }

        const INT DestX = CursorX;
        const INT DestY = CursorY;

        // Copy interior
        for (INT y = 0; y < LM.Height; ++y)
        {
            FPlane* Dest = &Atlas((DestY + 1 + y) * AtlasWidth + (DestX + 1));
            FPlane* Src  = &LM.Pixels(y * LM.Width);
            appMemcpy(Dest, Src, LM.Width * sizeof(FPlane));
        }

        // Duplicate top/bottom rows
        {
            FPlane* SrcTop = &Atlas((DestY + 1) * AtlasWidth + (DestX + 1));
            FPlane* DstTop = &Atlas((DestY + 0) * AtlasWidth + (DestX + 1));
            appMemcpy(DstTop, SrcTop, LM.Width * sizeof(FPlane));

            FPlane* SrcBot = &Atlas((DestY + 1 + LM.Height - 1) * AtlasWidth + (DestX + 1));
            FPlane* DstBot = &Atlas((DestY + 1 + LM.Height) * AtlasWidth + (DestX + 1));
            appMemcpy(DstBot, SrcBot, LM.Width * sizeof(FPlane));
        }

        // Duplicate left/right columns
        for (INT y = 0; y < LM.Height + 2; ++y)
        {
            INT Ay = DestY + y;

            FPlane* SrcL = &Atlas(Ay * AtlasWidth + (DestX + 1));
            FPlane* DstL = &Atlas(Ay * AtlasWidth + (DestX + 0));
            *DstL = *SrcL;

            FPlane* SrcR = &Atlas(Ay * AtlasWidth + (DestX + 1 + LM.Width - 1));
            FPlane* DstR = &Atlas(Ay * AtlasWidth + (DestX + 1 + LM.Width));
            *DstR = *SrcR;
        }

        // Store atlas UVs for interior
        LM.AtlasX = DestX + 1;
        LM.AtlasY = DestY + 1;

        LM.AtlasMinU = float(DestX + 1) / AtlasWidth;
        LM.AtlasMaxU = float(DestX + 1 + LM.Width) / AtlasWidth;
        LM.AtlasMinV = float(DestY + 1) / AtlasHeight;
        LM.AtlasMaxV = float(DestY + 1 + LM.Height) / AtlasHeight;

        if (FSurfInfo* SI = SurfaceInfoMap.Find(LM.SurfIndex))
        {
            SI->HDLightmap.AtlasMinU = LM.AtlasMinU;
            SI->HDLightmap.AtlasMinV = LM.AtlasMinV;
            SI->HDLightmap.AtlasMaxU = LM.AtlasMaxU;
            SI->HDLightmap.AtlasMaxV = LM.AtlasMaxV;
            ComputeFinalAtlasUVs(*SI, LM.Basis, LM.MinU, LM.MaxU, LM.MinV, LM.MaxV,
                                 LM.AtlasMinU, LM.AtlasMaxU, LM.AtlasMinV, LM.AtlasMaxV);
        }

        CursorX   += PaddedW;
        RowHeight  = Max(RowHeight, PaddedH);
    }

    // Upload to GL
    glGenTextures(1, &GStaticLightmapAtlasTex);
    glBindTexture(GL_TEXTURE_2D, GStaticLightmapAtlasTex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F,
                 AtlasWidth, AtlasHeight,
                 0, GL_RGBA, GL_FLOAT,
                 Atlas.GetData());

    glGenerateMipmap(GL_TEXTURE_2D);

    if (UseBindlessTextures)
    {
        GStaticLightmapAtlasHandle = glGetTextureHandleARB(GStaticLightmapAtlasTex);
        glMakeTextureHandleResidentARB(GStaticLightmapAtlasHandle);
    }

    DumpAtlasToDisk(Atlas, AtlasWidth, AtlasHeight, AtlasPNG);
    //DumpAtlasToDDS(Atlas, AtlasWidth, AtlasHeight, AtlasPNG, EDDSType::BC1);
    DumpAtlasMetadata(PendingLightmaps, AtlasMeta);
}


INT CDECL Compare(const FPendingLightmap& A, const FPendingLightmap& B)
{
    if (A.Height < B.Height) return +1;  // B first
    if (A.Height > B.Height) return -1;  // A first
    return 0;
}

void UXOpenGLRenderDevice::BuildPerSurfaceStaticLight(ULevel* Level, const FString& AtlasPNG, const FString& AtlasMeta)
{
    UModel* Model = Level->Model;

    PendingLightmaps.Empty();

    for (INT iSurf = 0; iSurf < Model->Surfs.Num(); ++iSurf)
    {
        FBspSurf& Surf = Model->Surfs(iSurf);

        TArray<AActor*>* Lights = StaticLightsForFacet.Find(iSurf);
        if (!Lights || Lights->Num() == 0)
            continue;

        FSurfInfo* SI = SurfaceInfoMap.Find(iSurf);
        if (!SI || SI->Verts.Num() < 3)
            continue;

        SurfaceBasis Basis = BuildSurfaceBasis(Model, Surf);

        // --------------------------------------------
        // Compute extents in OUR UV space:
        // U = dot(TangentU, P - Origin)
        // V = dot(TangentV, P - Origin)
        // --------------------------------------------
        float minU = FLT_MAX, maxU = -FLT_MAX;
        float minV = FLT_MAX, maxV = -FLT_MAX;

        for (INT i = 0; i < SI->Verts.Num(); ++i)
        {
            const FVector& P = SI->Verts(i);
            FVector Local = P - Basis.Origin;

            float U = (Basis.TangentU | Local);
            float V = (Basis.TangentV | Local);

            if (U < minU) minU = U;
            if (U > maxU) maxU = U;
            if (V < minV) minV = V;
            if (V > maxV) maxV = V;
        }

        float USize = Max(0.001f, maxU - minU);
        float VSize = Max(0.001f, maxV - minV);

        // --------------------------------------------
        // Choose resolution based on world-space size
        // --------------------------------------------
        const float Density = 0.25f; // texels per unit, tweak as desired
        INT W = Clamp(appRound(USize * Density), 8, 512);
        INT H = Clamp(appRound(VSize * Density), 8, 512);

        TArray<FPlane> Pixels;
        Pixels.AddZeroed(W * H);

        // --------------------------------------------
        // Bake in OUR UV space
        // --------------------------------------------
        for (INT y = 0; y < H; ++y)
        {
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

                FPlane Color = EvaluateStaticShadowFactor(Lights, WorldPos, Basis, Model);
                Pixels(y * W + x) = Color;
            }
        }

        // No GL upload here — atlas will handle it
        FSurfaceLightmap LM;
        appMemzero(&LM, sizeof(LM));

        SI->HDLightmap = LM;
        SI->HasHDLightmap = true;

        // Prepare for atlas packing
        FPendingLightmap Pending;
        Pending.SurfIndex = iSurf;
        Pending.Width  = W;
        Pending.Height = H;
        Pending.Pixels = Pixels;
        Pending.MinU = minU;
        Pending.MaxU = maxU;
        Pending.MinV = minV;
        Pending.MaxV = maxV;
        Pending.Basis = Basis;

        PendingLightmaps.AddItem(Pending);
    }
    Sort(&PendingLightmaps(0), PendingLightmaps.Num());

    BuildStaticLightmapAtlas(AtlasPNG, AtlasMeta);
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
    if (UseBindlessTextures)
    {
        GStaticLightmapAtlasHandle = glGetTextureHandleARB(GStaticLightmapAtlasTex);
        glMakeTextureHandleResidentARB(GStaticLightmapAtlasHandle);
    }

    // Apply atlas UVs to surfaces
    // Apply atlas UVs AND reconstruct per-vertex lightmap UVs
    for (INT i = 0; i < Entries.Num(); i++)
    {
        const FAtlasEntry& E = Entries(i);

        FSurfInfo* SI = SurfaceInfoMap.Find(E.SurfIndex);
        if (!SI)
            continue;

        SI->HasHDLightmap = true;

        // Apply atlas rectangle to runtime struct
        FSurfaceLightmap& LM = SI->HDLightmap;
        LM.AtlasMinU = E.AtlasMinU;
        LM.AtlasMaxU = E.AtlasMaxU;
        LM.AtlasMinV = E.AtlasMinV;
        LM.AtlasMaxV = E.AtlasMaxV;

        // Rebuild the UT planar basis (same as bake-time)
        FBspSurf& Surf = Level->Model->Surfs(E.SurfIndex);
        UXOpenGLRenderDevice::SurfaceBasis Basis = BuildSurfaceBasis(Level->Model, Surf);

        // Reconstruct per-vertex lightmap UVs using MinU/MaxU/MinV/MaxV from metadata
        ComputeFinalAtlasUVs(*SI, Basis, E.MinU, E.MaxU, E.MinV, E.MaxV, E.AtlasMinU, E.AtlasMaxU, E.AtlasMinV, E.AtlasMaxV);
    }


    debugf(TEXT("XOpenGL: Loaded static lightmap atlas %s, entries=%d"), *AtlasPNG, Entries.Num());
    return true;
}

void UXOpenGLRenderDevice::NewLevelOC()
{
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
    GetAtlasPathsForLevel(LastLevel->GetLevelInfo()->Title, AtlasPNG, AtlasMeta);
    if (FileExistsUE1(AtlasPNG) && FileExistsUE1(AtlasMeta))
    {
        if (LoadStaticLightmapAtlas(LastLevel, AtlasPNG, AtlasMeta))
            return; // success
    }

    // nothing to load, or failed.  create
    BuildPerSurfaceStaticLight(LastLevel, AtlasPNG, AtlasMeta);

}