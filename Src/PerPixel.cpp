#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include "XOpenGLDrv.h"
#include "XOpenGL.h"
#include "ExternalTextureLoader.h"

INT UXOpenGLRenderDevice::GetFacetSurfId(FSceneNode* Frame, const FSurfaceFacet& Facet)
{
    if (!Facet.Polys || !Frame || !Frame->Level || !Frame->Level->Model)
        return INDEX_NONE;

	ULevel* Level = Frame->Level;

    // Walk polys until we find one with a valid iNode
    for (FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next)
    {
        const INT iNode = Poly->iNode;

        // Node index must be valid
        if (iNode < 0 || iNode >= Level->Model->Nodes.Num())
            continue;

		const FBspNode& Node = Level->Model->Nodes(iNode);
		const INT iSurf = Node.iSurf;

        // Surface index must be valid
        if (iSurf < 0 || iSurf >= Level->Model->Surfs.Num())
            continue;

		//const FBspSurf& Surf = Level->Model->Surfs(iSurf);
		//AActor* Owner = Surf.Actor;

		//if (Owner && Owner->IsA(AMover::StaticClass()))
			//return INDEX_NONE;

        return iSurf; // Found a valid surface ID
    }

    return INDEX_NONE; // No valid poly/node/surface found
}

bool IsStaticLight(AActor* A)
{
    if (!A) return false;

    if (A->LightType == LT_None)
        return false;

    if (A->LightBrightness == 0 && A->LightRadius == 0)
        return false;

    // Dynamic lights are excluded
    if (A->bDynamicLight)
        return false;

    // Movable actors cannot be static lights
    if (A->bMovable && !A->bStatic) // Liandri's green teleport light is "movable" but also "static", so we check both flags to be sure
        return false;

    return true;
}

inline bool IsDynamicLight(AActor* A)
{
    if (!A) return false;

    if (A->LightType == LT_None)
        return false;

    if (A->LightBrightness == 0 && A->LightRadius == 0)
        return false;

    if (A->bDynamicLight)
       return true;

    if (A->bMovable && !A->bStatic) // Liandri's green teleport light is "movable" but also "static", so we check both flags to be sure
        return true;

    // Animated light types are dynamic
	/*switch (A->LightType)
	{
		case LT_Pulse:
		case LT_Blink:
		case LT_Flicker:
		case LT_Strobe:
		case LT_SubtlePulse:
		case LT_TexturePaletteOnce:
		case LT_TexturePaletteLoop:
			return true;   // dynamic

		case LT_Steady:
		case LT_BackdropLight:
		default:
			break;         // not dynamic
	}*/

    return false;
}

glm::vec3 WorldToView_Point(const glm::vec3& worldPos, const glm::vec4 FrameCoords[4])
{
    glm::vec3 v = worldPos - glm::vec3(FrameCoords[0]);
    return glm::vec3(
        glm::dot(v, glm::vec3(FrameCoords[1])),
        glm::dot(v, glm::vec3(FrameCoords[2])),
        glm::dot(v, glm::vec3(FrameCoords[3]))
    );
}

struct RankedLight
{
    AActor* Light;
    float   Score;
    bool    IsStatic;
};

INT Compare(const RankedLight& A, const RankedLight& B)
{
    // descending order
    if (A.Score < B.Score) return +1;
    if (A.Score > B.Score) return -1;
    return 0;
}

// Collect world-space vertices for a surface (iSurf)
void UXOpenGLRenderDevice::GetWorldspaceSurfaceVerts(ULevel* Level, INT iSurf, TArray<FVector>& OutVerts)
{
    OutVerts.Empty();

    if (!Level || !Level->Model || iSurf < 0 || iSurf >= Level->Model->Surfs.Num())
        return;

    const FBspSurf& Surf = Level->Model->Surfs(iSurf);

    // Walk all nodes that belong to this surface
    for (INT ni = 0; ni < Surf.Nodes.Num(); ++ni)
    {
        INT iNode = Surf.Nodes(ni);
        if (iNode < 0 || iNode >= Level->Model->Nodes.Num())
            continue;

        const FBspNode& Node = Level->Model->Nodes(iNode);

        // Extract polygon vertices from this node
        for (INT vi = 0; vi < Node.NumVertices; ++vi)
        {
            INT iVert = Node.iVertPool + vi;
            const FVert& V = Level->Model->Verts(iVert);
            const FVector& P = Level->Model->Points(V.pVertex); // world-space
            OutVerts.AddItem(P);
        }
    }
}

UBOOL UXOpenGLRenderDevice::PointInTriangle(const FVector& P, const FVector& A, const FVector& B, const FVector& C, const FVector& N)
{
    FVector v0 = B - A;
    FVector v1 = C - A;
    FVector v2 = P - A;

    float d00 = v0 | v0;
    float d01 = v0 | v1;
    float d11 = v1 | v1;
    float d20 = v2 | v0;
    float d21 = v2 | v1;

    float denom = d00 * d11 - d01 * d01;
    if (Abs(denom) < 1e-6f)
        return 0;

    float v = (d11 * d20 - d01 * d21) / denom;
    float w = (d00 * d21 - d01 * d20) / denom;
    float u = 1.f - v - w;

    return (u >= 0.f && v >= 0.f && w >= 0.f);
}

FVector UXOpenGLRenderDevice::ClosestPointOnTriangle(const FVector& P, const FVector& A, const FVector& B, const FVector& C)
{
    // Edges
    FVector AB = B - A;
    FVector AC = C - A;
    FVector AP = P - A;

    float d1 = AB | AP;
    float d2 = AC | AP;

    if (d1 <= 0.f && d2 <= 0.f) return A;

    FVector BP = P - B;
    float d3 = AB | BP;
    float d4 = AC | BP;

    if (d3 >= 0.f && d4 <= d3) return B;

    float vc = d1*d4 - d3*d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f)
    {
        float v = d1 / (d1 - d3);
        return A + v * AB;
    }

    FVector CP = P - C;
    float d5 = AB | CP;
    float d6 = AC | CP;

    if (d6 >= 0.f && d5 <= d6) return C;

    float vb = d5*d2 - d1*d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f)
    {
        float w = d2 / (d2 - d6);
        return A + w * AC;
    }

    float va = d3*d6 - d5*d4;
    if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f)
    {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return B + w * (C - B);
    }

    float denom = 1.f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    return A + AB * v + AC * w;
}

void UXOpenGLRenderDevice::ComputeStaticLightsForFacet(
    ULevel* Level,
    INT iSurf,
    TArray<AActor*>& OutTopLights,
    int MaxStaticLights)
{
    OutTopLights.Empty();

    if (!Level || !Level->Model || iSurf < 0 || iSurf >= Level->Model->Surfs.Num())
        return;

    FSurfInfo* SurfaceInfo = SurfaceInfoMap.Find(iSurf);
    if (!SurfaceInfo) return;

    // Retrieve cached world-space polygon vertices if present
    TArray<FVector>& Verts = SurfaceInfo->Verts;
    if (Verts.Num() < 3) return;

    TArray<glm::uint>& TriIdx = SurfaceInfo->TriIdx;
    if (TriIdx.Num() <= 0) return;

    // Get precomputed triangulation (surface is degenerate if there is none)
    TArray<FVector> Triangles;
    for (INT t = 0; t < TriIdx.Num(); t += 3)
    {
        Triangles.AddItem(Verts(TriIdx(t)));
        Triangles.AddItem(Verts(TriIdx(t+1)));
        Triangles.AddItem(Verts(TriIdx(t+2)));
    }

    FBspSurf bspSurf = Level->Model->Surfs(iSurf);
    bool twoSided   = (bspSurf.PolyFlags & PF_TwoSided);
    bool specialLit = (bspSurf.PolyFlags & PF_SpecialLit);

    TArray<RankedLight> Ranked;
    Ranked.Reserve(Level->Actors.Num());

    AActor* DummyLight = nullptr; // keep one light to ensure each surface has at least one, so the shader doesn't draw a surface with none as fullbright
    FVector FacetNormal = Level->Model->Vectors(bspSurf.vNormal);
    FVector BaseVert = Verts(0);

    for (INT i = 0; i < Level->Actors.Num(); ++i)
    {
        AActor* L = Level->Actors(i);
        if (!L || !IsStaticLight(L)) continue;

        bool specialLight = L->bSpecialLit == 1;
        if (specialLit != specialLight) continue;

        if (!DummyLight) DummyLight = L;

        // Check if this top light is one of our upgraded spotlights
        FakeSpotlightPair* SpotData = GetSpotlightData(L);
        bool bIsSpot = (SpotData != nullptr);

        // Get appropriate radius limits
        float Radius = bIsSpot ? SpotData->ReachRadius : L->WorldLightRadius();
        if (Radius <= 0.f) continue;

        // For spotlights, pull coordinates from the ceiling fixture (TopLight)
        // For regular point lights, use their own location
        FVector LightPos = bIsSpot ? SpotData->TopLight->Location : L->Location;
        
        FVector TargetPoint;
        bool bFoundValidPoint = false;

        // --- POSITION & ATTENUATION SELECTION ---
        if (bIsSpot)
        {
            // 1. Trace the center ray of the spotlight to the infinite plane of the surface
            float Denominator = SpotData->SpotDirection | FacetNormal;
            
            // If the spotlight beam is not completely parallel to the surface plane
            if (Abs(Denominator) > 0.0001f)
            {
                float T = ((BaseVert - LightPos) | FacetNormal) / Denominator;
                
                // If the surface is in front of the spotlight direction
                if (T > 0.f && T < Radius)
                {
                    FVector InfinitePlaneIntersection = LightPos + SpotData->SpotDirection * T;
                    
                    // Check if this intersection point actually falls inside our polygon triangles
                    for (INT t = 0; t < Triangles.Num(); t += 3)
                    {
                        if (PointInTriangle(InfinitePlaneIntersection, Triangles(t), Triangles(t+1), Triangles(t+2), FacetNormal))
                        {
                            TargetPoint = InfinitePlaneIntersection;
                            bFoundValidPoint = true;
                            break;
                        }
                    }
                }
            }

            // 2. Fallback: If center ray misses the polygon, test if vertices or edges clip the cone envelope!
            if (!bFoundValidPoint)
            {
                float minDistSq = FLT_MAX;
                
                for (INT t = 0; t < Triangles.Num(); t += 3)
                {
                    const FVector& A = Triangles(t);
                    const FVector& B = Triangles(t+1);
                    const FVector& C = Triangles(t+2);
                    
                    // Core structural array of edge combinations
                    FVector Edges[3][2] = { {A, B}, {B, C}, {C, A} };
                    
                    for (int e = 0; e < 3; ++e)
                    {
                        const FVector& Start = Edges[e][0];
                        const FVector& End   = Edges[e][1];
                        
                        // Sample 5 discrete points along the segment (Start, 25%, Mid, 75%, End)
                        // This perfectly catches grazing steep ramps cutting through the cone edge!
                        for (int step = 0; step <= 4; ++step)
                        {
                            float Alpha = (float)step * 0.25f;
                            FVector SamplePoint = Start + (End - Start) * Alpha;
                            
                            FVector ToSample = SamplePoint - LightPos;
                            float DistSq = ToSample.SizeSquared();
                            
                            if (DistSq < (Radius * Radius))
                            {
                                float SampleDist = appSqrt(DistSq);
                                FVector DirNorm  = ToSample / Max(SampleDist, 0.001f);
                                float CosAngle   = DirNorm | SpotData->SpotDirection;
                                
                                // If this specific segment sample sits inside the cone, lock it in!
                                if (CosAngle >= SpotData->SpotCosOuter)
                                {
                                    if (DistSq < minDistSq)
                                    {
                                        minDistSq = DistSq;
                                        TargetPoint = SamplePoint;
                                        bFoundValidPoint = true;
                                    }
                                }
                            }
                        }
                    }
                }
                
                // Final safety fallback: If completely missing the cone boundary, use closest point on triangle
                if (!bFoundValidPoint)
                {
                    float planeDist = (LightPos - BaseVert) | FacetNormal;
                    FVector projected = LightPos - FacetNormal * planeDist;
                    float minDistSq2 = FLT_MAX;

                    for (INT t = 0; t < Triangles.Num(); t += 3)
                    {
                        FVector cp = ClosestPointOnTriangle(projected, Triangles(t), Triangles(t+1), Triangles(t+2));
                        float d2 = (cp - projected).SizeSquared();
                        if (d2 < minDistSq2)
                        {
                            minDistSq2 = d2;
                            TargetPoint = cp;
                        }
                    }
                    bFoundValidPoint = true;
                }
            }
        }
        else
        {
            // Standard Point Light logic: Find the mathematically closest point on the polygon
            float planeDist = (LightPos - BaseVert) | FacetNormal;
            FVector projected = LightPos - FacetNormal * planeDist;
            bool inside = false;

            for (INT t = 0; t < Triangles.Num(); t += 3)
            {
                if (PointInTriangle(projected, Triangles(t), Triangles(t+1), Triangles(t+2), FacetNormal))
                {
                    inside = true;
                    TargetPoint = projected;
                    break;
                }
            }

            if (!inside)
            {
                float minDistSq = FLT_MAX;
                for (INT t = 0; t < Triangles.Num(); t += 3)
                {
                    FVector cp = ClosestPointOnTriangle(projected, Triangles(t), Triangles(t+1), Triangles(t+2));
                    float d2 = (cp - projected).SizeSquared();
                    if (d2 < minDistSq)
                    {
                        minDistSq = d2;
                        TargetPoint = cp;
                    }
                }
            }
            bFoundValidPoint = true;
        }

        if (!bFoundValidPoint) continue;

        // Calculate core vectors relative to the chosen target point
        FVector LightToTarget = TargetPoint - LightPos;
        float dist = LightToTarget.Size();
        if (dist > Radius) continue;

        // --- CALCULATE ATTENUATION ---
        float x = Clamp(dist / Radius, 0.0f, 1.0f);
        float attenuation = (1.f - x) / (1.f + 4.f * x * x);

        // --- SPOTLIGHT CONE FACTOR ---
        float ConeFactor = 1.0f;
        if (bIsSpot)
        {
            FVector LightDirNorm = LightToTarget.SafeNormal();
            float CosAngle = LightDirNorm | SpotData->SpotDirection;

            // Outside the outer cone completely? Reject the light loop.
            if (CosAngle < SpotData->SpotCosOuter)
                continue;

            // --- PURE COSINE-SPACE LINEAR ATTENUATION ---
            // Maps perfectly from 1.0 (100% full brightness at SpotCosInner)
            // straight down to 0.0 (0% brightness at SpotCosOuter)
            if (CosAngle < SpotData->SpotCosInner)
            {
                float Range = SpotData->SpotCosInner - SpotData->SpotCosOuter;
                ConeFactor = (CosAngle - SpotData->SpotCosOuter) / Max(Range, 0.001f);
                ConeFactor = Clamp(ConeFactor, 0.0f, 1.0f);
            }
        }

        // --- BRIGHTNESS AND COLOR CALCULATIONS ---
        // Use custom brightness values for upgraded spotlights
        float BaseBrightness = bIsSpot ? (float)SpotData->Brightness : (float)L->LightBrightness;
        float brightness = BaseBrightness / 255.f;

        FPlane RGBColor = FGetHSV(L->LightHue, L->LightSaturation, (BYTE)BaseBrightness);
        float lum = 0.299f * Clamp(RGBColor.X / 255.0f, 0.0f, 1.0f) +
                    0.587f * Clamp(RGBColor.Y / 255.0f, 0.0f, 1.0f) +
                    0.114f * Clamp(RGBColor.Z / 255.0f, 0.0f, 1.0f);
        float brightnessFactor = Max(lum, brightness);

        // --- LAMBERT FACTOR ---
        FVector SurfaceToLightDir = (-LightToTarget).SafeNormal();
        float dot = FacetNormal | SurfaceToLightDir;
        if (twoSided && dot < 0.f) dot = -dot;
        float lambert = Max(0.f, dot);

        // Apply ConeFactor directly to the scoring heuristic
        float score = attenuation * brightnessFactor * lambert * ConeFactor;

        if (score > 0)
        {
            RankedLight R;
            R.Light = L;
            R.Score = score;
            Ranked.AddItem(R);
        }
    }

    // Insert dummy if dark to avoid fullbright bug
    if (Ranked.Num() == 0)
    {
        RankedLight R;
        R.Light = DummyLight;
        R.Score = 0.0f;
        R.IsStatic = true;
        Ranked.AddItem(R);
    }

    Sort(&Ranked(0), Ranked.Num());

    int Count = Min(MaxStaticLights, Ranked.Num());
    OutTopLights.Empty(Count);

    for (int i = 0; i < Count; ++i)
        OutTopLights.AddItem(Ranked(i).Light);
}

static const DOUBLE AngleScale = (2.0 * PI) / 65536.0;
void UXOpenGLRenderDevice::GetAxes(FRotator R, FVector& X, FVector& Y, FVector& Z)
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

void UXOpenGLRenderDevice::ComputeStaticLightsForMover(
    ULevel* Level,
    INT iSurf,
    TArray<AActor*>& OutTopLights,
    int MaxStaticLights)
{
    // --- PART 1: Initialization, validation, and surface centroid calculation ---
    OutTopLights.Empty();
    if (!Level || !Level->Model || iSurf < 0 || iSurf >= Level->Model->Surfs.Num())
        return;

    FBspSurf& bspSurf = Level->Model->Surfs(iSurf);
    AMover* Mover = Cast<AMover>(bspSurf.Actor);
    if (!Mover) return;

    FSurfInfo* SurfaceInfo = SurfaceInfoMap.Find(iSurf);
    if (!SurfaceInfo || SurfaceInfo->Verts.Num() < 3) return;

    // Calculate baseline centroid in REST WORLD SPACE (FSurfInfo::Verts are rest-space world verts)
    FVector RestCentroid(0.f, 0.f, 0.f);
    for (INT v = 0; v < SurfaceInfo->Verts.Num(); ++v)
    {
        RestCentroid += SurfaceInfo->Verts(v);
    }
    RestCentroid /= (FLOAT)SurfaceInfo->Verts.Num();

    // Convert centroid to LOCAL REST SPACE relative to mover's rest pivot (BasePos)
    FVector RestLocal = RestCentroid - Mover->BasePos;

    UBOOL specialLit = (bspSurf.PolyFlags & PF_SpecialLit) != 0;
    TArray<RankedLight> Ranked;
    Ranked.Reserve(Level->Actors.Num());
    AActor* DummyLight = nullptr;

    FVector FacetNormal = Level->Model->Vectors(bspSurf.vNormal);

    // Loop through actors to find potential static lights
    for (INT i = 0; i < Level->Actors.Num(); ++i)
    {
        AActor* L = Level->Actors(i);
        if (!L || !IsStaticLight(L) || (L->bSpecialLit != 0) != specialLit)
            continue;

        if (!DummyLight) DummyLight = L;

        FakeSpotlightPair* SpotData = GetSpotlightData(L);
        bool bIsSpot = (SpotData != nullptr);
        float Radius = bIsSpot ? SpotData->ReachRadius : L->WorldLightRadius();
        if (Radius <= 0.f) continue;

        FVector LightPos = bIsSpot ? SpotData->TopLight->Location : L->Location;

        float MaxScore = 0.0f;

        // Iterate through all mover keyframes, reconstructing position and calculating illumination
        for (INT k = 0; k < Mover->NumKeys; ++k)
        {
            // Correct rest-space ? keyframe-space transform
            FRotator KeyRotation = Mover->BaseRot + Mover->KeyRot[k];
            FVector  KeyPos      = Mover->BasePos + Mover->KeyPos[k];

            // Extract axes for rotation
            FVector RX, RY, RZ;
            GetAxes(KeyRotation, RX, RY, RZ);

            // Rotate rest-local centroid into keyframe orientation
            FVector Rotated;
            Rotated.X = RestLocal.X * RX.X + RestLocal.Y * RY.X + RestLocal.Z * RZ.X;
            Rotated.Y = RestLocal.X * RX.Y + RestLocal.Y * RY.Y + RestLocal.Z * RZ.Y;
            Rotated.Z = RestLocal.X * RX.Z + RestLocal.Y * RY.Z + RestLocal.Z * RZ.Z;

            // Reconstruct world-space centroid for this keyframe
            FVector KeyWorldCentroid = KeyPos + Rotated;

            // Distance check against light radius
            FVector LightToTarget = KeyWorldCentroid - LightPos;
            float dist = LightToTarget.Size();
            if (dist > Radius) continue;

            // Calculate attenuation and spot cone factor if applicable
            float x = Clamp(dist / Radius, 0.0f, 1.0f);
            float attenuation = (1.f - x) / (1.f + 4.f * x * x);

            float ConeFactor = 1.0f;
            if (bIsSpot)
            {
                FVector LightDirNorm = LightToTarget.SafeNormal();
                float CosAngle = LightDirNorm | SpotData->SpotDirection;
                if (CosAngle < SpotData->SpotCosOuter) continue;
                if (CosAngle < SpotData->SpotCosInner)
                    ConeFactor = Clamp(
                        (CosAngle - SpotData->SpotCosOuter) /
                        Max(SpotData->SpotCosInner - SpotData->SpotCosOuter, 0.001f),
                        0.0f, 1.0f);
            }

            float BaseBrightness = bIsSpot ? (float)SpotData->Brightness : (float)L->LightBrightness;
            FPlane RGBColor = FGetHSV(L->LightHue, L->LightSaturation, (BYTE)BaseBrightness);
            float lum = 0.299f * Clamp(RGBColor.X / 255.0f, 0.0f, 1.0f)
                      + 0.587f * Clamp(RGBColor.Y / 255.0f, 0.0f, 1.0f)
                      + 0.114f * Clamp(RGBColor.Z / 255.0f, 0.0f, 1.0f);

            float score = attenuation * Max(lum, BaseBrightness / 255.f) * ConeFactor;
            if (score > MaxScore)
                MaxScore = score;
        }

        if (MaxScore > 0.0f)
        {
            RankedLight R;
            R.Light = L;
            R.Score = MaxScore;
            Ranked.AddItem(R);
        }
    }

    if (Ranked.Num() == 0)
    {
        RankedLight R;
        R.Light = DummyLight;
        R.Score = 0.0f;
        R.IsStatic = true;
        Ranked.AddItem(R);
    }

    Sort(&Ranked(0), Ranked.Num());

    int Count = Min(MaxStaticLights, Ranked.Num());
    OutTopLights.Empty(Count);
    for (int i = 0; i < Count; ++i)
        OutTopLights.AddItem(Ranked(i).Light);
}

void UXOpenGLRenderDevice::ComputeDynamicLightsForFacet(
    ULevel* Level,
    INT iSurf,
    TArray<AActor*>& OutLights)
{
	OutLights.Empty();

	if (!Level || !Level->Model || iSurf < 0 || iSurf >= Level->Model->Surfs.Num())
		return;

	// Try cached verts
    TArray<FVector> Verts;
	FSurfInfo* SurfaceInfo = SurfaceInfoMap.Find(iSurf);
	if (SurfaceInfo)
	{
        Verts = SurfaceInfo->Verts;
	}
    else
    {
        // fallback: compute world-space verts now
        GetWorldspaceSurfaceVerts(Level, iSurf, Verts);
    }

    if (Verts.Num() < 3)
        return;

    // --- Stable world-space normal ---
    FVector N = Level->Model->Vectors(Level->Model->Surfs(iSurf).vNormal);
    N.Normalize();

    // --- Stable world-space centroid ---
    FVector C(0,0,0);
    for (INT i = 0; i < Verts.Num(); i++)
        C += Verts(i);
    C /= Verts.Num();

    // --- Radius ---
    float facetRadius = 0.f;
    for (INT i = 0; i < Verts.Num(); i++)
    {
        float d = (Verts(i) - C).Size();
        if (d > facetRadius)
            facetRadius = d;
    }

    // Iterate dynamic lights
    for (INT i = 0; i < Level->Actors.Num(); ++i)
    {
        AActor* A = Level->Actors(i);
        if (!A || !IsDynamicLight(A))
            continue;

		const FVector LightWorld = A->Location;

		const float dist1 = (LightWorld - C).Size();

		if (dist1 > A->WorldLightRadius()+facetRadius) {
			continue;
		}

        OutLights.AddItem(A);
    }
}

void UXOpenGLRenderDevice::ComputeStaticAndDynamicLightsForFacet(
	FSceneNode* Frame,
    FSurfaceFacet& Facet,
    TArray<AActor*>& OutStaticLights,
    TArray<AActor*>& OutDynamicLights,
	INT MaxLights)
{
	OutStaticLights.Empty();
    OutDynamicLights.Empty();

	if (!Frame || !Frame->Level || !Frame->Level->Model)
		return;

	ULevel* Level = Frame->Level;

	INT Count = 0;
	FVector CentroidView(0,0,0);
	TArray<FVector> VertsView;
	FVector FacetNormalView(0,0,0);
	FSavedPoly* P = Facet.Polys;
	bool haveNormal = false;

    bool twoSided = false;
	for (FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next)
	{
        INT iNode = Facet.Polys->iNode;
        if (iNode >= 0 && iNode < Level->Model->Nodes.Num())
        {
            const FBspNode& Node = Level->Model->Nodes(iNode);
            if (Node.iSurf >= 0 && Node.iSurf < Level->Model->Surfs.Num())
            {
                const FBspSurf& Surf = Level->Model->Surfs(Node.iSurf);
                twoSided = (Surf.PolyFlags & PF_TwoSided) != 0;
            }
        }

		// Normal extraction: try to find a non-degenerate triangle
		if (!haveNormal && Poly->NumPts >= 3)
		{
			const FVector& v0 = Poly->Pts[0]->Point;

			for (INT i = 1; i < Poly->NumPts - 1; ++i)
			{
				const FVector& v1 = Poly->Pts[i]->Point;
				const FVector& v2 = Poly->Pts[i+1]->Point;

				FVector e1 = v1 - v0;
				FVector e2 = v2 - v0;
				FVector n  = e1 ^ e2;

				if (!n.IsNearlyZero())
				{
					FacetNormalView = n.SafeNormal();
					haveNormal = true;
					break;
				}
			}
		}

		// Centroid + radius accumulation
		for (INT i = 0; i < Poly->NumPts; ++i)
		{
            const FVector& ViewPt  = Poly->Pts[i]->Point; // view space
            VertsView.AddItem(ViewPt);
            CentroidView += ViewPt;
            Count += 1;
		}
	}
	CentroidView /= Count;
	FVector CentroidWorld = CentroidView.TransformPointBy(Frame->Uncoords);
	FVector FacetNormalWorld = FacetNormalView.TransformVectorBy(Frame->Uncoords).SafeNormal();

    // --- Radius ---
    float facetRadius = 0.f;
    for (INT i = 0; i < VertsView.Num(); i++)
    {
        float d = (VertsView(i) - CentroidView).Size();
        if (d > facetRadius)
            facetRadius = d;
    }

	TArray<RankedLight> Ranked;
    Ranked.Reserve(Level->Actors.Num());

    AActor* DummyLight = nullptr; // keep one light to ensure each surface has at least one, so the shader doesn't draw a surface with none as fullbright6

    // Iterate lights
    for (INT i = 0; i < Level->Actors.Num(); ++i)
    {
        AActor* A = Level->Actors(i);
        if (!A)
            continue;

        bool isDynamic = IsDynamicLight(A);
        bool isStatic = IsStaticLight(A);

        if (!isDynamic && !isStatic)
            continue;

        if (!DummyLight && isStatic)
            DummyLight = A;

		if (A->WorldLightRadius() <= 0.f)
			continue;
		
		const FVector LightWorld = A->Location;

		const float dist = (LightWorld - CentroidWorld).Size();

		if (dist > A->WorldLightRadius()+facetRadius) {
			continue;
		}

        float x = Clamp(dist / A->WorldLightRadius(), 0.0f, 1.0f);
        float attenuation = (1.f - x) / (1.f + 4.f * x * x);

		float brightness = A->LightBrightness / 255.f;
		FPlane RGBColor = FGetHSV(
			A->LightHue,
			A->LightSaturation,
			A->LightBrightness
		);
		float lum =
			0.299f * Clamp(RGBColor.X / 255.0f, 0.0f, 1.0f) +
			0.587f * Clamp(RGBColor.Y / 255.0f, 0.0f, 1.0f) +
			0.114f * Clamp(RGBColor.Z / 255.0f, 0.0f, 1.0f);
        float brightnessFactor = Max(lum, brightness);

		FVector LightDir = (LightWorld - CentroidWorld).SafeNormal();
        float dot = FacetNormalWorld | LightDir;
        if (twoSided && dot < 0.f)
            dot = -dot;
        float lambert = Max(0.f, dot);

		float score = attenuation * brightnessFactor;

        if (score > 0)
        {
            RankedLight R;
            R.Light = A;
            R.Score = score;
            R.IsStatic = isStatic;
            Ranked.AddItem(R);
        }
    }
    // If no lights contributed, insert a dummy so BSP is not fullbright
    // need a better way to differentiate between surfaces meant to have no lights (sky etc.)
    // that are lit by the lightmap and those that are just legitimately occluded from everything
    if (Ranked.Num() == 0)
    {
        RankedLight R;
        R.Light = DummyLight;
        R.Score = 0.0f;
        R.IsStatic = true;
        Ranked.AddItem(R);
    }
	Sort(&Ranked(0), Ranked.Num());

    Count = Min(MaxLights, Ranked.Num());
    for (int i = 0; i < Count; ++i)
    {
        if (Ranked(i).IsStatic)
            OutStaticLights.AddItem(Ranked(i).Light);
        else
            OutDynamicLights.AddItem(Ranked(i).Light);
    }
}

float UXOpenGLRenderDevice::GetRoughnessFromTextureName(const FSurfaceInfo& Surface)
{
	if (float* Cached = RoughnessCache.Find(Surface.Texture->Texture))
        return *Cached;

	float Cached = ComputeRoughnessFromTextureName(Surface);
	RoughnessCache.Set(Surface.Texture->Texture, Cached);
    return Cached;
}

float UXOpenGLRenderDevice::ComputeRoughnessFromTextureName(const FSurfaceInfo& Surface)
{
    if (!Surface.Texture || !Surface.Texture->Texture)
        return 0.5f; // neutral fallback

	if (Surface.PolyFlags & PF_Environment)
        return 0.05f; // chrome like surfaces are very smooth

    FString Name = Surface.Texture->Texture->GetName();
    Name = Name.Locs();

    auto Has = [&](const TCHAR* Sub) -> bool
    {
        return Name.InStr(Sub) != -1;
    };

    // Metals
    if (Has(TEXT("metal")) || Has(TEXT("steel")) || Has(TEXT("iron")) || Has(TEXT("pipe")) || Has(TEXT("bolt")))// || Has(TEXT("trim")))
        return 0.2f;

    // Glass
    if (Has(TEXT("glass")) || Has(TEXT("window")) || Has(TEXT("screen")) || Has(TEXT("water")))
        return 0.05f;

    // Stone / rock / brick
    if (Has(TEXT("stone")) || Has(TEXT("rock")) || Has(TEXT("brick")) || Has(TEXT("concrete")))
        return 0.7f;

    // Wood
    if (Has(TEXT("wood")) || Has(TEXT("plank")) || Has(TEXT("timber")))
        return 0.65f;

    // Dirt / mud / sand
    if (Has(TEXT("dirt")) || Has(TEXT("mud")) || Has(TEXT("soil")) || Has(TEXT("sand")))
        return 0.8f;

    // textiles
    if (Has(TEXT("cloth")) || Has(TEXT("rug")) || Has(TEXT("carpet")) || Has(TEXT("sail")))
        return 0.95f;

    // Default for everything else
    return 0.7f;
}

struct FLevelLightOverride
{
    FString Match;   // lowercase substring to match
    INT     Cap;     // light cap for this level
};
TArray<FLevelLightOverride> LevelOverrides;

void UXOpenGLRenderDevice::InitLightLevelOverrides()
{
    LevelOverrides.Empty();

    auto Add = [&](const TCHAR* Match, INT Cap)
    {
        FLevelLightOverride Ovr;
        Ovr.Match = FString(Match).Locs();  // lowercase once
        Ovr.Cap   = Cap;
        LevelOverrides.AddItem(Ovr);
    };

    // some built in values, can be overridden by config file
    // Defaults: 55 unless explicitly listed below.

    Add(TEXT("AS-Frigate"), 25);
    Add(TEXT("AS-Guardia"), 35);                 // "guardia fortress"
    Add(TEXT("AS-HiSpeed"), 75);                // "high speed"
    Add(TEXT("AS-Mazon"), 35);                  // "mazon fortress"
    Add(TEXT("AS-OceanFloor"), 35);             // "ocean floor \"station 5\""
    Add(TEXT("AS-Overlord"), 25);
    Add(TEXT("AS-Rook"), 25);
    Add(TEXT("AS-Tutorial"), 25);

    Add(TEXT("Autoplay"), 25);
    Add(TEXT("CityIntro"), 25);

    Add(TEXT("CTF-Beatitude"), 25);
    Add(TEXT("CTF-Command"), 55);               // "command"
    Add(TEXT("CTF-Coret"), 135);                // "coret"
    Add(TEXT("CTF-Cybrosis]["), 65);            // "cybrosis"
    Add(TEXT("CTF-Darji16"), 65);               // "darji outpost #16-a"
    Add(TEXT("CTF-Dreary"), 45);                // "dreary outpost"
    Add(TEXT("CTF-EpicBoy"), 65);               // "epic boy"
    Add(TEXT("CTF-EternalCave"), 25);
    Add(TEXT("CTF-Face"), 35);                  // "facing worlds"
    Add(TEXT("CTF-Face]["), 35);                // "facing worlds"
    Add(TEXT("CTF-Face-SE"), 35);               // "facing worlds special edition"
    Add(TEXT("CTF-Gauntlet"), 25);
    Add(TEXT("CTF-HallOfGiants"), 65);          // "hall of giants"
    Add(TEXT("CTF-High"), 25);
    Add(TEXT("CTF-Hydro16"), 165);              // "hydro bases"
    Add(TEXT("CTF-Kosov"), 25);
    Add(TEXT("CTF-LavaGiant"), 35);             // "lava giant"
    Add(TEXT("CTF-Niven"), 25);
    Add(TEXT("CTF-November"), 25);
    Add(TEXT("CTF-Noxion16"), 35);              // "noxion base"
    Add(TEXT("CTF-Nucleus"), 35);               // "nucleus power plant"
    Add(TEXT("CTF-Orbital"), 75);               // "orbital station #12"
    Add(TEXT("CTF-Ratchet"), 60);               // "ratchet"
    Add(TEXT("CTF-Tutorial"), 25);

    Add(TEXT("DM-Agony"), 45);                  // "the pit of agony"
    Add(TEXT("DM-ArcaneTemple"), 25);
    Add(TEXT("DM-Barricade"), 25);
    Add(TEXT("DM-Bishop"), 25);
    Add(TEXT("DM-Closer"), 45);                 // "closer"
    Add(TEXT("DM-Codex"), 25);
    Add(TEXT("DM-Codex.edit"), 25);
    Add(TEXT("DM-Conveyor"), 25);
    Add(TEXT("DM-Crane"), 85);                  // "crane"
    Add(TEXT("DM-Curse]["), 25);
    Add(TEXT("DM-Cybrosis]["), 65);             // "cybrosis"
    Add(TEXT("DM-Deck[ReduX]"), 25);
    Add(TEXT("DM-Deck16]["), 25);
    Add(TEXT("DM-Fetid"), 25);
    Add(TEXT("DM-Fractal"), 25);
    Add(TEXT("DM-Gothic"), 45);
    Add(TEXT("DM-Grinder"), 65);                // "heavy metal grinder"
    Add(TEXT("DM-Grit-TOURNEY"), 75);           // "grit"
    Add(TEXT("DM-HealPod]["), 35);              // "healing pod ]["
    Add(TEXT("DM-HyperBlast"), 25);
    Add(TEXT("DM-KGalleon"), 25);
    Add(TEXT("DM-Liandri"), 35);                // "liandri"
    Add(TEXT("DM-Mojo]["), 25);
    Add(TEXT("DM-Morbias]["), 45);              // "morbias"
    Add(TEXT("DM-Morpheus"), 65);               // "morpheus"
    Add(TEXT("DM-Oblivion"), 35);               // "itv oblivion"
    Add(TEXT("DM-Peak"), 25);
    Add(TEXT("DM-Phobos"), 25);
    Add(TEXT("DM-Phobos.edit"), 25);
    Add(TEXT("DM-Pressure"), 50);               // "pressure"
    Add(TEXT("DM-Pyramid"), 25);
    Add(TEXT("DM-Shrapnel]["), 45);             // "shrapnel ]["
    Add(TEXT("DM-SpaceNoxx"), 25);
    Add(TEXT("DM-Stalwart"), 45);               // "stalwart"
    Add(TEXT("DM-StalwartXL"), 55);             // "stalwart xl"
    Add(TEXT("DM-Tempest"), 35);                // "tempest"
    Add(TEXT("DM-Turbine"), 25);
    Add(TEXT("DM-Tutorial"), 25);
    Add(TEXT("DM-Viridian-TOURNEY"), 50);       // "viridian"
    Add(TEXT("DM-Zeto"), 95);                   // "zeto"

    Add(TEXT("DOM-Bullet"), 25);
    Add(TEXT("DOM-Cidom"), 65);                 // "city domination"
    Add(TEXT("DOM-Cinder"), 25);
    Add(TEXT("DOM-Condemned"), 25);
    Add(TEXT("DOM-Cryptic"), 25);
    Add(TEXT("DOM-Cybrosis]["), 65);            // "cybrosis"
    Add(TEXT("DOM-Gearbolt"), 25);
    Add(TEXT("DOM-Ghardhen"), 35);              // "ghardhen"
    Add(TEXT("DOM-Lament"), 55);                // "lament ][" (base version)
    Add(TEXT("DOM-Lament]["), 55);              // "lament ]["
    Add(TEXT("DOM-Leadworks"), 95);             // "southside leadworks"
    Add(TEXT("DOM-MetalDream"), 45);            // "metal dream"
    Add(TEXT("DOM-Olden"), 45);
    Add(TEXT("DOM-Sesmar"), 55);                // "tomb of sesmar"
    Add(TEXT("DOM-Tutorial"), 25);
    Add(TEXT("DOM-WolfsBay"), 45);              // "wolf's bay"

    const TCHAR* IniFile = TEXT("XOpenGLDrv.ini");
    const TCHAR* Section = TEXT("XOpenGLDrv.LevelLightCaps");

    TMultiMap<FString,FString>* Map = GConfig->GetSectionPrivate(Section, /*Force=*/false, /*Const=*/true, IniFile);

    if (!Map)
        return;

    for (TMultiMap<FString,FString>::TIterator It(*Map); It; ++It)
    {
        const FString& Key   = It.Key();
        const FString& Value = It.Value();
		if (Key.Len() == 0 || !Value.IsNum()) {
			continue;
		}
        FString Match = Key.Locs();
        INT Cap = appAtoi(*Value);

		bool existing = false;
		for (INT i = 0; i < LevelOverrides.Num(); ++i)
		{
			if (LevelOverrides(i).Match == Match)
			{
				LevelOverrides(i).Cap = Cap; // override existing
				existing = true;
				break;
			}
		}
		if (!existing)
		{
			FLevelLightOverride Ovr;
			Ovr.Match = Match;
			Ovr.Cap = Cap;
			LevelOverrides.AddItem(Ovr);
		}
    }
}

// global array of spotlight pairs
TArray<UXOpenGLRenderDevice::FakeSpotlightPair> FakeSpotlightPairs;

// Global map: FloorLight* -> TopLight* for quick lookup during filtering
TMap<AActor*, AActor*> FakeSpotlightFloorToTopMap;

// Global map: TopLight* -> FloorLight* for quick lookup when disqualifying shadow casting
TMap<AActor*, AActor*> FakeSpotlightTopToFloorMap;

// Global map of floor lights to pair indices, used for fast lookup during data upload
TMap<AActor*, INT> SpotlightFloorIndexMap;

// --- Detect all fake spotlight pairs in the level ---
void DetectFakeSpotlights(ULevel* Level, TArray<AActor*>& AllLights)
{
    UModel* Model = Level->Model;
    
    FakeSpotlightPairs.Empty();
    FakeSpotlightFloorToTopMap.Empty();
    FakeSpotlightTopToFloorMap.Empty();
    SpotlightFloorIndexMap.Empty();

    // Fast O(1) lookup map to track processed lights during this loop
    TMap<AActor*, UBOOL> ProcessedLights;

    for (INT i = 0; i < AllLights.Num(); ++i)
    {
        AActor* FloorCandidate = AllLights(i);
        if (!FloorCandidate) continue;

        // Skip if already paired
        if (ProcessedLights.Find(FloorCandidate)) continue;

        // Check if near the floor
        FVector Down = FloorCandidate->Location + FVector(0, 0, -48);
        bool HasFloor = !UXOpenGLRenderDevice::BSPVisibilityRay(Model, -1, FloorCandidate->Location, Down);
        if (!HasFloor) continue;

        // Look for a vertically stacked partner above
        for (INT j = 0; j < AllLights.Num(); ++j)
        {
            if (i == j) continue;

            AActor* TopCandidate = AllLights(j);
            if (!TopCandidate || ProcessedLights.Find(TopCandidate)) continue;

            // Rough XY alignment (Deck-tested)
            if (Abs(FloorCandidate->Location.X - TopCandidate->Location.X) > 16.f) continue;
            if (Abs(FloorCandidate->Location.Y - TopCandidate->Location.Y) > 16.f) continue;

            // Must be above
            if (TopCandidate->Location.Z <= FloorCandidate->Location.Z) continue;

            // Colors must match closely
            FPlane C1 = FGetHSV(FloorCandidate->LightHue, FloorCandidate->LightSaturation, FloorCandidate->LightBrightness);
            FPlane C2 = FGetHSV(TopCandidate->LightHue, TopCandidate->LightSaturation, TopCandidate->LightBrightness);

            float ColorDist = Abs(C1.X - C2.X) + Abs(C1.Y - C2.Y) + Abs(C1.Z - C2.Z);
            if (ColorDist > 20.f) continue;

            // Must have unobstructed line-of-sight
            if (!UXOpenGLRenderDevice::BSPVisibilityRay(Model, -1, FloorCandidate->Location, TopCandidate->Location))
                continue;

            // Upper light must NOT be near the floor
            FVector Down2 = TopCandidate->Location + FVector(0, 0, -128);
            bool UpperHasFloor = !UXOpenGLRenderDevice::BSPVisibilityRay(Model, -1, TopCandidate->Location, Down2);
            if (UpperHasFloor) continue;

            // --- PAIR FOUND! ---
            UXOpenGLRenderDevice::FakeSpotlightPair Pair;
            Pair.FloorLight  = FloorCandidate;
            Pair.TopLight    = TopCandidate;

            FLOAT FloorRadius = FloorCandidate->WorldLightRadius();
            FLOAT h = Abs(FloorCandidate->Location.Z - TopCandidate->Location.Z);

            // Cone angle from footprint radius
            FLOAT theta = appAtan(FloorRadius / Max(h, 1.f));
            Pair.SpotCosOuter = appCos(theta);
            Pair.SpotCosInner = appCos(theta * 0.001f);

            // Beam reach radius (distance falloff)
            float ReachRadius = (h + FloorRadius) * 1.2f;
            Pair.ReachRadius = ReachRadius;

            // Brightness scaling (stable, mapper-faithful)
            Pair.Brightness = FloorCandidate->LightBrightness;

            // Direction vector
            Pair.SpotDirection = (FloorCandidate->Location - TopCandidate->Location).SafeNormal();
            
            // Map tracking updates
            INT NewIdx = FakeSpotlightPairs.AddItem(Pair);
            SpotlightFloorIndexMap.Set(FloorCandidate, NewIdx);
            FakeSpotlightFloorToTopMap.Set(FloorCandidate, TopCandidate);
            FakeSpotlightTopToFloorMap.Set(TopCandidate, FloorCandidate);

            // Mark both as processed
            ProcessedLights.Set(FloorCandidate, TRUE);
            ProcessedLights.Set(TopCandidate, TRUE);

            //debugf(TEXT("DetectFakeSpotlights: Paired floor light at (%.0f, %.0f, %.0f) with top at (%.0f, %.0f, %.0f)"),
            //    FloorCandidate->Location.X, FloorCandidate->Location.Y, FloorCandidate->Location.Z,
            //    TopCandidate->Location.X, TopCandidate->Location.Y, TopCandidate->Location.Z);

            break; 
        }
    }
}

// --- Check if a light should be excluded (is a fake spotlight floor) ---
UBOOL UXOpenGLRenderDevice::IsFakeSpotlightCeilingToExclude(AActor* L)
{
    return FakeSpotlightTopToFloorMap.Find(L) != nullptr;
}

// --- Check if a light is a spotlight and should have spotlight data sideloaded---
UBOOL UXOpenGLRenderDevice::IsSpotlight(AActor* L)
{
    // retreiving a spotlight:
    // FakeSpotlightPair& P = FakeSpotlightPairs[SpotlightTopIndexMap[L]];
    return FakeSpotlightTopToFloorMap.Find(L) != nullptr;
}

// --- Safely retrieve sideloaded spotlight data ---
UXOpenGLRenderDevice::FakeSpotlightPair* UXOpenGLRenderDevice::GetSpotlightData(AActor* L)
{
    INT* IndexPtr = SpotlightFloorIndexMap.Find(L);
    if (IndexPtr && FakeSpotlightPairs.IsValidIndex(*IndexPtr))
    {
        return &FakeSpotlightPairs(*IndexPtr);
    }
    return nullptr;
}


// lightmap stuff

// run on new level to gather list of lights per surface,
void UXOpenGLRenderDevice::NewLevelPP()
{
	StaticLightsForFacet.Empty();
    DynamicLightsForFacet.Empty();
    CurrentLightToIndex.Empty();
    StaticLevelLights.Empty();
		
	// empty this on new level.  Otherwise can get stale pointers
	RoughnessCache.Empty();

    if (LastLevel && LastLevel->Model && LastLevel->GetLevelInfo())
	{
	    // set number of lights for this level
        // Get the map filename (package name), lowercase
        FString MapName = FString(LastLevel->GetOuter()->GetName()).Locs();
        debugf(TEXT("new level (mapname) %s"), *MapName);
        // Lookup using filename key
        LevelLightCap = GetLevelLightCap(MapName);

        // build the level's static light list for quick lookup when doing occlusion for movers
        for (INT ai = 0; ai < LastLevel->Actors.Num(); ++ai)
        {
            AActor* A = LastLevel->Actors(ai);
            if (A && A->IsA(ALight::StaticClass())
                && !IsDynamicLight(A))
            {
                StaticLevelLights.AddItem(A);
            }
        }

        DetectFakeSpotlights(LastLevel, StaticLevelLights);

        // build lightlist map

        // Precompute static lights for every surface in the level and load extra textures
        UModel* Model = LastLevel->Model;
        INT NumSurfs = Model->Surfs.Num();

        for (INT SurfIndex = 0; SurfIndex < NumSurfs; SurfIndex++)
        {
            // Movers have no static lights.  
            // rather when rendering they use all, until we can sort out which affect it at all possible positions
            // but we really should associate some lights with them, so they get associated with lights and are handled in shadowmapping
            // until we can sort out which surfaces affect movers at all possible positions, 
            // this won't affect drawComplex because that checks isMover before StaticLightsForFacet
            const FBspSurf& Surf = Model->Surfs(SurfIndex);
			AActor* Owner = Surf.Actor;
			bool isMover = (Owner && Owner->IsA(AMover::StaticClass()));
            //if (isMover)
            //    continue;

            // Build static light list
            TArray<AActor*> StaticList;
            if (isMover)
                ComputeStaticLightsForMover(LastLevel, SurfIndex, StaticList, LevelLightCap - 10);
            else
                ComputeStaticLightsForFacet(LastLevel, SurfIndex, StaticList, LevelLightCap - 10);

            StaticLightsForFacet.Set(SurfIndex, StaticList);

            // Preload bump/height maps
            FTextureInfo Info;
            if (Surf.Texture) {
                Surf.Texture->Lock(Info, appSeconds(), 0, Viewport->RenDev);
                QWORD parentID = Info.CacheID;
                if (parentID == INDEX_NONE)
                    continue;

                // Force load bump/height map if present
                ExternalTexture::GetExtra(parentID, ExternalTexture::Extra_Bump);
                ExternalTexture::GetExtra(parentID, ExternalTexture::Extra_Height);
                ExternalTexture::GetExtra(parentID, ExternalTexture::Extra_ORM);
            }
        }
    }
} // end function NewLevelPP

INT UXOpenGLRenderDevice::GetLevelLightCap(const FString& LevelTitle)
{
    FString Lower = LevelTitle.Locs();

    INT Cap = DefaultLightCap;

	for (int t = 0; t < LevelOverrides.Num(); ++t)
	{
		const auto& Ovr = LevelOverrides(t);
        if (Lower == Ovr.Match)
			Cap = Ovr.Cap;
    }

    return Cap;
}

// textures allowed to be depth-faded (sorted for binary search)
static const char* DepthFadeKeys[] = {
    "ancflame1", // ??
    "ancflame2", // yellow flame in DM-ArcaneTemple
    "ancsconc", // DOM-Cryptic
    "asaring",
    "asasring",
    "cststeam", // green steam in DM-Conveyor
    "donfire", // DM-Barricade
    "lightning6", // blue flame in DM-ArcaneTemple
    "liquid6", // water eg CTF-Ratchet
    "liquid7", // ???
    "liquid9", // water eg DM-Codex
    "pbluering",
    "ppurplering",
    "smallfireh3", // DM-Peak, DOM-Sesmar
    "smoke1", // ??
    "swater4a", // DM-FOT-Metalwraith.  accidentally doesn't work on that map because it's not marked as a portal, and that's OK is too shallow anyway would disappear
    "torches2", // DOM-Olden
    "torches3", // DM-Agony
    "water4", // ??
    "water6", // ??
    "waterpool", // DM-FOT-Atlantis
};

bool BinarySearchDepthFade(const char* key)
{
    int low = 0;
    int high = sizeof(DepthFadeKeys) / sizeof(DepthFadeKeys[0]) - 1;

    while (low <= high)
    {
        int mid = (low + high) >> 1;
        int cmp = strcmp(key, DepthFadeKeys[mid]);

        if (cmp == 0)
            return true;
        if (cmp < 0)
            high = mid - 1;
        else
            low = mid + 1;
    }
    return false;
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

bool UXOpenGLRenderDevice::IsDepthFadeFX(const UTexture* Tex)
{
    if (!Tex)
        return false;

    const char* RawName = TCHAR_TO_ANSI(Tex->GetName());

    char LowerName[64];
    ToLowerASCII(RawName, LowerName);

    return BinarySearchDepthFade(LowerName);
}








