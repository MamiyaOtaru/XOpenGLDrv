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

		// Get or create FSurfInfo
		FSurfInfo* pSI = SurfaceInfoMap.Find(iSurf);
		if (!pSI)
		{
			SurfaceInfoMap.Set(iSurf, FSurfInfo());
			pSI = SurfaceInfoMap.Find(iSurf);

			pSI->SurfaceNormal = Level->Model->Vectors(Surf.vNormal).SafeNormal();
			pSI->IsMover = isMover;
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
						 PF_Flat | PF_Unlit | PF_Highlighted |
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

bool FacetInsidePolygonUV(
    const TArray<FVector>& facetVerts,
    const TArray<FVector>& polyVerts,
    const FVector& surfU,
    const FVector& surfV,
    const FVector& surfNormal,
	float epsilon = 1.0f)
{
    if (polyVerts.Num() < 3 || facetVerts.Num() == 0)
        return false;

    // Normalize U/V to form a stable 2D basis
    FVector U = surfU.SafeNormal();
    FVector V = surfV.SafeNormal();

    // Reference point for projection
    const FVector Aref = polyVerts(0);

    auto projectUV = [&](const FVector& P)
    {
        FVector d = P - Aref;
        return FVector(double(d | U), double(d | V), 0.f);
    };

    auto pointInPoly2D = [&](const FVector& P)->bool
    {
        FVector p = projectUV(P);
        int wn = 0;
        int m = polyVerts.Num();

        for (int i = 0; i < m; ++i)
        {
            FVector a = projectUV(polyVerts(i));
            FVector b = projectUV(polyVerts((i + 1) % m));

            // On-edge check
            double cross = (b.X - a.X)*(p.Y - a.Y) - (p.X - a.X)*(b.Y - a.Y);
            if (fabs(cross) < epsilon)
            {
                double dot = (p.X - a.X)*(b.X - a.X) + (p.Y - a.Y)*(b.Y - a.Y);
                double len2 = (b - a).SizeSquared();
                if (dot >= -epsilon && dot <= len2 + epsilon)
                    return true;
            }

            // Winding test
            if (((a.Y <= p.Y + epsilon) && (b.Y > p.Y + epsilon) && cross > epsilon) ||
                ((a.Y > p.Y + epsilon) && (b.Y <= p.Y + epsilon) && cross < -epsilon))
            {
                wn ^= 1;
            }
        }

        return wn != 0;
    };

    // Require all facet verts inside
    for (int fi = 0; fi < facetVerts.Num(); ++fi)
    {
        if (!pointInPoly2D(facetVerts(fi)))
            return false;
    }

    return true;
}

bool FacetInsidePolygon(
    const TArray<FVector>& facetVerts,
    const TArray<FVector>& polyVerts,
    const FVector& polyNormal   // normalized
)
{
    if (polyVerts.Num() < 3 || facetVerts.Num() == 0)
        return false;

    // --- Stable 2D basis from polygon normal ---
    FVector X;
    if (Abs(polyNormal.X) > Abs(polyNormal.Z))
        X = FVector(-polyNormal.Y, polyNormal.X, 0.f).SafeNormal();
    else
        X = FVector(0.f, -polyNormal.Z, polyNormal.Y).SafeNormal();

    FVector Y = (polyNormal ^ X).SafeNormal();

    const FVector Aref = polyVerts(0);

    const double eps = .25;

    auto pointInPoly2D = [&](const FVector& P)->bool
	{
		double px = double((P - Aref) | X);
		double py = double((P - Aref) | Y);

		int wn = 0;
		int m = polyVerts.Num();

		for (int ii = 0; ii < m; ++ii)
		{
			const FVector& P1 = polyVerts(ii);
			const FVector& P2 = polyVerts((ii + 1) % m);

			double x1 = double((P1 - Aref) | X), y1 = double((P1 - Aref) | Y);
			double x2 = double((P2 - Aref) | X), y2 = double((P2 - Aref) | Y);

			// On-edge check
			double cross = (x2 - x1)*(py - y1) - (px - x1)*(y2 - y1);
			if (fabs(cross) < eps)
			{
				double dot = (px - x1)*(x2 - x1) + (py - y1)*(y2 - y1);
				if (dot >= -eps)
				{
					double len2 = (x2 - x1)*(x2 - x1) + (y2 - y1)*(y2 - y1);
					if (dot <= len2 + eps)
						return true; // treat on-edge as inside
				}
			}

			// Winding test with epsilon
			if (((y1 <= py + eps) && (y2 > py + eps) && cross > eps) ||
				((y1 > py + eps) && (y2 <= py + eps) && cross < -eps))
			{
				wn ^= 1;
			}
		}

		return wn != 0;
	};

    // --- Require all facet verts inside ---
    for (INT fvi = 0; fvi < facetVerts.Num(); ++fvi)
    {
        if (!pointInPoly2D(facetVerts(fvi)))
            return false;
    }

    return true;
}

bool FacetInsidePolygonUV_3D(
    const TArray<FVector>& facetVerts,
    const TArray<FVector>& polyVerts,
    const FVector& surfU,
    const FVector& surfV,
    const FVector& surfNormal,
	float epsilon = 1.0f)
{
    int pv = polyVerts.Num();
    if (pv < 3) return false;

    int fv = facetVerts.Num();
    if (fv == 0) return false;

    for (int fi = 0; fi < fv; ++fi)
    {
        const FVector& P = facetVerts(fi);

        for (int pi = 0; pi < pv; ++pi)
        {
            const FVector& A = polyVerts(pi);
            const FVector& B = polyVerts((pi + 1) % pv);

            FVector edge = B - A;
            FVector outward = edge ^ surfNormal; // outward half-space

            float d = (P - A) | outward;

            if (d > epsilon) // outside this edge
                return false;
        }
    }

    return true;
}

bool FacetInsidePolygon3D(const TArray<FVector>& facetVerts,
                          const TArray<FVector>& polyVerts,
                          const FVector& polyNormal)
{	
	int pv = polyVerts.Num();
    if (pv < 3) return false;

    int fv = facetVerts.Num();
    if (fv == 0) return false;

    for (int fi = 0; fi < fv; ++fi)
	{
        FVector P = facetVerts(fi);
        for (int pi = 0; pi < pv; ++pi)
        {
            const FVector& A = polyVerts(pi);
            const FVector& B = polyVerts((pi+1) % pv);

            FVector edge = B - A;
            FVector outward = edge ^ polyNormal;  // outward-facing plane

            float d = (P - A) | outward;
            //if (d > 0.0001f)   // outside
			if (d > 1.0f)   // outside
                return false;
        }
    }

    return true;
}

void UXOpenGLRenderDevice::NewLevelBSP()
{
	    
	// Build smooth vertex normals for phong shading (precompute once per level)
	BuildSmoothVertexNormalsForLevel(LastLevel);
    BuildSurfaceTriangulation(LastLevel);
}

