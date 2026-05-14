/*=============================================================================
	DrawComplex.cpp: Unreal XOpenGL DrawComplexSurface routines.
	Used for BSP drawing.

	Copyright 2014-2021 Oldunreal

	Revision history:
		* Created by Smirftsch
=============================================================================*/

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "XOpenGLDrv.h"
#include "XOpenGL.h"
#include "ExternalTextureLoader.h"
#include <algorithm>
#include <vector>
#include <cmath>


/*-----------------------------------------------------------------------------
	Helpers
-----------------------------------------------------------------------------*/

static void SetTextureHelper
(
	UXOpenGLRenderDevice* RenDev,
	INT Multi,
	FTextureInfo& Info,
	DWORD PolyFlags,
	DWORD& DrawFlags,
	DWORD AddDrawFlag,
	FLOAT PanBias,
	glm::vec4* TextureCoords,
	glm::vec4* TextureInfo,
	glm::uint64* TexHandles
)
{
	RenDev->SetTexture(Multi, Info, PolyFlags, PanBias);
	if (TextureCoords)
		*TextureCoords = glm::vec4(RenDev->TexInfo[Multi].UMult, RenDev->TexInfo[Multi].VMult, RenDev->TexInfo[Multi].UPan, RenDev->TexInfo[Multi].VPan);
	if (TextureInfo)
		*TextureInfo = glm::vec4(Info.Texture->Diffuse > 0.f ? Info.Texture->Diffuse : 1.f, Info.Texture->Specular, Info.Texture->Alpha > 0.f ? Info.Texture->Alpha : 1.f, Info.Texture->TEXTURE_SCALE_NAME);
	TexHandles[Multi] = RenDev->TexInfo[Multi].BindlessTexHandle;
	DrawFlags |= AddDrawFlag;
}

void UXOpenGLRenderDevice::DumpSurfInfo(INT iSurf, const FSurfInfo& SI)
{
	debugf(TEXT("Dumping FSurfInfo for iSurf %d"), iSurf);
	debugf(TEXT("Verts: %d, UVs: %d, Nodes: %d"), SI.Verts.Num(), SI.UVs.Num(), SI.Nodes.Num());

	for (INT vi = 0; vi < SI.Verts.Num(); ++vi)
	{
		const FVector& V = SI.Verts(vi);
		const FVector& UV = SI.UVs.IsValidIndex(vi) ? SI.UVs(vi) : FVector(0, 0, 0);
		debugf(TEXT("  Vert %d: Pos=(%f,%f,%f) UV=(%f,%f)"),
			vi, V.X, V.Y, V.Z, UV.X, UV.Y);
	}

	for (INT ni = 0; ni < SI.Nodes.Num(); ++ni)
	{
		const FNodeInfo& NI = SI.Nodes(ni);
		debugf(TEXT("  Node %d: PlaneN=(%f,%f,%f) W=%f, NumIndices=%d"),
			ni, NI.PlaneNormal.X, NI.PlaneNormal.Y, NI.PlaneNormal.Z,
			NI.PlaneW, NI.VertIndices.Num());
	}

	// Dump TriIdx if present
	if (SI.TriIdx.Num() > 0)
	{
		FString triStr;
		for (INT ti = 0; ti < SI.TriIdx.Num(); ++ti)
		{
			triStr += FString::Printf(TEXT("%d "), SI.TriIdx(ti));
		}
		debugf(TEXT("    TriIdx: %s"), *triStr);
	}


}


/*-----------------------------------------------------------------------------
	RenDev Interface
-----------------------------------------------------------------------------*/
void UXOpenGLRenderDevice::DrawComplexSurface(FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet)
{
	guard(UXOpenGLRenderDevice::DrawComplexSurface);

	if (NoDrawComplexSurface)
		return;

	check(Surface.Texture);

	// Gather options
	DWORD DrawFlags = ShaderDrawFlags::DF_None;
	const DWORD NextPolyFlags = GetPolyFlagsAndDrawFlags(Surface.PolyFlags, DrawFlags, FALSE);
	if (GIsEditor && NextPolyFlags & PF_Selected)
		DrawFlags |= ShaderDrawFlags::DF_Selected;

	bool IsSolidBSP = (Frame->Recursion == 0) && !(NextPolyFlags & (PF_Modulated | PF_FakeBackdrop | PF_NoSmooth | PF_Flat | PF_Unlit | PF_Highlighted | PF_FlatShaded | PF_Portal));

	INT facetSurfId = INDEX_NONE;
	FSurfInfo* SI = nullptr;
	if (IsSolidBSP)
	{
		facetSurfId = GetFacetSurfId(Frame, Facet);
		if (facetSurfId != INDEX_NONE)
		{
			SI = SurfaceInfoMap.Find(facetSurfId);
			// check if already rendered this surface
			if (PhongShading && BumpMaps && SI && !SI->IsMover && SI->LastDrawnFrame == LocalFrameCounter)
			{
				// already drawn this frame, skip
				return;
			}
			if (SI)
				SI->LastDrawnFrame = LocalFrameCounter;
		}
	}

	auto Shader = dynamic_cast<DrawComplexProgram*>(Shaders[Complex_Prog]);

	STAT(clockFast(Stats.ComplexCycles));
	SetProgram(Complex_Prog);

	TArray<glm::uint> facetIndices;
	int staticCount = 0;
	int dynamicCount = 0;

	if (BumpMaps && IsSolidBSP) { // do per pixel lighting

		TArray<AActor*> staticList;
		TArray<AActor*> dynamicList;

		if (facetSurfId == INDEX_NONE || SI && SI->IsMover)
		{
			ComputeStaticAndDynamicLightsForFacet(Frame, Facet, staticList, dynamicList, LevelLightCap);
		}
		else
		{
			TArray<AActor*>* SurfaceLightList = StaticLightsForFacet.Find(facetSurfId);
			if (!SurfaceLightList)
			{
				TArray<AActor*> list;
				ComputeStaticLightsForFacet(Frame->Level, facetSurfId, list, LevelLightCap - 10);
				StaticLightsForFacet.Set(facetSurfId, list);
				SurfaceLightList = StaticLightsForFacet.Find(facetSurfId);
			}

			// Dynamic lights (cheap)
			ComputeDynamicLightsForFacet(Frame->Level, facetSurfId, dynamicList);

			staticList = *SurfaceLightList;
		} // end else is static BSP facet with valid key
		
		int NumSurfaceLights = staticList.Num() + dynamicList.Num();
		if (NumSurfaceLights > LevelLightCap)
			NumSurfaceLights = LevelLightCap;
		//debugf(TEXT("XOpenGL: lights static dynamic: %d %d"), staticList.Num(), dynamicList.Num());
		// Build index array for this facet (map actors -> indices in LightInfoBuffer)
		facetIndices.Reserve(NumSurfaceLights);

		for (INT i = 0; i < NumSurfaceLights; ++i)
		{
			AActor* Actor;
			if (i < staticList.Num())
				Actor = staticList(i);
			else
				Actor = dynamicList(i - staticList.Num());
			if (!Actor) continue;
			GLuint* Found = CurrentLightToIndex.Find(Actor);
			if (Found)
			{
				facetIndices.AddItem(static_cast<glm::uint>(*Found));
				if (i < staticList.Num())
					staticCount++;
				else
					dynamicCount++;
			}
			else
			{
				// Actor not present in current LightInfo (dynamic reorder or omitted) -> skip
			}
			if (facetIndices.Num() >= LevelLightCap)
				break;
		}
	} // end if bumpmapping (gather lights)

	const bool CanBuffer = !Shader->DrawBuffer.IsFull()
		&& Shader->ParametersBuffer.CanBuffer(1)
		&& Shader->FacetIndexRing.CanBuffer(facetIndices.Num())
		&& Shader->FacetMetaRing.CanBuffer(1);

	// Check if this draw call will change any global state. If so, we want to flush any pending draw calls before we make the changes
	if (WillBlendStateChange(CurrentBlendPolyFlags, NextPolyFlags) || // Check if the blending mode will change
		WillTextureStateChange(DiffuseTextureIndex, *Surface.Texture, NextPolyFlags) || // Check if the surface textures will change
		(Surface.LightMap && WillTextureStateChange(LightMapIndex, *Surface.LightMap, NextPolyFlags)) ||
		((Surface.FogMap && Surface.FogMap->Mips[0] && Surface.FogMap->Mips[0]->DataPtr) && WillTextureStateChange(FogMapIndex, *Surface.FogMap, NextPolyFlags)) ||
		(Surface.DetailTexture && DetailTextures && WillTextureStateChange(DetailTextureIndex, *Surface.DetailTexture, NextPolyFlags)) ||
		(Surface.MacroTexture && MacroTextures && WillTextureStateChange(MacroTextureIndex, *Surface.MacroTexture, NextPolyFlags)) ||
#if ENGINE_VERSION==227
		(Surface.BumpMap && BumpMaps && WillTextureStateChange(BumpMapIndex, *Surface.BumpMap, NextPolyFlags)) ||
		(Surface.EnvironmentMap && EnvironmentMaps && WillTextureStateChange(EnvironmentMapIndex, *Surface.EnvironmentMap, NextPolyFlags)) ||
		(Surface.HeightMap && WillTextureStateChange(HeightMapIndex, *Surface.HeightMap, NextPolyFlags)) ||
#endif
		!CanBuffer)
	{
		// Dispatch buffered data
		Shader->Flush(!CanBuffer);

		// Update global GL state
		SetBlend(NextPolyFlags);
	}

	GLuint metaIndex = Shader->FacetMetaRing.SubBufferOffset + Shader->FacetMetaRing.NextElemIndex;
	// Compute the facet record pointer ONCE
	FFacetData* facetPtr = Shader->FacetMetaRing.GetCurrentElementPtr();
	facetPtr->LightMeta      = glm::uvec4(0, 0, 0, 0);
	facetPtr->StaticUVMinMax = glm::vec4(0);

	// Write light list meta (if BumpMaps enabled)
	if (BumpMaps)
	{
		GLuint startIndex = 0;
		GLuint count = static_cast<GLuint>(facetIndices.Num());

		if (count > 0)
		{
			// absolute start index in the big SSBO
			startIndex = Shader->FacetIndexRing.SubBufferOffset + Shader->FacetIndexRing.NextElemIndex;

			// copy indices
			glm::uint* dst = Shader->FacetIndexRing.GetCurrentElementPtr();
			for (UINT k = 0; k < count; ++k)
				dst[k] = facetIndices(k);

			Shader->FacetIndexRing.Advance(count);

			facetPtr->LightMeta = glm::uvec4(startIndex, staticCount, dynamicCount, 0);
		}
	}

	// Write static lightmap params (if present).  Only do mover if we have a Node match
	if (HDLightMap && GOcclusionState == EOcclusionState::Ready && SI && SI->HasHDLightmap)// && (!SI->IsMover || NI))
	{
		const FSurfaceLightmap& LM = SI->HDLightmap;
		facetPtr->StaticUVMinMax = glm::vec4(LM.AtlasMinU, LM.AtlasMaxU, LM.AtlasMinV, LM.AtlasMaxV);
		DrawFlags |= ShaderDrawFlags::DF_HDLightMap;
	}
	// Advance ONCE per facet
	Shader->FacetMetaRing.Advance(1);
	
	DrawComplexParameters* DrawCallParams = Shader->ParametersBuffer.GetCurrentElementPtr();

	// Editor Support.
	if (GIsEditor)
		DrawCallParams->DrawColor = HitTesting() ? FPlaneToVec4(HitColor) : FPlaneToVec4(Surface.FlatColor.Plane());

	// Set Textures
	SetTextureHelper(this, DiffuseTextureIndex, *Surface.Texture, NextPolyFlags, DrawFlags, ShaderDrawFlags::DF_DiffuseTexture, 0.0, &DrawCallParams->DiffuseUV, Surface.Texture->Texture ? &DrawCallParams->DiffuseInfo : nullptr, DrawCallParams->TexHandles);

	if (!Surface.Texture->Texture)
		DrawCallParams->DiffuseInfo = glm::vec4(1.f, 0.f, 0.f, 1.f);

	if (Surface.LightMap)
		SetTextureHelper(this, LightMapIndex, *Surface.LightMap, PF_None, DrawFlags, ShaderDrawFlags::DF_LightMap, -0.5, &DrawCallParams->LightMapUV, nullptr, DrawCallParams->TexHandles);

	if (Surface.FogMap && Surface.FogMap->Mips[0] && Surface.FogMap->Mips[0]->DataPtr)
		SetTextureHelper(this, FogMapIndex, *Surface.FogMap, PF_AlphaBlend, DrawFlags, ShaderDrawFlags::DF_FogMap, -0.5, &DrawCallParams->FogMapUV, nullptr, DrawCallParams->TexHandles);

	if (Surface.DetailTexture && DetailTextures)
		SetTextureHelper(this, DetailTextureIndex, *Surface.DetailTexture, PF_None, DrawFlags, ShaderDrawFlags::DF_DetailTexture, 0.0, &DrawCallParams->DetailUV, nullptr, DrawCallParams->TexHandles);

	if (Surface.MacroTexture && MacroTextures)
		SetTextureHelper(this, MacroTextureIndex, *Surface.MacroTexture, PF_None, DrawFlags, ShaderDrawFlags::DF_MacroTexture, 0.0, &DrawCallParams->MacroUV, &DrawCallParams->MacroInfo, DrawCallParams->TexHandles);

#if ENGINE_VERSION==227
	if (Surface.BumpMap && BumpMaps)
	{
		Shader->BumpMapInfo = Surface.BumpMap;
#else
	if (BumpMaps && Surface.Texture && Surface.Texture->Texture && Surface.Texture->Texture->BumpMap)
	{
# if ENGINE_VERSION==1100
		Surface.Texture->Texture->BumpMap->Lock(Shader->BumpMapInfo, Viewport->CurrentTime, 0, this);
# else
		Surface.Texture->Texture->BumpMap->Lock(Shader->BumpMapInfo, FTime(), 0, this);
# endif
#endif
		SetTextureHelper(this, BumpMapIndex, FTEXTURE_GET(Shader->BumpMapInfo), PF_None, DrawFlags, ShaderDrawFlags::DF_BumpMap, 0.0, nullptr, &DrawCallParams->BumpMapInfo, DrawCallParams->TexHandles);
	}

#if ENGINE_VERSION==227
	if (Surface.EnvironmentMap && EnvironmentMaps)
		SetTextureHelper(this, EnvironmentMapIndex, *Surface.EnvironmentMap, PF_None, DrawFlags, ShaderDrawFlags::DF_EnvironmentMap, 0.0, &DrawCallParams->EnviroMapUV, nullptr, DrawCallParams->TexHandles);

	if (Surface.HeightMap && ParallaxVersion != Parallax_Disabled)
		SetTextureHelper(this, HeightMapIndex, *Surface.HeightMap, PF_None, DrawFlags, ShaderDrawFlags::DF_HeightMap, 0.0, nullptr, &DrawCallParams->HeightMapInfo, DrawCallParams->TexHandles);
#endif

	QWORD parentID = Surface.Texture->CacheID;
	//bool hasDetail = (DetailTextures && IsSolidBSP && ExternalTexture::GetExtra(parentID, ExternalTexture::Extra_Detail) != nullptr);
	bool hasBump = (BumpMaps && IsSolidBSP && ExternalTexture::GetExtra(parentID, ExternalTexture::Extra_Bump) != nullptr);
	bool hasHeight = (ParallaxVersion != Parallax_Disabled && IsSolidBSP && ExternalTexture::GetExtra(parentID, ExternalTexture::Extra_Height) != nullptr);

	// ------------------------------------------------------------
	// BumpMapInfo (external version)
	// ------------------------------------------------------------
	if (hasBump)
	{
		FTextureInfo* ExternalBumpInfo =
			ExternalTexture::GetExtra(parentID, ExternalTexture::Extra_Bump);

		if (ExternalBumpInfo)
		{
			// Give the external FTextureInfo a valid UTexture* for metadata
			ExternalBumpInfo->Texture = Surface.Texture->Texture;

			SetTextureHelper(
				this,
				BumpMapIndex,                  // TMU index for bumpmap
				*ExternalBumpInfo,             // pass by reference
				PF_None,
				DrawFlags,
				ShaderDrawFlags::DF_BumpMap,
				0.0,
				nullptr,
				&DrawCallParams->BumpMapInfo,  // what the shader reads
				DrawCallParams->TexHandles
			);
		}
	}

	// ------------------------------------------------------------
	// HeightMapInfo (external-only, no engine-native height maps)
	// ------------------------------------------------------------
	if (hasHeight)
	{
		FTextureInfo* ExternalHeightInfo = ExternalTexture::GetExtra(parentID, ExternalTexture::Extra_Height);

		if (ExternalHeightInfo)
		{
			// Give the external FTextureInfo a valid UTexture* for metadata
			ExternalHeightInfo->Texture = Surface.Texture->Texture;

			SetTextureHelper(
				this,
				HeightMapIndex,                  // TMU index for heightmap
				*ExternalHeightInfo,             // pass by reference
				PF_None,
				DrawFlags,
				ShaderDrawFlags::DF_HeightMap,
				0.0,
				nullptr,
				&DrawCallParams->HeightMapInfo,  // what the shader reads
				DrawCallParams->TexHandles
			);

			DrawCallParams->HeightMapInfo.x = 0.0f;
			DrawCallParams->HeightMapInfo.y = 0.0f;
			DrawCallParams->HeightMapInfo.z = 2.0f;  // tunable parallax scale
			DrawCallParams->HeightMapInfo.w = 0.0f;   // no animation for now
		}
	}

	if (BumpMaps && Multipass && IsSolidBSP && (SI && !SI->IsMover)) // only works in per pixel
	{
		glActiveTexture(GL_TEXTURE0 + PostProcessIndex);
		glBindTexture(GL_TEXTURE_2D, SsaoFbo->colorTexIDs[0]);

		DrawCallParams->TexHandles[PostProcessIndex] =
			glGetTextureHandleARB(SsaoFbo->colorTexIDs[0]);

		glMakeTextureHandleResidentARB(DrawCallParams->TexHandles[PostProcessIndex]);

		DrawFlags |= ShaderDrawFlags::DF_Multipass;
	}

	if (BumpMaps && Multipass)
	{
		PreparePrepassDepthTexture();
		INT depthIndex = PrepassDepthIndex;
		DrawCallParams->TexHandles[depthIndex] = gbufferFbo->depthBindlessHandle;
	}

	if (SI && SI->HasHDLightmap &&GOcclusionState == EOcclusionState::Ready)
	{
		DrawCallParams->TexHandles[StaticLightmapIndex] = GStaticLightmapAtlasHandle;
	}

	// Other draw data
	DrawCallParams->XAxis = glm::vec4(Facet.MapCoords.XAxis.X, Facet.MapCoords.XAxis.Y, Facet.MapCoords.XAxis.Z, Facet.MapCoords.XAxis | Facet.MapCoords.Origin);
	DrawCallParams->YAxis = glm::vec4(Facet.MapCoords.YAxis.X, Facet.MapCoords.YAxis.Y, Facet.MapCoords.YAxis.Z, Facet.MapCoords.YAxis | Facet.MapCoords.Origin);
	DrawCallParams->ZAxis = glm::vec4(Facet.MapCoords.ZAxis.X, Facet.MapCoords.ZAxis.Y, Facet.MapCoords.ZAxis.Z, 0.0);
	if (BumpMaps)
		DrawCallParams->Roughness = GetRoughnessFromTextureName(Surface);
	if (PhongShading && BumpMaps && (SI && !SI->IsMover)) // phong only works in per pixel lighting mode
		DrawFlags |= ShaderDrawFlags::DF_PhongShading;
	bool safeToReadDepth = !(Surface.PolyFlags & PF_Occlude);
	if (safeToReadDepth && !IsSolidBSP && (Surface.PolyFlags & (PF_AlphaTexture | PF_Translucent)) && !(Surface.PolyFlags & PF_Semisolid))// && IsDepthFadeFX(Surface.Texture->Texture)) // don't fade out "non solid" that is really just unlit
	{
		DrawFlags |= ShaderDrawFlags::DF_ReadDepth;
		PrepareDepthTexture();
		INT depthIndex = SceneDepthIndex;
		DrawCallParams->TexHandles[depthIndex] = SceneFbo->depthBindlessHandle;
	}
	DrawCallParams->SceneWidth = SceneWidth;
	DrawCallParams->SceneHeight = SceneHeight;
	DrawCallParams->DrawFlags = DrawFlags;

	Shader->DrawBuffer.StartDrawCall();
	auto DrawID = Shader->DrawBuffer.GetDrawID();
	auto facetIDForVerts = metaIndex; // absolute index into FacetMeta SSBO

	INT FacetVertexCount = 0;
	TArray<glm::vec3> PolyVertices;
	TArray<glm::vec4> PolyVertexNormals;
	TArray<glm::vec4> PolyVertexTangents;
	TArray<glm::vec4> PolyVertexBitangents;
	TArray<glm::vec2> PolyVertexLightmapUVs;
	int NumPts = 0;

	// something in here is causing weirdness on movers when phong is on, though it should not trigger if it is a mover. ah, regular stuff under glowy bits zfights with phong on
	if (PhongShading && BumpMaps && SI && !SI->IsMover) // phong shading only works with "bumpmaps" aka per pixel lighting
	{
		// Per-surface precomputed normals (if available) and world-space verts for matching
		TArray<FVector>& SurfWorldVerts   = SI->Verts;
		TArray<FVector>& SurfNormals      = SI->VertexNormals;
		TArray<FVector>& SurfTangents     = SI->Tangents;
		TArray<FVector>& SurfBitangents   = SI->Bitangents;
		TArray<FVector>& SurfLightmapUVs  = SI->LightmapUVs;

		INT NumPts = SurfWorldVerts.Num();

		PolyVertices.Empty();
		PolyVertices.AddZeroed(NumPts);
		PolyVertexNormals.Empty();
		PolyVertexNormals.AddZeroed(NumPts);
		PolyVertexTangents.Empty();
		PolyVertexTangents.AddZeroed(NumPts);
		PolyVertexBitangents.Empty();
		PolyVertexBitangents.AddZeroed(NumPts);
		PolyVertexLightmapUVs.Empty();
		PolyVertexLightmapUVs.AddZeroed(NumPts);

		TArray<glm::uint>& TriIdx = SI->TriIdx;

		// Build per-vertex data for this node polygon
		for (INT vi = 0; vi < NumPts; ++vi)
		{
			FVector Vert      = SurfWorldVerts(vi).TransformPointBy(Frame->Coords);
			FVector Normal    = SurfNormals(vi).TransformVectorBy(Frame->Coords).SafeNormal();
			FVector Tangent   = SurfTangents(vi).TransformVectorBy(Frame->Coords).SafeNormal();
			FVector Bitangent = SurfBitangents(vi).TransformVectorBy(Frame->Coords).SafeNormal();

			PolyVertices(vi)          = glm::vec4(Vert.X, Vert.Y, Vert.Z, 0.0f);
			PolyVertexNormals(vi)     = glm::vec4(Normal.X, Normal.Y, Normal.Z, 0.0f);
			PolyVertexTangents(vi)    = glm::vec4(Tangent.X, Tangent.Y, Tangent.Z, 0.0f);
			PolyVertexBitangents(vi)  = glm::vec4(Bitangent.X, Bitangent.Y, Bitangent.Z, 0.0f);
			if (GOcclusionState == EOcclusionState::Ready && PolyVertexLightmapUVs.Num() == SurfLightmapUVs.Num())
				PolyVertexLightmapUVs(vi) = glm::vec2(SurfLightmapUVs(vi).X, SurfLightmapUVs(vi).Y);
			//else
			//	debugf(TEXT("XOpenGL: not enough UVs: %d %d"), PolyVertexLightmapUVs.Num(), SurfLightmapUVs.Num());
		}

		const INT numNodes = SI->Nodes.Num();
		for (INT ni = 0; ni < numNodes; ++ni)
		{
			const FNodeInfo& NI = SI->Nodes(ni);

			INT numTriVerts = NI.TriCount;
			INT triStart = NI.TriStart;
			INT triEnd = triStart + numTriVerts;

			if (NI.VertIndices.Num() < 3)
				continue;

			const INT neededVerts = numTriVerts;

			if (!Shader->VertBuffer.CanBuffer(neededVerts))
			{
				Shader->DrawBuffer.EndDrawCall(FacetVertexCount);
				Shader->ParametersBuffer.Advance(1);
				Shader->Flush(true);
				Shader->DrawBuffer.StartDrawCall();
				DrawID = Shader->DrawBuffer.GetDrawID();

				if (neededVerts >= Shader->VertexBufferSize)
				{
					debugf(TEXT("DrawComplexSurface facet too big (facet has %d vertices - need to buffer %d points - Vertex Buffer Size is %d)!"),
						NumPts, neededVerts, Shader->VertexBufferSize);
					continue;
				}

				FacetVertexCount = 0;
			}

			auto Out = Shader->VertBuffer.GetCurrentElementPtr();
			INT emittedVerts = 0;

			for (INT ti = triStart; ti < triEnd; ti += 3)
			{
				const INT ia = TriIdx(ti);
				const INT ib = TriIdx(ti + 1);
				const INT ic = TriIdx(ti + 2);

				// ---- Vertex 0 ----
				Out->Coords = PolyVertices(ia);
				Out->DrawID = DrawID;
				Out->Normal = PolyVertexNormals(ia);
				Out->Tangent = PolyVertexTangents(ia);
				Out->Bitangent = PolyVertexBitangents(ia);
				Out->FacetID = facetIDForVerts;
				Out->LightmapUV = PolyVertexLightmapUVs(ia);
				Out++;
				emittedVerts++;

				// ---- Vertex 1 ----
				Out->Coords = PolyVertices(ib);
				Out->DrawID = DrawID;
				Out->Normal = PolyVertexNormals(ib);
				Out->Tangent = PolyVertexTangents(ib);
				Out->Bitangent = PolyVertexBitangents(ib);
				Out->FacetID = facetIDForVerts;
				Out->LightmapUV = PolyVertexLightmapUVs(ib);
				Out++;
				emittedVerts++;

				// ---- Vertex 2 ----
				Out->Coords = PolyVertices(ic);
				Out->DrawID = DrawID;
				Out->Normal = PolyVertexNormals(ic);
				Out->Tangent = PolyVertexTangents(ic);
				Out->Bitangent = PolyVertexBitangents(ic);
				Out->FacetID = facetIDForVerts;
				Out->LightmapUV = PolyVertexLightmapUVs(ic);
				Out++;
				emittedVerts++;
			}

			FacetVertexCount += emittedVerts;
			Shader->VertBuffer.Advance(emittedVerts);
		} // end loop through Nodes
	} // end if SI (with normal data)
	else if (SI && ((HDLightMap && GOcclusionState == EOcclusionState::Ready) || (PhongShading && BumpMaps)))
	{
		const SurfaceBasis& Basis = SI->LightmapBasis;

		//debugf(TEXT("Facet: iSurf %d"), facetSurfId);
		// 
		// No smoothing data available: compute polygon normal from facet.MapCoords.ZAxis (already provided in DrawCallParams)
		// Flat fallback TBN from DrawCallParams axes
		glm::vec3 N = glm::normalize(DrawCallParams->ZAxis);
		glm::vec3 T = glm::vec3(DrawCallParams->XAxis);
		glm::vec3 B = glm::vec3(DrawCallParams->YAxis);
		// Ensure T is valid
		if (glm::length(T) < 1e-6f) {
			glm::vec3 up = (std::abs(N.z) < 0.999f) ? glm::vec3(0,0,1) : glm::vec3(0,1,0);
			T = glm::normalize(glm::cross(up, N));
		} else {
			T = glm::normalize(T);
		}
		// Gram–Schmidt T against N
		T = glm::normalize(T - N * glm::dot(N, T));
		// Recompute B from N×T to guarantee orthonormality
		B = glm::normalize(glm::cross(N, T));

		glm::vec4 fallbackN = glm::vec4(N, 0.0f);
		glm::vec4 fallbackT = glm::vec4(T, 0.0f);
		glm::vec4 fallbackB = glm::vec4(B, 0.0f);

		for (FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next)
		{
			NumPts = Poly->NumPts;

			PolyVertices.Empty();
			PolyVertices.AddZeroed(NumPts);
			PolyVertexNormals.Empty();
			PolyVertexNormals.AddZeroed(NumPts);
			PolyVertexTangents.Empty();
			PolyVertexTangents.AddZeroed(NumPts);
			PolyVertexBitangents.Empty();
			PolyVertexBitangents.AddZeroed(NumPts);
			PolyVertexLightmapUVs.Empty();
			PolyVertexLightmapUVs.AddZeroed(NumPts);

			// assign same flat TBN to all verts
			for (INT vi = 0; vi < NumPts; ++vi) 
			{
				const FVector& Pvs = Poly->Pts[vi]->Point;
				// Reproject into HD lightmap UV space
				PolyVertices(vi)         = glm::vec3(Pvs.X, Pvs.Y, Pvs.Z);
				PolyVertexNormals(vi)    = fallbackN;
				PolyVertexTangents(vi)   = fallbackT;
				PolyVertexBitangents(vi) = fallbackB;

				if (HDLightMap && GOcclusionState == EOcclusionState::Ready)
				{
					FVector Origin;
					if (SI->IsMover)
					{
						// Adjust origin for current mover position
						Origin = SI->Owner->Location + SI->HDLightmap.OriginOffset;
					}
					else
					{
						Origin = SI->LightmapBasis.Origin;
					}
					// Project into surf basis relative to adjusted origin
					FVector WorldPos = Pvs.TransformPointBy(Frame->Uncoords);
					FVector Local = WorldPos - Origin;
					float U = (Basis.TangentU | Local);
					float V = (Basis.TangentV | Local);

					// Normalize into [0,1] for this surf
					float uNorm = (U - SI->HDLightmap.SurfMinU) / (SI->HDLightmap.SurfMaxU - SI->HDLightmap.SurfMinU);
					float vNorm = (V - SI->HDLightmap.SurfMinV) / (SI->HDLightmap.SurfMaxV - SI->HDLightmap.SurfMinV);

					// Clamp to avoid bleed if mover goes outside baked extents
					uNorm = Clamp(uNorm, 0.0f, 1.0f);
					vNorm = Clamp(vNorm, 0.0f, 1.0f);

					// Remap into atlas space
					float uAtlas = SI->HDLightmap.AtlasMinU + uNorm * (SI->HDLightmap.AtlasMaxU - SI->HDLightmap.AtlasMinU);
					float vAtlas = SI->HDLightmap.AtlasMinV + vNorm * (SI->HDLightmap.AtlasMaxV - SI->HDLightmap.AtlasMinV);

					PolyVertexLightmapUVs(vi) = glm::vec2(uAtlas, vAtlas);
				} // end if hd lightmap
			} // end loop verts

			if (!Shader->VertBuffer.CanBuffer((NumPts - 2) * 3))
			{
				Shader->DrawBuffer.EndDrawCall(FacetVertexCount);
				Shader->ParametersBuffer.Advance(1); // advance so Flush automatically restores the drawcall params of the _current_ drawcall
				Shader->Flush(true);
				Shader->DrawBuffer.StartDrawCall();
				DrawID = Shader->DrawBuffer.GetDrawID();

				// just in case...
				if ((NumPts - 2) * 3 >= Shader->VertexBufferSize)
				{
					debugf(TEXT("DrawComplexSurface facet too big (facet has %d vertices - need to buffer %d points - Vertex Buffer Size is %d)!"), NumPts, (NumPts - 2) * 3, Shader->VertexBufferSize);
					continue;
				}

				FacetVertexCount = 0;
			}

			// write facet poly to buffer
			auto Out = Shader->VertBuffer.GetCurrentElementPtr();
			for (INT i = 0; i < PolyVertices.Num() - 2; i++)
			{
				// ---- Vertex 0 ----
				Out->Coords   = PolyVertices(0);
				Out->DrawID   = DrawID;

				Out->Normal   = PolyVertexNormals(0);
				Out->Tangent  = PolyVertexTangents(0);
				Out->Bitangent  = PolyVertexBitangents(0);

				Out->FacetID  = facetIDForVerts;
				Out->LightmapUV = PolyVertexLightmapUVs(0);
				Out++;

				// ---- Vertex 1 ----
				Out->Coords   = PolyVertices(i + 1);
				Out->DrawID   = DrawID;

				Out->Normal   = PolyVertexNormals(i + 1);
				Out->Tangent  = PolyVertexTangents(i + 1);
				Out->Bitangent  = PolyVertexBitangents(i + 1);

				Out->FacetID  = facetIDForVerts;
				Out->LightmapUV = PolyVertexLightmapUVs(i + 1);
				Out++;

				// ---- Vertex 2 ----
				Out->Coords   = PolyVertices(i + 2);
				Out->DrawID   = DrawID;

				Out->Normal   = PolyVertexNormals(i + 2);
				Out->Tangent  = PolyVertexTangents(i + 2);
				Out->Bitangent  = PolyVertexBitangents(i + 2);

				Out->FacetID  = facetIDForVerts;
				Out->LightmapUV = PolyVertexLightmapUVs(i + 2);
				Out++;
			}

			FacetVertexCount += (PolyVertices.Num() - 2) * 3;
			Shader->VertBuffer.Advance((PolyVertices.Num() - 2) * 3);
		} // end loop through polys
	} // end have an SI but is a mover or we are not doing phong
	else {
		glm::vec4 fallbackN = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
		glm::vec4 fallbackT = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
		glm::vec4 fallbackB = glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
		for (FSavedPoly* Poly = Facet.Polys; Poly; Poly = Poly->Next)
		{
			const INT NumPts = Poly->NumPts;
			if (NumPts < 3) //Skip invalid polygons,if any?
				continue;

			if (!Shader->VertBuffer.CanBuffer((NumPts - 2) * 3))
			{
				Shader->DrawBuffer.EndDrawCall(FacetVertexCount);
				Shader->ParametersBuffer.Advance(1); // advance so Flush automatically restores the drawcall params of the _current_ drawcall
				Shader->Flush(true);
				Shader->DrawBuffer.StartDrawCall();
				DrawID = Shader->DrawBuffer.GetDrawID();

				// just in case...
				if ((NumPts - 2) * 3 >= Shader->VertexBufferSize)
				{
					debugf(TEXT("DrawComplexSurface facet too big (facet has %d vertices - need to buffer %d points - Vertex Buffer Size is %d)!"), NumPts, (NumPts - 2) * 3, Shader->VertexBufferSize);
					continue;
				}

				FacetVertexCount = 0;
			}

			FTransform** In = &Poly->Pts[0];
			auto Out = Shader->VertBuffer.GetCurrentElementPtr();
			for (INT i = 0; i < NumPts - 2; i++)
			{
				// stijn: not using the normals currently, but we're keeping them in
				// because they make our vertex data aligned to a 48 byte boundary
				(Out)->Coords = FPlaneToVec4(In[0]->Point);
				(Out)->DrawID = DrawID;
				Out->Normal   = fallbackN;
				Out->Tangent  = fallbackT;
				Out->Bitangent  = fallbackB;
				(Out++)->FacetID = facetIDForVerts;
				(Out)->Coords = FPlaneToVec4(In[i + 1]->Point);
				(Out)->DrawID = DrawID;
				Out->Normal   = fallbackN;
				Out->Tangent  = fallbackT;
				Out->Bitangent  = fallbackB;
				(Out++)->FacetID = facetIDForVerts;
				(Out)->Coords = FPlaneToVec4(In[i + 2]->Point);
				(Out)->DrawID = DrawID;
				Out->Normal   = fallbackN;
				Out->Tangent  = fallbackT;
				Out->Bitangent  = fallbackB;
				(Out++)->FacetID = facetIDForVerts;
			}
			FacetVertexCount += (NumPts - 2) * 3;
			Shader->VertBuffer.Advance((NumPts - 2) * 3);
		} // end loop through polys
	} // end if just straight up old path
		
	Shader->DrawBuffer.EndDrawCall(FacetVertexCount);
	Shader->ParametersBuffer.Advance(1);

#if ENGINE_VERSION!=227
	if ((DrawFlags & ShaderDrawFlags::DF_BumpMap) &&
		Surface.Texture && Surface.Texture->Texture &&
		Surface.Texture->Texture->BumpMap)
	{
		Surface.Texture->Texture->BumpMap->Unlock(Shader->BumpMapInfo);
	}
#endif

    STAT(unclockFast(Stats.ComplexCycles));

	unguard;
}

#if ENGINE_VERSION==227
//Draw everything after one pass. This function is called after each internal rendering pass, everything has to be properly indexed before drawing. Used for DrawComplexSurface.
void UXOpenGLRenderDevice::DrawPass(FSceneNode* Frame, INT Pass) {}
#endif

/*-----------------------------------------------------------------------------
	Complex Surface Shader
-----------------------------------------------------------------------------*/

UXOpenGLRenderDevice::DrawComplexProgram::DrawComplexProgram(const TCHAR* Name, UXOpenGLRenderDevice* RenDev)
	: ShaderProgramImpl(Name, RenDev)
{
	VertexBufferSize				= DRAWCOMPLEX_SIZE * 12;
	ParametersBufferSize			= DRAWCOMPLEX_SIZE;
	ParametersBufferBindingIndex	= GlobalShaderBindingIndices::ComplexParametersIndex;
	NumTextureSamplers				= 13;
	DrawMode						= GL_TRIANGLES;
	UseSSBOParametersBuffer			= RenDev->UsingShaderDrawParameters;
	ParametersInfo					= DrawComplexParametersInfo;
	VertexShaderFunc				= &BuildVertexShader;
	GeoShaderFunc					= nullptr;
	FragmentShaderFunc				= &BuildFragmentShader;
	// Configure facet index/meta ring sizes (elements per sub-buffer)
	FacetIndexRingSize = 65536; // uint indices per sub-buffer (tune up/down)
	FacetMetaRingSize  = DRAWCOMPLEX_SIZE*2; // number of metadata entries per sub-buffer (one per drawID)
	RelevantSpecializationOptions =
		ShaderCompilationOptions::OPT_DetailTextures |
		ShaderCompilationOptions::OPT_MacroTextures |
		ShaderCompilationOptions::OPT_EnvironmentMaps |
		ShaderCompilationOptions::OPT_BumpMaps |
		ShaderCompilationOptions::OPT_HeightMaps |
		ShaderCompilationOptions::OPT_PhongShading |
		ShaderCompilationOptions::OPT_Multipass |
		ShaderCompilationOptions::OPT_HDLightMap |
		ShaderCompilationOptions::OPT_MSAA |
		ShaderCompilationOptions::OPT_HWLighting |
		ShaderCompilationOptions::OPT_DistanceFog |
		ShaderCompilationOptions::OPT_ClipDistance |
		ShaderCompilationOptions::OPT_Editor |
		ShaderCompilationOptions::OPT_SimulateMultiPass;
}

void UXOpenGLRenderDevice::DrawComplexProgram::CreateInputLayout()
{
	for (INT i = 0; i < 7; ++i)
		glEnableVertexAttribArray(i);
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(DrawComplexVertex), (GLvoid*)offsetof(DrawComplexVertex, Coords));
	glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT, sizeof(DrawComplexVertex), (GLvoid*)offsetof(DrawComplexVertex, DrawID));
	// Normal - vec3 in shader (we store vec4, shader reads xyz)
	glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(DrawComplexVertex), (GLvoid*)offsetof(DrawComplexVertex, Normal));
	// Tangent
	glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(DrawComplexVertex), (GLvoid*)offsetof(DrawComplexVertex, Tangent));
	// Bitangent
	glVertexAttribPointer(4, 3, GL_FLOAT, GL_FALSE, sizeof(DrawComplexVertex), (GLvoid*)offsetof(DrawComplexVertex, Bitangent));
	glVertexAttribIPointer(5, 1, GL_UNSIGNED_INT, sizeof(DrawComplexVertex), (GLvoid*)offsetof(DrawComplexVertex, FacetID));
	glVertexAttribPointer(6, 2, GL_FLOAT, GL_FALSE, sizeof(DrawComplexVertex), (GLvoid*)offsetof(DrawComplexVertex, LightmapUV));
	VertBuffer.SetInputLayoutCreated();
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
