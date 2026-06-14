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

static UBOOL PointInTriangle(const FVector& P, const FVector& A, const FVector& B, const FVector& C, const FVector& N)
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

static FVector ClosestPointOnTriangle(const FVector& P, const FVector& A, const FVector& B, const FVector& C)
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

	// --- Retrieve cached world-space polygon vertices if present ---
    TArray<FVector> Verts;
	FSurfInfo* SurfaceInfo = SurfaceInfoMap.Find(iSurf);
    if (!SurfaceInfo)
    {
        return;
    }
    Verts = SurfaceInfo->Verts;
	
    if (Verts.Num() < 3)
        return;

    TArray<glm::uint>& TriIdx = SurfaceInfo->TriIdx;

	// get precomputed triangulation (surface is degenerate if there is none)
	TArray<FVector> Triangles; // triplets of vertices
	if (TriIdx.Num() > 0)
	{
		for (INT t = 0; t < TriIdx.Num(); t += 3)
		{
			Triangles.AddItem(Verts(TriIdx(t)));
			Triangles.AddItem(Verts(TriIdx(t+1)));
			Triangles.AddItem(Verts(TriIdx(t+2)));
		}
	}
	else
	{
        return;
	}

    FBspSurf bspSurf = Level->Model->Surfs(iSurf);
    bool twoSided = (bspSurf.PolyFlags & PF_TwoSided);
    bool specialLit = (bspSurf.PolyFlags & PF_SpecialLit);

    TArray<RankedLight> Ranked;
    Ranked.Reserve(Level->Actors.Num());

    AActor* DummyLight = nullptr; // keep one light to ensure each surface has at least one, so the shader doesn't draw a surface with none as fullbright6

	// Iterate static lights
    for (INT i = 0; i < Level->Actors.Num(); ++i)
    {
        AActor* L = Level->Actors(i);
        if (!L || !IsStaticLight(L))
            continue;

        // surfaces marked specialLit only receive lighting from actors with bSpecialLit=1
        bool specialLight = L->bSpecialLit == 1;
        if (specialLit != specialLight)
            continue;

        if (!DummyLight)
            DummyLight = L;

        float Radius = L->WorldLightRadius();
        if (Radius <= 0.f)
            continue;

		FVector LightPos = (L->Location);

		float planeDist = (LightPos - (Verts.Num()>0 ? Verts(0) : FVector(0,0,0))) | (Level->Model->Vectors(Level->Model->Surfs(iSurf).vNormal));
		// projection onto surface plane
		FVector FacetNormal = Level->Model->Vectors(Level->Model->Surfs(iSurf).vNormal);
		FVector projected = LightPos - FacetNormal * planeDist;

		FVector closest;
        // Inside test using triangles
        bool inside = false;
		for (INT t = 0; t < Triangles.Num(); t += 3)
		{
			const FVector& A = Triangles(t);
			const FVector& B = Triangles(t+1);
			const FVector& C = Triangles(t+2);

			if (PointInTriangle(projected, A, B, C, FacetNormal))
			{
				inside = true;
				closest = projected;
				break;
			}
		}

        if (!inside)
        {
            // Closest point on polygon edges (fallback to triangle closest)
            float minDistSq = FLT_MAX;
			for (INT t = 0; t < Triangles.Num(); t += 3)
			{
				const FVector& A = Triangles(t);
				const FVector& B = Triangles(t+1);
				const FVector& C = Triangles(t+2);

				FVector cp = ClosestPointOnTriangle(projected, A, B, C);
				float d2 = (cp - projected).SizeSquared();

				if (d2 < minDistSq)
				{
					minDistSq = d2;
					closest = cp;
				}
			}
        }

		float dist = (LightPos - closest).Size();

        float x = Clamp(dist / Radius, 0.0f, 1.0f);
        float attenuation = (1.f - x) / (1.f + 4.f * x * x);

        float brightness = L->LightBrightness / 255.f;
		FPlane RGBColor = FGetHSV(
			L->LightHue,
			L->LightSaturation,
			L->LightBrightness
		);
		float lum =
			0.299f * Clamp(RGBColor.X / 255.0f, 0.0f, 1.0f) +
			0.587f * Clamp(RGBColor.Y / 255.0f, 0.0f, 1.0f) +
			0.114f * Clamp(RGBColor.Z / 255.0f, 0.0f, 1.0f);
        float brightnessFactor = Max(lum, brightness);

		FVector LightDir = (LightPos - closest).SafeNormal();
        float dot = FacetNormal | LightDir;
        if (twoSided && dot < 0.f)
            dot = -dot;
        float lambert = Max(0.f, dot);

        float score = attenuation * brightnessFactor * lambert;

        if (score > 0)
        {
            RankedLight R;
            R.Light = L;
            R.Score = score;
            Ranked.AddItem(R);
        }
    } // end iterate static lights
    // If no lights contributed, insert a dummy so BSP is not fullbright
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

    FString Name = Surface.Texture->Texture->GetFullName();
    Name = Name.Locs();

    auto Has = [&](const TCHAR* Sub) -> bool
    {
        return Name.InStr(Sub) != -1;
    };

    // Metals
    if (Has(TEXT("metal")) || Has(TEXT("steel")) || Has(TEXT("iron")) || Has(TEXT("pipe")) || Has(TEXT("bolt")))// || Has(TEXT("trim")))
        return 0.2f;

    // Glass
    if (Has(TEXT("glass")) || Has(TEXT("window")) || Has(TEXT("screen")))
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

// lightmap stuff



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

        // build lightlist map

        // Precompute static lights for every surface in the level and load extra textures
        UModel* Model = LastLevel->Model;
        INT NumSurfs = Model->Surfs.Num();

        for (INT SurfIndex = 0; SurfIndex < NumSurfs; SurfIndex++)
        {
            // Movers have no static lights
            const FBspSurf& Surf = Model->Surfs(SurfIndex);
			AActor* Owner = Surf.Actor;
			bool isMover = (Owner && Owner->IsA(AMover::StaticClass()));
            if (isMover)
               continue;

            // Build static light list
            TArray<AActor*> StaticList;
            ComputeStaticLightsForFacet(LastLevel, SurfIndex, StaticList, LevelLightCap - 10);
            StaticLightsForFacet.Set(SurfIndex, StaticList);

            // Debug surfaces exceeding threshold
            /*if (StaticList.Num() > 256)
            {
                debugf(TEXT("Surface %d exceeds 256 lights: %d lights, %d nodes"),
                    SurfIndex,
                    StaticList.Num(),
                    Surf.Nodes.Num()
                );

                // Optional: dump node indices
                for (INT i = 0; i < Surf.Nodes.Num(); i++)
                {
                    debugf(TEXT("    Node[%d] = %d"), i, Surf.Nodes(i));
                }
            }*/

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
            }
        }
        // build the level's static light list for quick lookup when doing occlusion for movers
        for (INT ai = 0; ai < LastLevel->Actors.Num(); ++ai)
        {
            AActor* A = LastLevel->Actors(ai);
            if (A && A->IsA(ALight::StaticClass()) && !IsDynamicLight(A))
                StaticLevelLights.AddItem(A);
        }
    }
}

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


static const char* DepthFadeKeys[] = {
    //"ancflame1", // ??
    //"ancflame2", // yellow flame in DM-ArcaneTemple
    //"ancsconc", // DOM-Cryptic
    "asaring",
    "asasring",
    //"cststeam", // green steam in DM-Conveyor
    //"donfire", // DM-Barricade
    //"lightning6", // blue flame in DM-ArcaneTemple
    "pbluering",
    "ppurplering",
    //"smallfireh3", // DM-Peak, DOM-Sesmar
    //"smoke1", // ??
    //"torches2", // DOM-Olden
    //"torches3", // DM-Agony
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








