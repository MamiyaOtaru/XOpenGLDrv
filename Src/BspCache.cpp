#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_inverse.hpp>

#include "XOpenGLDrv.h"
#include "XOpenGL.h"

// --- New helper: build smooth vertex normals per-surface (called from NewLevel) ---
//
// Algorithm summary:
// 1. For every surface (iSurf) gather the world-space vertex list in the same
//    order as GetWorldspaceSurfaceVerts() logic. Deduplicate small duplicates.
// 2. Triangulate the polygon using ear clipping and produce index triplets.
// 3. Keep per-surface aggregate area and surface normal.
// 4. Build a map from spatial position key -> list of (iSurf, localIndex).
// 5. For each position, for each referenced (iSurf, localIndex) compute an
//    averaged normal by including only surfaces whose face normal is within
//    the angle threshold; weight each contributor by its surface area.
// 6. Store resulting per-vertex normals in SurfaceVertexNormals[iSurf]
//
// Notes:
// - Uses quantized keys to group identical/near-identical positions.
// - If triangulation/earclip fails, SurfaceTriIndices might be empty and consumers fall back to per-facet handling.

inline UBOOL VectorsEqual(const FVector& A, const FVector& B, float Tolerance = .01)
{
	return (fabs(A.X - B.X) < Tolerance &&
		fabs(A.Y - B.Y) < Tolerance &&
		fabs(A.Z - B.Z) < Tolerance);
}

bool VectorsEquivalent(const FVector& A, const FVector& B, float Tolerance = KINDA_SMALL_NUMBER)
{
	return (A - B).SizeSquared() < Tolerance * Tolerance ||
		(A + B).SizeSquared() < Tolerance * Tolerance; // allow flipped axis
}

void UXOpenGLRenderDevice::BuildSmoothVertexNormalsForLevel(ULevel* Level)
{
	SurfaceInfoMap.Empty();
	if (!Level || !Level->Model)
		return;

	const float DegToRad = 3.14159265358979323846f / 180.0f;
	const float dotThreshold = cosf(SmoothNormalAngleThresholdDegrees * DegToRad);

	struct PosRef { INT iSurf; INT LocalIndex; };
	// map quantized position key -> list of references into surfaces
	TMap<SQWORD, TArray<PosRef>> PosMap;


	// --- Unified path: iterate all BSP nodes ---
	for (INT ni = 0; ni < Level->Model->Nodes.Num(); ++ni)
	{
		const FBspNode& Node = Level->Model->Nodes(ni);
		INT iSurf = Node.iSurf;
		if (iSurf < 0 || iSurf >= Level->Model->Surfs.Num())
			continue;

		const FBspSurf& Surf = Level->Model->Surfs(iSurf);
		AActor* Owner = Surf.Actor;
		bool isMover = (Owner && Owner->IsA(AMover::StaticClass()));

		// Dynamic SkyZone evaluation: Look up what actor governs this node's zone index
		bool isSky = false;
		AZoneInfo* ZoneActor = nullptr;
		for (INT side = 0; side < 2; ++side)
		{
			BYTE ZoneIndex = Node.iZone[side]; // Read individual array element safely

			// UE1 has a maximum limit of 64 structural zones per level map
			if (ZoneIndex > 0 && ZoneIndex < 64 && Level->Model->Zones[ZoneIndex].ZoneActor) 
			{
				ZoneActor = Level->Model->Zones[ZoneIndex].ZoneActor;
				
				if (ZoneActor->IsA(ASkyZoneInfo::StaticClass()))
				{
					isSky = true;
					break; // Found it, no need to evaluate the other side
				}
				else
				{
					ZoneActor = nullptr;
				}
			}
		}

		// Get or create FSurfInfo
		FSurfInfo* pSI = SurfaceInfoMap.Find(iSurf);
		if (!pSI)
		{
			SurfaceInfoMap.Set(iSurf, FSurfInfo());
			pSI = SurfaceInfoMap.Find(iSurf);

			pSI->iSurf = iSurf;
			pSI->SurfaceNormal = Level->Model->Vectors(Surf.vNormal).SafeNormal();
			pSI->IsMover = isMover;
			pSI->IsSky = isSky;
			pSI->SkyZoneActor = (ASkyZoneInfo*)ZoneActor;
			pSI->Owner = Owner;
			pSI->PolyFlags = Surf.PolyFlags;
		}

		FSurfInfo& SI = *pSI;

		// Build FNodeInfo for this node
		FNodeInfo NI;
		NI.iNode = ni;
		NI.PlaneNormal = FVector(Node.Plane.X, Node.Plane.Y, Node.Plane.Z).SafeNormal();
		NI.PlaneW = Node.Plane.W;
		NI.VertIndices.AddZeroed(Node.NumVertices);

		for (INT vi = 0; vi < Node.NumVertices; ++vi)
		{
			INT iVert = Node.iVertPool + vi;
			if (iVert < 0 || iVert >= Level->Model->Verts.Num())
				continue;

			const FVert& V = Level->Model->Verts(iVert);
			const FVector& P = Level->Model->Points(V.pVertex);

			SI.Verts.AddItem(P);

			// Compute UVs from surf basis
			FVector Base = Level->Model->Points(Surf.pBase);
			FVector UVec = Level->Model->Vectors(Surf.vTextureU);
			FVector VVec = Level->Model->Vectors(Surf.vTextureV);

			float UU = ((P - Base) | UVec) + Surf.PanU;
			float VV = ((P - Base) | VVec) + Surf.PanV;

			FTextureInfo Info;
			if (Surf.Texture)
			{
				Surf.Texture->Lock(Info, appSeconds(), 0, Viewport->RenDev);
				UU = (UU - Info.Pan.X) * Info.UScale;
				VV = (VV - Info.Pan.Y) * Info.VScale;
			}

			SI.UVs.AddItem(FVector(UU, VV, 0));
			NI.VertIndices(vi) = SI.Verts.Num() - 1;
		}

		SI.Nodes.AddItem(NI);
	} // end loop through nodes

	// After node iteration has populated SurfaceInfoMap...
	for (INT iSurf = 0; iSurf < Level->Model->Surfs.Num(); ++iSurf)
	{
		FSurfInfo* pSI = SurfaceInfoMap.Find(iSurf);
		if (!pSI)
			continue;

		FSurfInfo& SI = *pSI;

		SI.VertexNormals.AddZeroed(SI.Verts.Num()); // initialize non-deduped per-vertex normals

		// triangulate to compute approximate area (fan from v0)
		if (SI.Verts.Num() >= 3)
		{
			const FVector& v0 = SI.Verts(0);
			for (INT i = 1; i < SI.Verts.Num() - 1; ++i)
			{
				FVector e1 = SI.Verts(i) - v0;
				FVector e2 = SI.Verts(i + 1) - v0;
				float triArea = ((e1 ^ e2).Size()) * 0.5f;
				SI.Area += triArea;
			}
			if (SI.Area <= 0.f)
				SI.Area = 1.0f;
		}
		else
		{
			SI.Area = 1.0f;
		}

		// Build quantized position map entries for smoothing
		for (INT vi = 0; vi < SI.Verts.Num(); ++vi)
		{
			const FVector& P = SI.Verts(vi);
			const SQWORD Xk = (SQWORD)appFloor(P.X * 400.0f + 0.5f);
			const SQWORD Yk = (SQWORD)appFloor(P.Y * 400.0f + 0.5f);
			const SQWORD Zk = (SQWORD)appFloor(P.Z * 400.0f + 0.5f);

			QWORD Key = (QWORD(Xk) & 0x1FFFFF) |
				((QWORD(Yk) & 0x1FFFFF) << 21) |
				((QWORD(Zk) & 0x1FFFFF) << 42);

			PosRef R; R.iSurf = iSurf; R.LocalIndex = vi;
			TArray<PosRef>* List = PosMap.Find((SQWORD)Key);
			if (!List)
			{
				TArray<PosRef> NewList;
				NewList.AddItem(R);
				PosMap.Set((SQWORD)Key, NewList);
			}
			else
			{
				List->AddItem(R);
			}
		} // end loop through verts
	} // end loop through surfaces

	const float posTolSq = 0.0025f * 0.0025f;

	// Now compute averaged normals per position reference (smoothing)
	for (TMap<SQWORD, TArray<PosRef>>::TIterator It(PosMap); It; ++It)
	{
		const TArray<PosRef>& Refs = It.Value();

		for (INT r = 0; r < Refs.Num(); ++r)
		{
			const PosRef& Target = Refs(r);
			INT tgtSurf = Target.iSurf;
			INT tgtIdx = Target.LocalIndex;

			// accumulate weighted normals
			FVector accum(0,0,0);
			float totalWeight = 0.f;

			// find base surface info (fall back to a stable normal if missing)
			FSurfInfo* baseSI = SurfaceInfoMap.Find(tgtSurf);
			const FVector baseNormal = baseSI ? baseSI->SurfaceNormal : FVector(0,0,1);

			for (INT s = 0; s < Refs.Num(); ++s)
			{
				const PosRef& Other = Refs(s);
				INT otherSurf = Other.iSurf;

				FSurfInfo* otherSI = SurfaceInfoMap.Find(otherSurf);
				if (!otherSI)
					continue; // skip missing surface entries

				const FVector& otherNormal = otherSI->SurfaceNormal;
				float areaWeight = otherSI->Area;

				// include only if angle between baseNormal and otherNormal is within threshold
				float dp = baseNormal | otherNormal;
				if (dp >= dotThreshold)
				{
					accum += otherNormal * areaWeight;
					totalWeight += areaWeight;
				}
			}

			FVector finalNormal;
			if (totalWeight > 0.f && !accum.IsNearlyZero())
			{
				finalNormal = (accum / totalWeight).SafeNormal();
			}
			else
			{
				finalNormal = baseNormal;
			}

			// store into SurfaceInfoMap per-vertex normals
			FSurfInfo* pFI = SurfaceInfoMap.Find(tgtSurf);
			if (pFI && tgtIdx >= 0)
			{
				// ensure VertexNormals array is sized; it should already be, but be defensive
				if (pFI->VertexNormals.Num() != pFI->Verts.Num())
					pFI->VertexNormals.AddZeroed(pFI->Verts.Num() - pFI->VertexNormals.Num());
				if (tgtIdx < pFI->VertexNormals.Num())
					(*pFI).VertexNormals(tgtIdx) = finalNormal;
			}
		}
	}

	// --- Compute per-vertex tangents & bitangents for normal-mapped phong shading ---
	// Requires: SurfaceInfoMap populated and SurfaceInfoMap[i].VertexNormals filled.
	const float kEps = 1e-8f;
	for (INT surfIdx = 0; surfIdx < Level->Model->Surfs.Num(); ++surfIdx)
	{
		// Find SurfaceInfo entry
		FSurfInfo* FI = SurfaceInfoMap.Find(surfIdx);
		if (!FI || FI->Verts.Num() < 3 || FI->Nodes.Num() == 0)
			continue;

		const int Vcount = FI->Verts.Num();
		// Ensure arrays
		if (FI->VertexNormals.Num() != Vcount)
		{
			// fallback: initialize normals from stored surface normal
			FI->VertexNormals.AddZeroed(Vcount);
			for (int vi = 0; vi < Vcount; ++vi)
				FI->VertexNormals(vi) = FI->SurfaceNormal;
		}
		FI->Tangents.AddZeroed(Vcount);
		FI->Bitangents.AddZeroed(Vcount);

		// Build local planar basis for this surface (centroid + X/Y used as UV proxy) how about using the real UV ffs
		FVector FacetPos(0,0,0);
		for (int i = 0; i < Vcount; ++i) FacetPos += FI->Verts(i);
		FacetPos /= (float)Vcount;

		FVector N = FI->SurfaceNormal;
		if (N.IsNearlyZero()) N = FVector(0,0,1);
		else N = N.SafeNormal();

		FVector X;
		if (Abs(N.X) > Abs(N.Z))
			X = FVector(-N.Y, N.X, 0.f);
		else
			X = FVector(0.f, -N.Z, N.Y);
		X = X.SafeNormal();
		FVector Y = (N ^ X).SafeNormal();

		// Precompute per-vertex UVs in that local basis (u = dot with X, v = dot with Y) aka fake ass not real UV or tangents
		TArray<glm::vec2> UV;
		UV.AddZeroed(Vcount);
		for (int i = 0; i < Vcount; ++i)
		{
			//FVector d = FI->Verts(i) - FacetPos;
			//UV(i).x = (d | X);
			//UV(i).y = (d | Y);
			UV(i).x = FI->UVs(i).X;
			UV(i).y = FI->UVs(i).Y;
		}

		// For each polygon, triangulate as fan and accumulate triangle tangents/bitangents
		for (INT p = 0; p < FI->Nodes.Num(); ++p)
		{
			const TArray<glm::uint>& poly = FI->Nodes(p).VertIndices;
			const int m = poly.Num();
			if (m < 3) continue;

			// fan triangulation: (0, i, i+1)
			for (int i = 1; i < m - 1; ++i)
			{
				int ia = poly(0);
				int ib = poly(i);
				int ic = poly(i + 1);
				if (ia < 0 || ib < 0 || ic < 0 || ia >= Vcount || ib >= Vcount || ic >= Vcount)
					continue;

				const FVector& p0 = FI->Verts(ia);
				const FVector& p1 = FI->Verts(ib);
				const FVector& p2 = FI->Verts(ic);

				const glm::vec2& uv0 = UV(ia);
				const glm::vec2& uv1 = UV(ib);
				const glm::vec2& uv2 = UV(ic);

				FVector dp1 = p1 - p0;
				FVector dp2 = p2 - p0;

				float du1 = uv1.x - uv0.x;
				float dv1 = uv1.y - uv0.y;
				float du2 = uv2.x - uv0.x;
				float dv2 = uv2.y - uv0.y;

				float denom = du1 * dv2 - du2 * dv1;
				if (fabsf(denom) <= kEps)
					continue;

				float r = 1.0f / denom;

				// tangent (points in direction of +U), bitangent (points in direction of +V)
				FVector T = (dp1 * dv2 - dp2 * dv1) * r;
				FVector B = (dp2 * du1 - dp1 * du2) * r;

				// Accumulate (we'll orthonormalize & normalize per-vertex after accumulation)
				FI->Tangents(ia) += T;
				FI->Tangents(ib) += T;
				FI->Tangents(ic) += T;

				FI->Bitangents(ia) += B;
				FI->Bitangents(ib) += B;
				FI->Bitangents(ic) += B;
			}
		}

		// Orthonormalize per-vertex tangent to the smoothed normal and reconstruct bitangent with handedness
		// Final per-vertex orthonormalization and handedness
		for (int vi = 0; vi < Vcount; ++vi)
		{
			FVector n = FI->VertexNormals(vi);
			if (n.IsNearlyZero())
				n = N; // fallback

			FVector t = FI->Tangents(vi);
			FVector b = FI->Bitangents(vi);

			n = n.SafeNormal();

			// Gram–Schmidt T against N
			t = (t - n * (n | t));
			if (t.IsNearlyZero())
			{
				// fallback tangent
				t = (n ^ FVector(1,0,0));
				if (t.SizeSquared() <= 1e-8f)
					t = (n ^ FVector(0,1,0));
			}
			t = t.SafeNormal();

			// Gram–Schmidt B against N and T
			b = b - n * (n | b) - t * (t | b);
			if (b.IsNearlyZero())
			{
				// fallback bitangent = N × T
				b = (n ^ t);
			}
			b = b.SafeNormal();

			// Compute handedness from final orthonormal TBN 
			//float handed = ((n ^ t) | b) < 0.f ? -1.f : 1.f;

			// --- Store final tangent and bitangent ---
			FI->Tangents(vi)   = t;
			FI->Bitangents(vi) = b;
			//FI->TangentHandedness(vi) = handed;   // or store in tangent.w if desired
		}

	}
}

void UXOpenGLRenderDevice::BuildSurfaceTriangulation(ULevel* Level)
{
    if (!Level || !Level->Model)
        return;

    for (INT iSurf = 0; iSurf < Level->Model->Surfs.Num(); ++iSurf)
    {
		FBspSurf& Surf = Level->Model->Surfs(iSurf);
		DWORD PolyFlags = Surf.PolyFlags;

		if (PolyFlags & (PF_Modulated | PF_FakeBackdrop | PF_NoSmooth |
						 PF_Flat | PF_Highlighted | //PF_Unlit |
						 PF_FlatShaded | PF_Portal))
		{
			continue; // skip this surface entirely, not solid
		}

        FSurfInfo* pSI = SurfaceInfoMap.Find(iSurf);
        if (!pSI)
            continue;

        FSurfInfo& SI = *pSI;
        const INT vcount = SI.Verts.Num();
        if (vcount < 3 || SI.Nodes.Num() == 0)
            continue;

        // Create an empty tri index array for this surface
        TArray<glm::uint>& TriIdx = SI.TriIdx;
        TriIdx.Empty();

        const INT numNodes = SI.Nodes.Num();
        for (INT ni = 0; ni < numNodes; ++ni)
        {
            FNodeInfo& NI = SI.Nodes(ni);

            const INT NumPts = NI.VertIndices.Num();
            if (NumPts < 3)
            {
                NI.TriStart = TriIdx.Num();
                NI.TriCount = 0;
                continue;
            }

            const INT triStart = TriIdx.Num();
            const INT triCount = NumPts - 2;

            for (INT i = 0; i < triCount; ++i)
            {
                const INT ia = 0;
                const INT ib = i + 1;
                const INT ic = i + 2;

                const INT va = (INT)NI.VertIndices(ia);
                const INT vb = (INT)NI.VertIndices(ib);
                const INT vc = (INT)NI.VertIndices(ic);

                if (va < 0 || vb < 0 || vc < 0 ||
                    va >= vcount || vb >= vcount || vc >= vcount)
                    continue;

                const FVector& PwA = SI.Verts(va);
                const FVector& PwB = SI.Verts(vb);
                const FVector& PwC = SI.Verts(vc);

                const FVector e1   = PwB - PwA;
                const FVector e2   = PwC - PwA;
                const FVector triN = e1 ^ e2;
                if (triN.SizeSquared() < 1e-8f)
                    continue;

                TriIdx.AddItem((glm::uint)va);
                TriIdx.AddItem((glm::uint)vb);
                TriIdx.AddItem((glm::uint)vc);
            }

            const INT triEnd = TriIdx.Num();
            NI.TriStart = triStart;
            NI.TriCount = triEnd - triStart; // multiple of 3, may be 0
        }
    }
}

UBOOL UXOpenGLRenderDevice::PointInPolyProjected(
    const FVector& P,                 // 3D point to test
    const TArray<FVector>& PolyVerts, // Polygon vertices in 3D
    const FVector& PlaneBase,         // Reference point on the polygon plane
    const FVector& PlaneNormal,       // Polygon plane normal vector
    FLOAT Epsilon                     // Precision tolerance threshold
)
{
    INT m = PolyVerts.Num();
    if (m < 3) return 0;

    // Build a stable, localized 2D basis coordinate system (U, V) orthogonal to the normal
    FVector N = PlaneNormal.SafeNormal();
    FVector U;
    if (abs(N.X) > abs(N.Z))
        U = FVector(-N.Y, N.X, 0.f).SafeNormal();
    else
        U = FVector(0.f, -N.Z, N.Y).SafeNormal();

    FVector V = (N ^ U).SafeNormal();

    // Project our primary evaluation point relative to our local coordinate origin
    FVector Pref = P - PlaneBase;
    FLOAT px = Pref | U;
    FLOAT py = Pref | V;

    INT wn = 0; // Initialize our winding number cross counter tracks

    for (INT i = 0; i < m; ++i)
    {
        const FVector& A3 = PolyVerts(i);
        const FVector& B3 = PolyVerts((i + 1) % m);

        FVector A = A3 - PlaneBase;
        FVector B = B3 - PlaneBase;

        FLOAT ax = A | U;
        FLOAT ay = A | V;
        FLOAT bx = B | U;
        FLOAT by = B | V;

        // Calculate the cross product of the edge vector relative to our test point
        FLOAT cross = (bx - ax) * (py - ay) - (px - ax) * (by - ay);

        // Explicit edge-proximity override check
        if (abs(cross) < Epsilon)
        {
            FLOAT dotProduct = (px - ax) * (bx - ax) + (py - ay) * (by - ay);
            FLOAT len2 = (bx - ax) * (bx - ax) + (by - ay) * (by - ay);

            if (dotProduct >= -Epsilon && dotProduct <= len2 + Epsilon)
                return 1; // Explicitly treat hard edge collisions as inside bounds
        }

        // Evaluate winding counter switches based on crossing positions
        if (ay <= py + Epsilon)
        {
            if (by > py + Epsilon && cross > Epsilon)
                wn ^= 1;
        }
        else
        {
            if (by <= py + Epsilon && cross < -Epsilon)
                wn ^= 1;
        }
    }

    return wn != 0; // Returns TRUE if the point sits cleanly enveloped inside the polygon bounds
}

INT UXOpenGLRenderDevice::TestRay(
    const FVector& TargetPoint,
    const FCustomSkySurface& SurfX,
    const FCustomSkySurface& SurfY,
    const FSurfInfo* SIY,
    const FVector& SkyCenter)
{
    // A surface can never evaluate against its own infinite plane
    if (SurfX.iSurf == SurfY.iSurf)
        return 0;

    // Build a clean directional line-of-sight ray from the view origin straight toward the target point
    FVector RayDir = (TargetPoint - SkyCenter).SafeNormal();

    // Find exactly where that ray pierces SurfY's infinite mathematical plane
    float Denom = SurfY.PlaneNormal | RayDir;
    if (abs(Denom) < 0.0001f)
        return 0; // Parallel ray -> no intersection

    float t = ((SurfY.PlaneBase - SkyCenter) | SurfY.PlaneNormal) / Denom;
    if (t <= 0.0f) 
        return 0; // Intersection point is behind the camera

    FVector HitPoint = SkyCenter + (RayDir * t);

    // Verify if that intersection point actually lands inside SurfY's true 3D polygon bounds
    if (!PointInPolyProjected(HitPoint, SIY->Verts, SurfY.PlaneBase, SurfY.PlaneNormal, 0.5f))
        return 0; // No angular overlap along this direction vector

    // Depth comparison along this shared line-of-sight ray
    FLOAT DistX = (TargetPoint - SkyCenter).Size();
    FLOAT DistY = (HitPoint    - SkyCenter).Size();

    if (abs(DistX - DistY) < 0.1f)
        return 0; // Vertices are intersecting/co-planar, don't swap

    // THE INVARIANT DEPTH RULE:
    // If SurfX is physically CLOSER to the camera origin than SurfY along this angle (DistX < DistY),
    // then SurfX is in front of SurfY. It must return +1.
    // If SurfX is FURTHER AWAY (DistX > DistY), it sits behind SurfY. It must return -1.
    return (DistX < DistY) ? +1 : -1;
}

INT UXOpenGLRenderDevice::TestOverlapAndDepth(
    const FCustomSkySurface& A,
    const FCustomSkySurface& B,
    const FVector& SkyCenter,
    const TMap<INT, FSurfInfo>& SurfaceInfoMap)
{
    const FSurfInfo* SIA = SurfaceInfoMap.Find(A.iSurf);
    const FSurfInfo* SIB = SurfaceInfoMap.Find(B.iSurf);
    if (!SIA || !SIB)
        return 0;

    // --- 1. Center ray: Origin -> A.center projected onto B ---
    {
        INT r = TestRay(A.PolyCenter, A, B, SIB, SkyCenter);
        if (r == +1) return +1; // A is closer than B along this ray -> A must draw AFTER B!
        if (r == -1) return -1; // A is further than B along this ray -> A must draw BEFORE B!
    }

    // --- 2. Center ray: Origin -> B.center projected onto A ---
    {
        INT r = TestRay(B.PolyCenter, B, A, SIA, SkyCenter);
        if (r == +1) return -1; // B is closer than A -> A is further than B -> A must draw BEFORE B!
        if (r == -1) return +1; // B is further than A -> A is closer than B -> A must draw AFTER B!
    }

    // --- 3a. Vertex rays: Origin -> each vertex of A projected onto B ---
    for (INT v = 0; v < SIA->Verts.Num(); ++v)
    {
        INT r = TestRay(SIA->Verts(v), A, B, SIB, SkyCenter);
        if (r == +1) return +1;
        if (r == -1) return -1;
    }

    // --- 3b. Vertex rays: Origin -> each vertex of B projected onto A ---
    for (INT v = 0; v < SIB->Verts.Num(); ++v)
    {
        INT r = TestRay(SIB->Verts(v), B, A, SIA, SkyCenter);
        if (r == +1) return -1; 
        if (r == -1) return +1; 
    }

    return 0; // No angular overlap detected along any shared sightlines
}



void UXOpenGLRenderDevice::ExtractSkyboxGeometry(ULevel* Level)
{
    guard(UXOpenGLRenderDevice::ExtractSkyboxGeometry);
    LocalSkySurfaces.Empty();

    // Structural safety check
    if (!Level || !Level->Model)
        return;

    UModel* Model = Level->Model;

    // harvest data by iterating over our pre-computed SurfaceInfoMap
    // Harvest data by iterating over our pre-computed SurfaceInfoMap
    for (TMap<INT, FSurfInfo>::TIterator It(SurfaceInfoMap); It; ++It)
    {
        FSurfInfo& SI = It.Value();

        if (SI.IsSky && SI.TriIdx.Num() > 0)
        {
            const FBspSurf& Surf = Model->Surfs(SI.iSurf);
            FCustomSkySurface SkySurf;

            SkySurf.iSurf   = SI.iSurf;
            SkySurf.SkyZone = SI.SkyZoneActor;

            // Cache the physical engine-side panning parameters right now!
            SkySurf.BasePanU = Surf.PanU;
            SkySurf.BasePanV = Surf.PanV;

            // Populate persistent geometric anchors straight into our structural proxy
            SkySurf.PlaneBase   = Model->Points(Surf.pBase);
            SkySurf.PlaneNormal = Model->Vectors(Surf.vNormal).SafeNormal();

            // Calculate the true geometric center point of the mesh face by averaging world vertices
            FVector Center(0.f, 0.f, 0.f);
            for (INT v = 0; v < SI.Verts.Num(); ++v)
            {
                Center += SI.Verts(v);
            }
            if (SI.Verts.Num() > 0)
            {
                Center *= (1.0f / static_cast<FLOAT>(SI.Verts.Num()));
            }
            else
            {
                Center = SkySurf.PlaneBase;
            }
            SkySurf.PolyCenter = Center;

            // Use the physical surface bounding radius or flat vertex bounds to estimate scale bounds
            SkySurf.BoundingRadius = SI.Verts.Num() > 0 ? (SI.Verts(0) - Center).Size() : 100.0f;

            // Capture the specific scrolling speeds governed by the sky room's zone setup
            if (SI.SkyZoneActor)
            {
                SkySurf.TexUPanSpeed = SI.SkyZoneActor->TexUPanSpeed;
                SkySurf.TexVPanSpeed = SI.SkyZoneActor->TexVPanSpeed;

                const FVector SkyCenter = SI.SkyZoneActor->Location;

                // 1. Establish the primary line-of-sight viewing direction for this specific panel
                const FVector ViewDir = (SkySurf.PolyCenter - SkyCenter).SafeNormal();

                // 2. Find the deepest vertex boundary, projected strictly along this localized viewing axis!
                // This shields the calculation from infinite flat-plane stretching corners,
                // capturing the true absolute depth right where the sightlines intersect!
                FLOAT MaxProjectedDepth = 0.0f;
                for (INT v = 0; v < SI.Verts.Num(); ++v)
                {
                    FLOAT ProjectedDepth = (SI.Verts(v) - SkyCenter) | ViewDir;
                    if (ProjectedDepth > MaxProjectedDepth)
                    {
                        MaxProjectedDepth = ProjectedDepth;
                    }
                }

                // 3. Store the squared projected maximum depth as your clean sorting anchor key
                SkySurf.SortingDistanceSq = MaxProjectedDepth * MaxProjectedDepth;
            }
            else
            {
                SkySurf.TexUPanSpeed = 0.0f;
                SkySurf.TexVPanSpeed = 0.0f;
                SkySurf.SortingDistanceSq = 99999999.0f; // Force unzoned artifacts to the back
            }

            LocalSkySurfaces.AddItem(SkySurf);
        }
    }

    // Pairwise relational line-of-sight directional depth sort
    if (LocalSkySurfaces.Num() > 1)
    {
        const FVector SkyCenter =
            (ActiveWorldSkyZoneActors.Num() > 0 && ActiveWorldSkyZoneActors(0)->SkyZone)
            ? ActiveWorldSkyZoneActors(0)->SkyZone->Location
            : FVector(0.f, 0.f, 0.f);

		for (INT i = 0; i < LocalSkySurfaces.Num() - 1; ++i)
		{
			INT FurthestIndex = i;

			for (INT j = i + 1; j < LocalSkySurfaces.Num(); ++j)
			{
				// Always compare the current reigning "furthest" candidate against the field
				const FCustomSkySurface& CandidateFurthest = LocalSkySurfaces(FurthestIndex);
				const FCustomSkySurface& CurrentSurf = LocalSkySurfaces(j);

				INT order = TestOverlapAndDepth(CandidateFurthest, CurrentSurf, SkyCenter, SurfaceInfoMap);

				// If order > 0, CandidateFurthest is actually CLOSER to the camera than CurrentSurf.
				// That means CurrentSurf is a better candidate to be pushed to the back (index i).
				if (order > 0)
				{
					FurthestIndex = j;
				}
			}

			// Perform a single, clean swap per outer loop pass
			if (FurthestIndex != i)
			{
				FCustomSkySurface Temp = LocalSkySurfaces(i);
				LocalSkySurfaces(i) = LocalSkySurfaces(FurthestIndex);
				LocalSkySurfaces(FurthestIndex) = Temp;
			}
		}

    }

	capturedAllSkyboxData = !(LocalSkySurfaces.Num() > 0);

	// Scan the level's zone database to find every world area pointing to a sky portal
	ActiveWorldSkyZoneActors.Empty();
    for (INT z = 0; z < 64; ++z)
    {
        AZoneInfo* WorldZoneActor = Model->Zones[z].ZoneActor;
        if (WorldZoneActor && WorldZoneActor->SkyZone)
        {
            // Verify it matches the authentic engine-side sky camera class properties
            if (WorldZoneActor->SkyZone->IsA(ASkyZoneInfo::StaticClass()))
            {
                ActiveWorldSkyZoneActors.AddItem(WorldZoneActor);
            }
        }
    }

    // Log the direct outcome to the developer log console
    debugf(TEXT("XOpenGL extracted %d skybox surfaces for custom rendering and blinded engine core loops."), LocalSkySurfaces.Num());

    unguard;
}


void UXOpenGLRenderDevice::NewLevelBSP()
{
	    
	// Build smooth vertex normals for phong shading (precompute once per level)
	BuildSmoothVertexNormalsForLevel(LastLevel);
    BuildSurfaceTriangulation(LastLevel);
	ExtractSkyboxGeometry(LastLevel);
}

