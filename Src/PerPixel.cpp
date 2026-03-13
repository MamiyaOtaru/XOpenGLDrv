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

		const FBspSurf& Surf = Level->Model->Surfs(iSurf);
		AActor* Owner = Surf.Actor;

		if (Owner && Owner->IsA(AMover::StaticClass()))
			return INDEX_NONE;

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
void GetWorldspaceSurfaceVerts(ULevel* Level, INT iSurf, TArray<FVector>& OutVerts)
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

    // --- Retrieve world-space polygon vertices ---
    TArray<FVector> Verts;
    GetWorldspaceSurfaceVerts(Level, iSurf, Verts);

    if (Verts.Num() < 3)
        return;

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

	// Temporary struct
	TArray<FVert2D> Temp;
	Temp.Empty(Verts.Num());

	FVector X;
	// Pick the axis least aligned with the normal
	if (Abs(FacetNormal.X) > Abs(FacetNormal.Z))
	{
		// Use Y axis to build perpendicular
		X = FVector(-FacetNormal.Y, FacetNormal.X, 0.f);
	}
	else
	{
		// Use X axis to build perpendicular
		X = FVector(0.f, -FacetNormal.Z, FacetNormal.Y);
	}
	X.Normalize();
	// Now build Y = N × X
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

	TArray<FVector> Triangles; // output: triplets of vertices
	TArray<FVert2D> P = Poly2D; // working copy

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
		
		float planeDist = (LightPos - FacetPos) | FacetNormal;
        //if (abs(planeDist) > Radius)
        //    continue;

		FVector projected = LightPos - FacetNormal * planeDist;
		FVector closest;
        // Inside test (fan triangulation over Verts)
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
            // Closest point on polygon edges
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

        float score = attenuation * brightnessFactor * lambert;

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

	const FBspSurf& Surf = Level->Model->Surfs(iSurf);

	// --- Retrieve world-space polygon vertices ---
    TArray<FVector> Verts;
    GetWorldspaceSurfaceVerts(Level, iSurf, Verts);

    if (Verts.Num() < 3)
        return;

    // --- Stable world-space normal ---
    FVector N = Level->Model->Vectors(Surf.vNormal);
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

		/*if (true) {
			OutLights.AddItem(A);
			continue;
		}*/
		
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

void UXOpenGLRenderDevice::NewLevel()
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