/*=============================================================================
	DrawGouraud.cpp: Unreal XOpenGL DrawGouraud routines.
	Used for drawing meshes.

	VertLists are only supported by 227 so far, it pushes verts in a huge
	list instead of vertice by vertice. Currently this method improves
	performance 10x and more compared to unbuffered calls. Buffering
	catches up quite some.
	Copyright 2014-2021 Oldunreal

	Todo:
        * On a long run this should be replaced with a more mode
          modern mesh rendering method, but this requires also quite some
          rework in Render.dll and will be not compatible with other
          UEngine1 games.

	Revision history:
		* Created by Smirftsch
		* Added buffering to DrawGouraudPolygon
		* implemented proper usage of persistent buffers.
		* Added bindless texture support.

=============================================================================*/

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include "XOpenGLDrv.h"
#include "XOpenGL.h"

/*-----------------------------------------------------------------------------
	Helpers
-----------------------------------------------------------------------------*/

static void BufferVert(UXOpenGLRenderDevice::DrawGouraudVertex* Vert, FTransTexture* P, glm::uint DrawID)
{
	Vert->Coords		= glm::vec3(P->Point.X, P->Point.Y, P->Point.Z);
	Vert->DrawID		= DrawID;
	Vert->Normals		= glm::vec4(P->Normal.X, P->Normal.Y, P->Normal.Z, 0.f);
	Vert->TexCoords     = glm::vec2(P->U, P->V);
	Vert->LightColor	= glm::vec4(P->Light.X, P->Light.Y, P->Light.Z, P->Light.W);
	Vert->FogColor		= glm::vec4(P->Fog.X, P->Fog.Y, P->Fog.Z, P->Fog.W);
}

static void SetTextureHelper
(
	UXOpenGLRenderDevice* RenDev, 
	INT Multi, 
	UTexture* Texture, 
	FSceneNode* Frame, 
	FTEXTURE_PTR& CachedInfo,
	glm::uint64* TexHandles,
	DWORD& DrawFlags,
	DWORD AddDrawFlag
)
{
#if XOPENGL_MODIFIED_LOCK
	CachedInfo = Texture->GetTexture(INDEX_NONE, RenDev);
#else
	Texture->Lock(CachedInfo, Frame->Viewport->CurrentTime, -1, RenDev);
#endif

	RenDev->SetTexture(Multi, FTEXTURE_GET(CachedInfo), Texture->PolyFlags, 0.f);
	TexHandles[Multi] = RenDev->TexInfo[Multi].BindlessTexHandle;
	DrawFlags |= AddDrawFlag;
}

DWORD UXOpenGLRenderDevice::PrepareGouraudCall(FSceneNode* Frame, FTextureInfo& Info, DWORD PolyFlags)
{
	auto Shader = dynamic_cast<DrawGouraudProgram*>(Shaders[Gouraud_Prog]);

	// Gather options
	DWORD DrawFlags = ShaderDrawFlags::DF_None;
	DWORD NextPolyFlags = GetPolyFlagsAndDrawFlags(PolyFlags, DrawFlags, FALSE);
	UBOOL NoNearZ = (GUglyHackFlags & HACKFLAGS_NoNearZ) == HACKFLAGS_NoNearZ;
	if (GIsEditor && NextPolyFlags & PF_Selected)
		DrawFlags |= ShaderDrawFlags::DF_Selected;

	const bool CanBuffer = !Shader->DrawBuffer.IsFull() && Shader->ParametersBuffer.CanBuffer(1);

	// Check if the global state will change
	if (WillBlendStateChange(CurrentBlendPolyFlags, NextPolyFlags) || // Check if the blending mode will change
		WillTextureStateChange(0, Info, NextPolyFlags) || // Check if the texture will change
		StoredbNearZ != NoNearZ ||  // Force a flush if we're switching between NearZ and NoNearZ
		!CanBuffer // Check if we have room left in the multi-draw array
	)
	{
		// Dispatch buffered data
		Shader->Flush(!CanBuffer);

		SetBlend(NextPolyFlags);

		if (NoNearZ &&
			(StoredFovAngle != Frame->Viewport->Actor->FovAngle ||
				StoredFX != Frame->FX ||
				StoredFY != Frame->FY ||
				!StoredbNearZ))
		{
			SetProjection(Frame, 1); // TODO/FIXME: Shouldn't this second argument be !NoNearZ ?
		}
	}	
	
	DrawGouraudParameters* DrawCallParams = Shader->ParametersBuffer.GetCurrentElementPtr();

	const FLOAT TextureAlpha =
#if ENGINE_VERSION==227
		1.f;
#else
		(Info.Texture && Info.Texture->Alpha > 0.f) ? Info.Texture->Alpha : 1.f;
#endif
	
	DrawCallParams->DrawColor = HitTesting() ? FPlaneToVec4(HitColor) : glm::vec4(0.f, 0.f, 0.f, TextureAlpha);

	SetTexture(DiffuseTextureIndex, Info, NextPolyFlags, 0.0);
	DrawCallParams->DiffuseInfo = glm::vec4(TexInfo[DiffuseTextureIndex].UMult, TexInfo[DiffuseTextureIndex].VMult, Info.Texture ? Info.Texture->Diffuse : 1.f, TextureAlpha);
	DrawCallParams->TexHandles[DiffuseTextureIndex] = TexInfo[DiffuseTextureIndex].BindlessTexHandle;
	DrawFlags |= ShaderDrawFlags::DF_DiffuseTexture;

	DrawCallParams->DetailMacroInfo = glm::vec4(0.f, 0.f, 0.f, 0.f);
	if (Info.Texture && Info.Texture->DetailTexture && DetailTextures)
	{
		SetTextureHelper(this, DetailTextureIndex, Info.Texture->DetailTexture, Frame, Shader->DetailTextureInfo, DrawCallParams->TexHandles, DrawFlags, ShaderDrawFlags::DF_DetailTexture);
		DrawCallParams->DetailMacroInfo.x = TexInfo[DetailTextureIndex].UMult;
		DrawCallParams->DetailMacroInfo.y = TexInfo[DetailTextureIndex].VMult;
	}

	DrawCallParams->MiscInfo = glm::vec4(0.f, 0.f, 0.f, 0.f);
#if ENGINE_VERSION==227
	if (Info.Texture && Info.Texture->BumpMap && BumpMaps)
	{
		SetTextureHelper(this, BumpMapIndex, Info.Texture->BumpMap, Frame, Shader->BumpMapInfo, DrawCallParams->TexHandles, DrawFlags, ShaderDrawFlags::DF_BumpMap);
		DrawCallParams->MiscInfo.x = Info.Texture->BumpMap->Specular;
	}
#endif

	if (Info.Texture && Info.Texture->MacroTexture && MacroTextures)
	{
		SetTextureHelper(this, MacroTextureIndex, Info.Texture->MacroTexture, Frame, Shader->MacroTextureInfo, DrawCallParams->TexHandles, DrawFlags, ShaderDrawFlags::DF_MacroTexture);
		DrawCallParams->DetailMacroInfo.z = TexInfo[MacroTextureIndex].UMult;
		DrawCallParams->DetailMacroInfo.w = TexInfo[MacroTextureIndex].VMult;
	}

	bool safeToReadDepth = !(PolyFlags & PF_Occlude);
	if (safeToReadDepth && IsDepthFadeFX(Info.Texture))
	{
		// Fix shader-side behavior
		DrawFlags |= ShaderDrawFlags::DF_ReadDepth;
		DrawCallParams->SceneWidth = SceneWidth;
		DrawCallParams->SceneHeight = SceneHeight;
		PrepareDepthTexture();
		INT depthIndex = SceneDepthIndex;
		DrawCallParams->TexHandles[depthIndex] = SceneFbo->depthBindlessHandle;
		//Z -= 50 * min(Z / 300, 1);
	}

	if (PolyFlags & PF_SpecialPoly)
	{
		DrawFlags |= ShaderDrawFlags::DF_Weapon;
	}

	DrawCallParams->DrawFlags = DrawFlags;
	return DrawFlags;
}

void UXOpenGLRenderDevice::FinishGouraudCall(FTextureInfo& Info, DWORD DrawFlags)
{
#if !XOPENGL_MODIFIED_LOCK
	auto Shader = dynamic_cast<DrawGouraudProgram*>(Shaders[Gouraud_Prog]);
	if (DrawFlags & ShaderDrawFlags::DF_DetailTexture)
		Info.Texture->DetailTexture->Unlock(Shader->DetailTextureInfo);

	if (DrawFlags & ShaderDrawFlags::DF_BumpMap)
		Info.Texture->BumpMap->Unlock(Shader->BumpMapInfo);

	if (DrawFlags & ShaderDrawFlags::DF_MacroTexture)
		Info.Texture->MacroTexture->Unlock(Shader->MacroTextureInfo);
#endif
}

// -----------------------------------------------------------------------------
// Simplified UV-Linear Scaler: Reconstructs the absolute full-scale size of the mark 
// by dividing the maximum single-axis distance by its linear UV texture fraction.
// compares the world size to a threshold, above which we assume it is a shadow not a plasma mark
// -----------------------------------------------------------------------------
static FName GLastActiveTextureName = NAME_None;
static QWORD GShadowCacheIDLock     = 0;
static FName NAME_EnergyMark(TEXT("energymark"));
inline UBOOL IsBlobShadow(const FTextureInfo& Info, FTransTexture* const* Pts)
{
	// STATE TRACKING LAYER:
    // If the engine provided a valid texture pointer, update our primary name tracker.
    if (Info.Texture)
    {
        GLastActiveTextureName = Info.Texture->GetFName();
        
        // If this is explicitly the footprint shadow texture, lock its active CacheID!
        if (GLastActiveTextureName == NAME_EnergyMark)
        {
            GShadowCacheIDLock = Info.CacheID;
        }
    }

    // STATE MACHINE GATEWAY:
    // We determine if this call is our target shadow mesh using two strict conditions:
    // A: The texture pointer is valid and named "energymark".
    // B: The texture pointer is missing, but the incoming CacheID matches our locked shadow CacheID!
    UBOOL bIsShadowMeshCall = FALSE;
    
    if (Info.Texture && GLastActiveTextureName == NAME_EnergyMark)
    {
        bIsShadowMeshCall = TRUE;
    }
    else if (!Info.Texture && Info.CacheID == GShadowCacheIDLock && GShadowCacheIDLock != 0)
    {
        bIsShadowMeshCall = TRUE;
    }

	// If it fails both checks, it is undeniably a different texture/skin sequence. Exit instantly!
    if (!bIsShadowMeshCall)
    {
        return FALSE;
    }

    // Fetch the first three vertices cleanly out of the scattered pointer structure
    const FTransTexture& V0 = *Pts[0];
    const FTransTexture& V1 = *Pts[1];
    const FTransTexture& V2 = *Pts[2];

    // Gather the absolute minimum and maximum coordinates across the 3 target vertices
    FLOAT MinX = min(V0.Point.X, min(V1.Point.X, V2.Point.X));
    FLOAT MaxX = max(V0.Point.X, max(V1.Point.X, V2.Point.X));
    FLOAT MinY = min(V0.Point.Y, min(V1.Point.Y, V2.Point.Y));
    FLOAT MaxY = max(V0.Point.Y, max(V1.Point.Y, V2.Point.Y));
    FLOAT MinZ = min(V0.Point.Z, min(V1.Point.Z, V2.Point.Z));
    FLOAT MaxZ = max(V0.Point.Z, max(V1.Point.Z, V2.Point.Z));

    FLOAT MinU = min(V0.U, min(V1.U, V2.U));
    FLOAT MaxU = max(V0.U, max(V1.U, V2.U));
    FLOAT MinV = min(V0.V, min(V1.V, V2.V));
    FLOAT MaxV = max(V0.V, max(V1.V, V2.V));

    // Compute the straight-line physical widths and texture-pixel deltas
    FLOAT DeltaX = MaxX - MinX;
    FLOAT DeltaY = MaxY - MinY;
    FLOAT DeltaZ = MaxZ - MinZ;
    FLOAT DeltaU = MaxU - MinU;
    FLOAT DeltaV = MaxV - MinV;

    // Capture the largest physical distance span found on any of the 3 space channels
    FLOAT MaxPhysicalSpan = max(DeltaX, max(DeltaY, DeltaZ));

    // Convert the texture-pixel delta spans into an absolute 0.0 to 1.0 linear fraction
    FLOAT MaxUVFraction = max(DeltaU / (FLOAT)Info.USize, DeltaV / (FLOAT)Info.VSize);

    // Security boundary clamp to protect against tiny rounding noise producing infinite divisions
    if (MaxUVFraction < 0.001f)
        MaxUVFraction = 0.001f;

    // --- STRUCTURAL EXTRAPOLATION ---
    // Divide the physical span straight by the linear fraction to find out how 
    // large the entire mark is in world units if it were uncut!
    FLOAT FullUncutWorldSize = MaxPhysicalSpan / MaxUVFraction;

    // THE ABSOLUTE WORLD SIZE THRESHOLD FILTER:
    // Enforce your verified 35.0f baseline limit
    FLOAT FullAssetWorldThreshold = 35.0f;

    if (FullUncutWorldSize > FullAssetWorldThreshold)
    {
        return TRUE; // Confirmed as part of a massive footprint blob shadow structure!
    }

    return FALSE; // Small weapon impact effect decal
}

/*-----------------------------------------------------------------------------
	RenDev Interface
-----------------------------------------------------------------------------*/

void UXOpenGLRenderDevice::DrawGouraudPolygon(FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, INT NumPts, DWORD PolyFlags, FSpanBuffer* Span)
{
	guard(UXOpenGLRenderDevice::DrawGouraudPolygon);

	if (NoDrawGouraud)
		return;

	if (ShadowMaps && IsBlobShadow(Info, Pts))
	{
		return; // Drop individual blob shadow instance immediately!
	}

	auto Shader = dynamic_cast<DrawGouraudProgram*>(Shaders[Gouraud_Prog]);

    STAT(clockFast(Stats.GouraudPolyCycles));
	SetProgram(Gouraud_Prog);

	if (NumPts < 3 /*|| Frame->Recursion > MAX_FRAME_RECURSION*/) //reject invalid.
		return;

	auto InVertexCount = NumPts - 2;
	auto OutVertexCount = InVertexCount * 3;

#if ENGINE_VERSION==227
	if (Info.Modifier)
	{
		FLOAT UM = Info.USize, VM = Info.VSize;
		for (INT i = 0; i < NumPts; ++i)
			Info.Modifier->TransformPointUV(Pts[i]->U, Pts[i]->V, UM, VM);
	}
#endif

	if (!Shader->VertBuffer.CanBuffer(OutVertexCount)) // we check the available capacity of the parameters and draw buffer elsewhere
	{
		Shader->Flush(true);

		// just in case...
		if (OutVertexCount >= Shader->VertexBufferSize)
		{
			GWarn->Logf(TEXT("DrawGouraudPolygon poly too big!"));
			return;
		}
	}

	DWORD DrawFlags = PrepareGouraudCall(Frame, Info, PolyFlags);

	Shader->DrawBuffer.StartDrawCall();
	auto Out = Shader->VertBuffer.GetCurrentElementPtr();
	const auto DrawID = Shader->DrawBuffer.GetDrawID();

	if (DrawFlags & ShaderDrawFlags::DF_ReadDepth)
	{
		for (INT i = 0; i < InVertexCount; i++)
		{
			FLOAT Z = Pts[i]->Point.Z;
			int offsetZ = 50 * min(Z / 300.f, 1);
			Pts[i]->Point.Z -= offsetZ;
		}
	}

	// Unfan and buffer
	for (INT i = 0; i < InVertexCount; i++)
	{
		BufferVert(Out++, Pts[0    ], DrawID);
		BufferVert(Out++, Pts[i + 1], DrawID);
		BufferVert(Out++, Pts[i + 2], DrawID);
	}

	Shader->DrawBuffer.EndDrawCall(OutVertexCount);
	Shader->VertBuffer.Advance(OutVertexCount);
	Shader->ParametersBuffer.Advance(1);

	FinishGouraudCall(Info, DrawFlags);
    STAT(unclockFast(Stats.GouraudPolyCycles));
	unguard;
}

#if ENGINE_VERSION==227 || UNREAL_TOURNAMENT_OLDUNREAL
void UXOpenGLRenderDevice::DrawGouraudPolyList(FSceneNode* Frame, FTextureInfo& Info, FTransTexture* Pts, INT NumPts, DWORD PolyFlags, FSpanBuffer* Span)
{
	guard(UXOpenGLRenderDevice::DrawGouraudPolyList);

	if (NoDrawGouraudList)
		return;

	auto Shader = dynamic_cast<DrawGouraudProgram*>(Shaders[Gouraud_Prog]);

    STAT(clockFast(Stats.GouraudPolyCycles));
	SetProgram(Gouraud_Prog);

	if (NumPts < 3 /*|| Frame->Recursion > MAX_FRAME_RECURSION*/) //reject invalid.
		return;

#if ENGINE_VERSION==227
	if (Info.Modifier)
	{
		FLOAT UM = Info.USize, VM = Info.VSize;
		for (INT i = 0; i < NumPts; ++i)
			Info.Modifier->TransformPointUV(Pts[i].U, Pts[i].V, UM, VM);
	}
#endif

	DWORD DrawFlags = PrepareGouraudCall(Frame, Info, PolyFlags);

	Shader->DrawBuffer.StartDrawCall();
	auto Out = Shader->VertBuffer.GetCurrentElementPtr();
	auto End = Shader->VertBuffer.GetLastElementPtr();
	auto DrawID = Shader->DrawBuffer.GetDrawID();

	INT PolyListSize = 0;

	if (DrawFlags & ShaderDrawFlags::DF_ReadDepth)
	{
		for (INT i = 0; i < NumPts; i++)
		{
			FLOAT Z = (&Pts[i])->Point.Z;
			int offsetZ = 10 * min(Z / 60.f, 1);
			(&Pts[i])->Point.Z -= offsetZ;
		}
	}

	for (INT i = 0; i < NumPts; i++)
	{
		// Polylists can be bigger than the vertex buffer so check here if we
		// need to split the mesh up into separate drawcalls
		if ((i % 3 == 0) && (Out + 2 > End))
		{
			Shader->DrawBuffer.EndDrawCall(PolyListSize);
			Shader->VertBuffer.Advance(PolyListSize);
			Shader->ParametersBuffer.Advance(1); // advance so Flush automatically restores the drawcall params of the _current_ drawcall

			Shader->Flush(true);
			//debugf(NAME_DevGraphics, TEXT("DrawGouraudPolyList overflow!"));

			Shader->DrawBuffer.StartDrawCall();
			Out = Shader->VertBuffer.GetCurrentElementPtr();
			End = Shader->VertBuffer.GetLastElementPtr();
			DrawID = Shader->DrawBuffer.GetDrawID();

			PolyListSize = 0;
		}

		BufferVert(Out++, &Pts[i], DrawID);
		PolyListSize++;
	}

	Shader->DrawBuffer.EndDrawCall(PolyListSize);
	Shader->VertBuffer.Advance(PolyListSize);
	Shader->ParametersBuffer.Advance(1);

	FinishGouraudCall(Info, DrawFlags);
    STAT(unclockFast(Stats.GouraudPolyCycles));
	unguard;
}
#endif

// stijn: This is the UT extended renderer interface. This does not map directly onto DrawGouraudPolyList because DrawGouraudTriangles pushes info out earlier
#if UNREAL_TOURNAMENT_OLDUNREAL
void UXOpenGLRenderDevice::DrawGouraudTriangles(const FSceneNode* Frame, const FTextureInfo& Info, FTransTexture* const Pts, INT NumPts, DWORD PolyFlags, DWORD DataFlags, FSpanBuffer* Span)
{
	guard(UXOpenGLRenderDevice::DrawGouraudTriangles);

	if (NoDrawGouraudList)
		return;

    STAT(clockFast(Stats.GouraudPolyCycles));

	INT StartOffset = 0;
	INT i = 0;

	if (Frame->NearClip.W != 0.0)
		PushClipPlane(Frame->NearClip);

	for (; i < NumPts; i += 3)
	{
		if (Frame->Mirror == -1.0)
			Exchange(Pts[i + 2], Pts[i]);

		// Environment mapping.
		if (PolyFlags & PF_Environment)
		{
			FLOAT UScale = Info.UScale * Info.USize / 256.0f;
			FLOAT VScale = Info.VScale * Info.VSize / 256.0f;

			for (INT j = 0; j < 3; j++)
			{
				FVector T = Pts[i + j].Point.UnsafeNormal().MirrorByVector(Pts[i + j].Normal).TransformVectorBy(Frame->Uncoords);
				Pts[i + j].U = (T.X + 1.0f) * 0.5f * 256.0f * UScale;
				Pts[i + j].V = (T.Y + 1.0f) * 0.5f * 256.0f * VScale;
			}
		}

		bool isWeapon = (Pts[0].Point.Z < 10);
		if (ScreenSpaceReflections && isWeapon)
		{
			PolyFlags |= PF_SpecialPoly;
		}

		// If outcoded, skip it.
		if (Pts[i].Flags & Pts[i + 1].Flags & Pts[i + 2].Flags)
		{
			// stijn: push the triangles we've already processed (if any)
			if (i - StartOffset > 0)
			{
				DrawGouraudPolyList(const_cast<FSceneNode*>(Frame), const_cast<FTextureInfo&>(Info), Pts + StartOffset, i - StartOffset, PolyFlags, nullptr);
				StartOffset = i + 3;
			}
			continue;
		}

		// Backface reject it.
		if ((PolyFlags & PF_TwoSided) && FTriple(Pts[i].Point, Pts[i + 1].Point, Pts[i + 2].Point) <= 0.0)
		{
			if (!(PolyFlags & PF_TwoSided))
			{
				// stijn: push the triangles we've already processed (if any)
				if (i - StartOffset > 0)
				{
					DrawGouraudPolyList(const_cast<FSceneNode*>(Frame), const_cast<FTextureInfo&>(Info), Pts + StartOffset, i - StartOffset, PolyFlags, nullptr);
					StartOffset = i + 3;
				}
				continue;
			}
			Exchange(Pts[i + 2], Pts[i]);
		}
	}

	// stijn: push the remaining triangles
	if (i - StartOffset > 0)
		DrawGouraudPolyList(const_cast<FSceneNode*>(Frame), const_cast<FTextureInfo&>(Info), Pts + StartOffset, i - StartOffset, PolyFlags, nullptr);

	if (Frame->NearClip.W != 0.0)
		PopClipPlane();

    STAT(unclockFast(Stats.GouraudPolyCycles));
	unguard;
}
#endif

#if ENGINE_VERSION==227
void UXOpenGLRenderDevice::PreDrawGouraud(FSceneNode* Frame, FFogSurf& FogSurf)
{
	guard(UOpenGLRenderDevice::PreDrawGouraud);

	if (FogSurf.IsValid())
		SetDistanceFog(FogSurf);
	else
		ResetDistanceFog();

	unguard;
}

void UXOpenGLRenderDevice::PostDrawGouraud(FSceneNode* Frame, FFogSurf& FogSurf)
{
	guard(UOpenGLRenderDevice::PostDrawGouraud);
	ResetDistanceFog();
	unguard;
}
#endif // ENGINE_VERSION

/*-----------------------------------------------------------------------------
	Gouraud Mesh Shader
-----------------------------------------------------------------------------*/

UXOpenGLRenderDevice::DrawGouraudProgram::DrawGouraudProgram(const TCHAR* Name, UXOpenGLRenderDevice* RenDev)
	: ShaderProgramImpl(Name, RenDev)
{
	VertexBufferSize				= DRAWGOURAUDPOLY_SIZE * 12;
	ParametersBufferSize			= DRAWGOURAUDPOLY_SIZE;
	ParametersBufferBindingIndex	= GlobalShaderBindingIndices::GouraudParametersIndex;
	NumTextureSamplers				= 10;
	DrawMode						= GL_TRIANGLES;
	UseSSBOParametersBuffer			= RenDev->UsingShaderDrawParameters;
	ParametersInfo					= DrawGouraudParametersInfo;
	VertexShaderFunc				= &BuildVertexShader;
	GeoShaderFunc					= RenDev->UsingGeometryShaders ? &BuildGeometryShader : nullptr; // optional
	FragmentShaderFunc				= &BuildFragmentShader;
	RelevantSpecializationOptions =
		ShaderCompilationOptions::OPT_DetailTextures |
		ShaderCompilationOptions::OPT_MacroTextures |
		ShaderCompilationOptions::OPT_BumpMaps |
		ShaderCompilationOptions::OPT_HWLighting |
		ShaderCompilationOptions::OPT_DistanceFog |
		ShaderCompilationOptions::OPT_ClipDistance |
		ShaderCompilationOptions::OPT_Editor |
		ShaderCompilationOptions::OPT_ScreenSpaceReflections |
		ShaderCompilationOptions::OPT_GeometryShaders |
		ShaderCompilationOptions::OPT_MSAA;
}

void UXOpenGLRenderDevice::DrawGouraudProgram::CreateInputLayout()
{
	for (INT i = 0; i < 6; ++i)
		glEnableVertexAttribArray(i);
	using Vert = DrawGouraudVertex;
	glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vert), (GLvoid*)(0));
	glVertexAttribIPointer(1, 1, GL_UNSIGNED_INT,   sizeof(Vert), (GLvoid*)(offsetof(Vert, DrawID)));
	glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vert), (GLvoid*)(offsetof(Vert, Normals)));
	glVertexAttribPointer(3, 2, GL_FLOAT, GL_FALSE, sizeof(Vert), (GLvoid*)(offsetof(Vert, TexCoords)));
	glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(Vert), (GLvoid*)(offsetof(Vert, LightColor)));
	glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(Vert), (GLvoid*)(offsetof(Vert, FogColor)));
	VertBuffer.SetInputLayoutCreated();
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
