#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include "XOpenGLDrv.h"
#include "XOpenGL.h"

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
    if (A->bMovable)
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

    if (A->bMovable)
        return true;

    // Animated light types are dynamic
	switch (A->LightType)
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
	}

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

struct FVert2D { FLOAT X, Y; FVector P; };
static UBOOL IsConvex(const FVert2D& A, const FVert2D& B, const FVert2D& C)
{
    FLOAT cross = (B.X - A.X)*(C.Y - A.Y) - (B.Y - A.Y)*(C.X - A.X);
    return cross > 0.f; // CCW winding
}

static UBOOL PointInTri(const FVert2D& P, const FVert2D& A, const FVert2D& B, const FVert2D& C)
{
    FLOAT c1 = (B.X - A.X)*(P.Y - A.Y) - (B.Y - A.Y)*(P.X - A.X);
    FLOAT c2 = (C.X - B.X)*(P.Y - B.Y) - (C.Y - B.Y)*(P.X - B.X);
    FLOAT c3 = (A.X - C.X)*(P.Y - C.Y) - (A.Y - C.Y)*(P.X - C.X);
    return (c1 >= 0 && c2 >= 0 && c3 >= 0);
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
	FSceneNode* Frame,
    INT iSurf,
    TArray<AActor*>& OutTopLights,
    int MaxStaticLights)
{
	OutTopLights.Empty();

    if (!Frame || !Frame->Level || !Frame->Level->Model || iSurf < 0 || iSurf >= Frame->Level->Model->Surfs.Num())
        return;

	ULevel* Level = Frame->Level;

	// --- Retrieve cached world-space polygon vertices if present ---
	TArray<glm::uint>* TriIdx = SurfaceTriIndices.Find(iSurf);
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

	// If we don't have a precomputed triangulation, build it (dedupe + earclip)
	TArray<FVector> Triangles; // triplets of vertices
	if (TriIdx && TriIdx->Num() > 0)
	{
		for (INT t = 0; t < TriIdx->Num(); t += 3)
		{
			Triangles.AddItem(Verts((*TriIdx)(t)));
			Triangles.AddItem(Verts((*TriIdx)(t+1)));
			Triangles.AddItem(Verts((*TriIdx)(t+2)));
		}
	}
	else
	{
		// existing dedupe + earclip triangulation (same as earlier)
		// --- Dedupicate ---
		for (INT i = 0; i < Verts.Num(); i++)
		{
			for (INT j = i + 1; j < Verts.Num(); j++)
			{
				if (FPointsAreNear(Verts(i), Verts(j), 0.0025f))
				{
					Verts.Remove(j);
					j--;
				}
			}
		}

		// --- Stable world-space normal ---
		const FBspSurf& Surf = Level->Model->Surfs(iSurf);
		FVector FacetNormal = Level->Model->Vectors(Surf.vNormal);
		FacetNormal.Normalize();

		// --- Stable world-space centroid ---
		FVector FacetPos(0,0,0);
		for (INT i = 0; i < Verts.Num(); ++i)
			FacetPos += Verts(i);
		FacetPos /= Verts.Num();

		FVector X;
		if (Abs(FacetNormal.X) > Abs(FacetNormal.Z))
			X = FVector(-FacetNormal.Y, FacetNormal.X, 0.f);
		else
			X = FVector(0.f, -FacetNormal.Z, FacetNormal.Y);
		X.Normalize();
		FVector Y = (FacetNormal ^ X).SafeNormal();

		TArray<FVert2D> Poly2D;
		for (INT i = 0; i < Verts.Num(); i++)
		{
			FVector d = Verts(i) - FacetPos;
			FVert2D v;
			v.X = d | X;
			v.Y = d | Y;
			v.P = Verts(i);
			Poly2D.AddItem(v);
		}

		TArray<FVert2D> P = Poly2D;
		while (P.Num() >= 3)
		{
			UBOOL earFound = 0;
			for (INT i = 0; i < P.Num(); i++)
			{
				INT i0 = (i + P.Num() - 1) % P.Num();
				INT i1 = i;
				INT i2 = (i + 1) % P.Num();

				const FVert2D& A = P(i0);
				const FVert2D& B = P(i1);
				const FVert2D& C = P(i2);

				if (!IsConvex(A, B, C))
					continue;

				UBOOL containsPoint = 0;
				for (INT j = 0; j < P.Num(); j++)
				{
					if (j == i0 || j == i1 || j == i2)
						continue;

					if (PointInTri(P(j), A, B, C))
					{
						containsPoint = 1;
						break;
					}
				}

				if (containsPoint)
					continue;

				// This is an ear
				Triangles.AddItem(A.P);
				Triangles.AddItem(B.P);
				Triangles.AddItem(C.P);

				P.Remove(i1);
				earFound = 1;
				break;
			}

			if (!earFound)
				break; // polygon is degenerate
		}
	}

    TArray<RankedLight> Ranked;
    Ranked.Reserve(Level->Actors.Num());

	// Iterate static lights
    for (INT i = 0; i < Level->Actors.Num(); ++i)
    {
        AActor* L = Level->Actors(i);
        if (!L || !IsStaticLight(L))
            continue;

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
		float lambert = Max(0.f, FacetNormal | LightDir);

        float score = attenuation *brightnessFactor* lambert;

        RankedLight R;
        R.Light = L;
        R.Score = score;
        Ranked.AddItem(R);
    }

	Sort(&Ranked(0), Ranked.Num());

    int Count = Min(MaxStaticLights, Ranked.Num());
    OutTopLights.Empty(Count);

    for (int i = 0; i < Count; ++i)
        OutTopLights.AddItem(Ranked(i).Light);
}

void UXOpenGLRenderDevice::ComputeDynamicLightsForFacet(
	FSceneNode* Frame,
    INT iSurf,
    TArray<AActor*>& OutLights)
{
	OutLights.Empty();

	if (!Frame || !Frame->Level || !Frame->Level->Model || iSurf < 0 || iSurf >= Frame->Level->Model->Surfs.Num())
		return;

	ULevel* Level = Frame->Level;

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
    TArray<AActor*>& OutLights,
	INT MaxLights)
{
	OutLights.Empty();

	if (!Frame || !Frame->Level || !Frame->Level->Model)
		return;

	ULevel* Level = Frame->Level;

	INT Count = 0;
	FVector CentroidView(0,0,0);
	TArray<FVector> VertsView;
	FVector FacetNormalView(0,0,0);
	FSavedPoly* P = Facet.Polys;
	bool haveNormal = false;

	for (FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next)
	{
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

    // Iterate dynamic lights
    for (INT i = 0; i < Level->Actors.Num(); ++i)
    {
        AActor* A = Level->Actors(i);
        if (!A || (!IsDynamicLight(A) && !IsStaticLight(A)))
            continue;

		if (A->WorldLightRadius() <= 0.f)
			continue;

		/*if (true) {
			OutLights.AddItem(A);
			continue;
		}*/
		
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
		float lambert = Max(0.f, FacetNormalWorld | LightDir);

		float score = attenuation * brightnessFactor;

        RankedLight R;
        R.Light = A;
        R.Score = score;
        Ranked.AddItem(R);
    }

	Sort(&Ranked(0), Ranked.Num());

    Count = Min(MaxLights, Ranked.Num());
    for (int i = 0; i < Count; ++i)
        OutLights.AddItem(Ranked(i).Light);
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
    Add(TEXT("zeto"), 95);
    Add(TEXT("orbital station #12"), 75);
    Add(TEXT("grit"), 75);
    Add(TEXT("morpheus"), 65);
    Add(TEXT("darji outpost #16-a"), 65);
    Add(TEXT("ratchet"), 60);
    Add(TEXT("lament ]["), 55);
	Add(TEXT("pressure"), 50);
	Add(TEXT("closer"), 45);
    Add(TEXT("metal dream"), 45);
    Add(TEXT("wolf's bay"), 45);
    Add(TEXT("shrapnel ]["), 45);
    Add(TEXT("hydro bases"), 40);
    Add(TEXT("the pit of agony"), 35);
    Add(TEXT("heavy metal grinder"), 35);
    Add(TEXT("healing pod ]["), 35);
    Add(TEXT("itv oblivion"), 35);
    Add(TEXT("ocean floor \"station 5\""), 35);
    Add(TEXT("mazon fortress"), 35);
    Add(TEXT("guardia fortress"), 35);
    Add(TEXT("dreary outpost"), 35);
    Add(TEXT("facing worlds special edition"), 35);
    Add(TEXT("nucleus power plant"), 35);
    Add(TEXT("noxion base"), 35);
    Add(TEXT("ghardhen"), 35);

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

void UXOpenGLRenderDevice::NewLevelPP()
{
	StaticLightsForFacet.Empty();
    DynamicLightsForFacet.Empty();
    CurrentLightToIndex.Empty();
		
	// empty this on new level.  Otherwise can get stale pointers
	RoughnessCache.Empty();

	// set number of lights for this level
	FStringNoInit LevelName = LastLevel->GetLevelInfo()->Title;
	FString Lower = LevelName.Locs();
	//debugf(TEXT("new level %s"), Lower);
	LevelLightCap = GetLevelLightCap(Lower);
	// Build smooth vertex normals for phong shading (precompute once per level)
	if (LastLevel && LastLevel->Model)
	{
		BuildSmoothVertexNormalsForLevel(LastLevel);
	}
}

INT UXOpenGLRenderDevice::GetLevelLightCap(const FString& LevelTitle)
{
    FString Lower = LevelTitle.Locs();

    INT Cap = DefaultLightCap;

	for (int t = 0; t < LevelOverrides.Num(); ++t)
	{
		const auto& Ovr = LevelOverrides(t);
		if (Lower.InStr(Ovr.Match) != -1)
			Cap = Ovr.Cap;
    }

    return Cap;
}


static const char* DepthFadeKeys[] = {
    "asaring",
    "asasring",
    "asmdalt_a00",
    "asmdalt_a01",
    "asmdalt_a02",
    "asmdalt_a03",
    "asmdex_a00",
    "asmdex_a01",
    "asmdex_a02",
    "asmdex_a03",
    "asmdex_a04",
    "asmdex_a05",
    "asmdex_a06",
    "asmdex_a07",
    "asmdex_a08",
    "asmdex_a09",
    "asmdex_a10",
    "asmdex_a11",
    "exp1_a00",
    "exp1_a01",
    "exp1_a02",
    "exp1_a03",
    "exp1_a04",
    "exp1_a05",
    "exp1_a06",
    "exp1_a07",
    "exp1_a08",
    "exp1_a09",
    "exp2_a00",
    "exp2_a01",
    "exp2_a02",
    "exp2_a03",
    "exp2_a04",
    "exp2_a05",
    "exp2_a06",
    "exp2_a07",
    "exp2_a08",
    "exp2_a09",
    "exp2_a10",
    "exp2_a11",
    "exp2_a12",
    "exp2_a13",
    "exp2_a14",
    "exp2_a15",
    "exp2_a16",
    "exp2_a17",
    "exp3_a00",
    "exp3_a01",
    "exp3_a02",
    "exp3_a03",
    "exp3_a04",
    "exp3_a05",
    "exp3_a06",
    "exp3_a07",
    "exp3_a08",
    "exp4_a00",
    "exp4_a01",
    "exp4_a02",
    "exp4_a03",
    "exp4_a04",
    "exp4_a05",
    "exp4_a06",
    "exp4_a07",
    "exp4_a08",
    "exp5_a00",
    "exp5_a01",
    "exp5_a02",
    "exp5_a03",
    "exp5_a04",
    "exp5_a05",
    "exp5_a06",
    "exp5_a07",
    "exp5_a08",
    "exp5_a09",
    "exp5_a10",
    "exp5_a11",
    "exp5_a12",
    "exp5_a13",
    "exp6_a00",
    "exp6_a01",
    "exp6_a02",
    "exp6_a03",
    "exp6_a04",
    "exp6_a05",
    "exp6_a06",
    "exp6_a07",
    "exp6_a08",
    "exp6_a09",
    "exp6_a10",
    "exp7_a00",
    "exp7_a01",
    "exp7_a02",
    "exp7_a03",
    "exp7_a04",
    "exp7_a05",
    "exp7_a06",
    "exp7_a07",
    "exp7_a08",
    "exp7_a09",
    "exp7_a10",
    "exp7_a11",
    "exp7_a12",
    "g1r_a00",
    "g1r_a01",
    "g1r_a02",
    "g1r_a03",
    "g1r_a04",
    "g1r_a05",
    "g1r_a06",
    "g1r_a07",
    "g1r_a08",
    "g1r_a09",
    "g1r_a10",
    "g2r_a00",
    "g2r_a01",
    "g2r_a02",
    "g2r_a03",
    "g2r_a04",
    "g2r_a05",
    "g2r_a06",
    "g2r_a07",
    "g2r_a08",
    "g2r_a09",
    "g2r_a10",
    "g3r_a00",
    "g3r_a01",
    "g3r_a02",
    "g3r_a03",
    "g3r_a04",
    "g3r_a05",
    "g3r_a06",
    "g3r_a07",
    "g3r_a08",
    "g3r_a09",
    "g3r_a10",
    "gbproj0",
    "gbproj1",
    "gbproj2",
    "gbproj3",
    "gbproj4",
    "gbproj5",
    "ge1_a00",
    "ge1_a01",
    "ge1_a02",
    "ge1_a03",
    "ge1_a04",
    "ge1_a05",
    "ge1_a06",
    "ge1_a07",
    "ge1_a08",
    "ge1_a09",
    "ge1_a10",
    "heexpl1_a00",
    "heexpl1_a01",
    "heexpl1_a02",
    "heexpl1_a03",
    "heexpl1_a04",
    "heexpl1_a05",
    "heexpl1_a06",
    "impact_a00",
    "impact_a01",
    "impact_a02",
    "impact_a03",
    "impact_a04",
    "jenergy2",
    "jenergy3",
    "ne_a00",
    "ne_a01",
    "ne_a02",
    "ne_a03",
    "ne_a04",
    "ne_a05",
    "ne_a06",
    "ne_a07",
    "ne_a08",
    "ne_a09",
    "ne_a10",
    "ne_a11",
    "ne_a12",
    "pblst_a00",
    "pblst_a01",
    "pblst_a02",
    "pblst_a03",
    "pblst_a04",
    "pbluering",
    "pbolt1",
    "pbolt2",
    "pbolt3",
    "pbolt4",
    "pend_a00",
    "pend_a01",
    "pend_a02",
    "pend_a03",
    "phit_a00",
    "phit_a01",
    "phit_a02",
    "phit_a03",
    "ppurplering",
    "sbolt0",
    "sbolt1",
    "sbolt2",
    "sbolt3",
    "sbolt4",
    "we_a00",
    "we_a01",
    "we_a02",
    "we_a03",
    "we_a04",
    "we_a05",
    "we_a06",
    "we_a07",
    "we_a08",
    "we_a09",
    "we_a10",
    "we_a11",
    "we_a12",
    "we_a13",
    "we_a14",
    "we_a15",
    "we_a16",
    "we_a17",
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

