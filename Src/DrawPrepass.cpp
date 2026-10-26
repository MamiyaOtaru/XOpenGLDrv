/*=============================================================================
	DrawPrepass.cpp: Unreal XOpenGL DrawDepthOnly prepass routines.
	Used for BSP depth only pass.

	Revision history:
		* Created by MamiyaOtaru
=============================================================================*/

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <system_error>
#include "XOpenGLDrv.h"
#include "XOpenGL.h"

static bool bMultithreadSurfaceViewCache = true;

void UXOpenGLRenderDevice::EnsureViewCacheWorkers()
{
    if (bViewCacheWorkersInitialized)
        return;

    bViewCacheWorkersInitialized = true;
    bViewCacheWorkersStopping = false;
    ViewCacheReadyWorkers = 0;

    const unsigned int HardwareThreads = std::thread::hardware_concurrency();
    const INT WorkerCount = Min<INT>(4, HardwareThreads > 0 ? static_cast<INT>(HardwareThreads) : 1);
    ViewCacheWorkers.reserve(WorkerCount);
    try
    {
        for (INT WorkerIndex = 0; WorkerIndex < WorkerCount; ++WorkerIndex)
            ViewCacheWorkers.emplace_back(&UXOpenGLRenderDevice::ViewCacheWorker, this, WorkerIndex);
    }
    catch (const std::system_error& Error)
    {
        debugf(TEXT("Surface view-cache worker creation failed (%d); using %d persistent workers"),
            Error.code().value(), static_cast<INT>(ViewCacheWorkers.size()));
    }

    std::unique_lock<std::mutex> Lock(ViewCacheMutex);
    ViewCacheWorkComplete.wait(Lock, [this]()
    {
        return ViewCacheReadyWorkers == static_cast<INT>(ViewCacheWorkers.size());
    });
}

void UXOpenGLRenderDevice::StopViewCacheWorkers()
{
    if (!bViewCacheWorkersInitialized)
        return;

    {
        std::lock_guard<std::mutex> Lock(ViewCacheMutex);
        bViewCacheWorkersStopping = true;
    }
    ViewCacheWorkAvailable.notify_all();
    for (std::thread& Worker : ViewCacheWorkers)
        if (Worker.joinable())
            Worker.join();
    ViewCacheWorkers.clear();
    bViewCacheWorkersInitialized = false;
    bViewCacheWorkersStopping = false;
    ViewCacheSurfaces = nullptr;
    ViewCacheFrame = nullptr;
}

void UXOpenGLRenderDevice::TransformSurfaceViewCache(const FSceneNode* Frame, FSurfInfo& SI)
{
    for (INT vi = 0; vi < SI.Verts.Num(); ++vi)
    {
        const FVector Vert = SI.Verts(vi).TransformPointBy(Frame->Coords);
        const FVector Normal = SI.VertexNormals(vi).TransformVectorBy(Frame->Coords).SafeNormal();
        const FVector Tangent = SI.Tangents(vi).TransformVectorBy(Frame->Coords).SafeNormal();
        const FVector Bitangent = SI.Bitangents(vi).TransformVectorBy(Frame->Coords).SafeNormal();
        SI.ViewSpaceVerts(vi) = glm::vec3(Vert.X, Vert.Y, Vert.Z);
        SI.ViewSpaceNormals(vi) = glm::vec4(Normal.X, Normal.Y, Normal.Z, 0.0f);
        SI.ViewSpaceTangents(vi) = glm::vec4(Tangent.X, Tangent.Y, Tangent.Z, 0.0f);
        SI.ViewSpaceBitangents(vi) = glm::vec4(Bitangent.X, Bitangent.Y, Bitangent.Z, 0.0f);
    }
    SI.ViewSpaceCacheStamp = ViewSpaceCacheGeneration;
}

void UXOpenGLRenderDevice::ViewCacheWorker(INT WorkerIndex)
{
    UINT ObservedGeneration;
    {
        std::lock_guard<std::mutex> Lock(ViewCacheMutex);
        ObservedGeneration = ViewCacheWorkGeneration;
        ++ViewCacheReadyWorkers;
    }
    ViewCacheWorkComplete.notify_one();
    for (;;)
    {
        std::unique_lock<std::mutex> Lock(ViewCacheMutex);
        ViewCacheWorkAvailable.wait(Lock, [this, &ObservedGeneration]()
        {
            return bViewCacheWorkersStopping || ViewCacheWorkGeneration != ObservedGeneration;
        });

        if (bViewCacheWorkersStopping)
            return;

        ObservedGeneration = ViewCacheWorkGeneration;
        const INT ActiveWorkers = ViewCacheActiveWorkers;
        const INT BatchSize = ViewCacheBatchSize;
        const INT NumSurfaces = ViewCacheNumSurfaces;
        FSurfInfo** Surfaces = ViewCacheSurfaces;
        const FSceneNode* Frame = ViewCacheFrame;
        Lock.unlock();

        if (WorkerIndex < ActiveWorkers)
        {
            const INT Begin = WorkerIndex * BatchSize;
            const INT End = Min<INT>(Begin + BatchSize, NumSurfaces);
            for (INT si = Begin; si < End; ++si)
                TransformSurfaceViewCache(Frame, *Surfaces[si]);
        }

        Lock.lock();
        ++ViewCacheCompletedWorkers;
        if (ViewCacheCompletedWorkers == static_cast<INT>(ViewCacheWorkers.size()))
            ViewCacheWorkComplete.notify_one();
    }
}

void UXOpenGLRenderDevice::PrepareSurfaceViewCache(const FSceneNode* Frame, INT TargetFrame)
{
    guard(UXOpenGLRenderDevice::PrepareSurfaceViewCache);

    ++ViewSpaceCacheGeneration;
    std::vector<FSurfInfo*> Surfaces;
    for (TMap<INT, FSurfInfo>::TIterator It(SurfaceInfoMap); It; ++It)
    {
        FSurfInfo& SI = It.Value();
        if (SI.LastDrawnFrame != TargetFrame || SI.IsMover)
            continue;

        const INT NumPts = SI.Verts.Num();
        if (SI.ViewSpaceVerts.Num() != NumPts)
        {
            SI.ViewSpaceVerts.Empty();
            SI.ViewSpaceVerts.AddZeroed(NumPts);
        }
        if (SI.ViewSpaceNormals.Num() != NumPts)
        {
            SI.ViewSpaceNormals.Empty();
            SI.ViewSpaceNormals.AddZeroed(NumPts);
        }
        if (SI.ViewSpaceTangents.Num() != NumPts)
        {
            SI.ViewSpaceTangents.Empty();
            SI.ViewSpaceTangents.AddZeroed(NumPts);
        }
        if (SI.ViewSpaceBitangents.Num() != NumPts)
        {
            SI.ViewSpaceBitangents.Empty();
            SI.ViewSpaceBitangents.AddZeroed(NumPts);
        }
        Surfaces.push_back(&SI);
    }

    if (Surfaces.empty())
        return;

    const INT NumSurfaces = static_cast<INT>(Surfaces.size());
    if (!bMultithreadSurfaceViewCache || NumSurfaces < 8)
    {
        for (INT si = 0; si < NumSurfaces; ++si)
            TransformSurfaceViewCache(Frame, *Surfaces[si]);
        return;
    }

    EnsureViewCacheWorkers();
    if (ViewCacheWorkers.empty())
    {
        for (INT si = 0; si < NumSurfaces; ++si)
            TransformSurfaceViewCache(Frame, *Surfaces[si]);
        return;
    }

    const INT ActiveWorkers = Min<INT>(NumSurfaces, ViewCacheWorkers.size());
    {
        std::lock_guard<std::mutex> Lock(ViewCacheMutex);
        ViewCacheActiveWorkers = ActiveWorkers;
        ViewCacheBatchSize = (NumSurfaces + ActiveWorkers - 1) / ActiveWorkers;
        ViewCacheNumSurfaces = NumSurfaces;
        ViewCacheSurfaces = Surfaces.data();
        ViewCacheFrame = Frame;
        ViewCacheCompletedWorkers = 0;
        ++ViewCacheWorkGeneration;
    }
    ViewCacheWorkAvailable.notify_all();
    {
        std::unique_lock<std::mutex> Lock(ViewCacheMutex);
        ViewCacheWorkComplete.wait(Lock, [this]()
        {
            return ViewCacheCompletedWorkers == static_cast<INT>(ViewCacheWorkers.size());
        });
        ViewCacheSurfaces = nullptr;
        ViewCacheFrame = nullptr;
    }

    unguard;
}

glm::vec3 ToVec3(const FPlane& P)
{
    return glm::vec3(P.X, P.Y, P.Z);
}

void UXOpenGLRenderDevice::DrawPrepassSurface(
    const FSceneNode* Frame,
    //const FSurfaceInfo& Surface,
    FSurfInfo& SI)
{
    guard(UXOpenGLRenderDevice::DrawDepthOnlySurface);

    // Depth/normal-only shader is already bound, state already set:
    // - DepthMask(true)
    // - DepthFunc(GL_LESS)
    // - No blending

    auto Shader = dynamic_cast<DrawPrepassProgram*>(Shaders[Prepass_Prog]);
    check(Shader);

    SetProgram(Prepass_Prog);

    // Start a new draw call in the depth-only vertex buffer
    Shader->DrawBuffer.StartDrawCall();
    //auto DrawID = Shader->DrawBuffer.GetDrawID();

    INT FacetVertexCount = 0;

	INT NumPts = SI.Verts.Num();

    TArray<glm::vec3> PolyVertices;
    PolyVertices.AddZeroed(NumPts);
    TArray<glm::vec3> PolyVertexNormals;
    PolyVertexNormals.AddZeroed(NumPts);

    TArray<glm::uint>& TriIdx = SI.TriIdx;

    // Build per-vertex data for this node polygon
    if (SI.IsMover)
    {
        AMover* Mov = Cast<AMover>(SI.Owner);
        if (!Mov) return;

        CachedMoverGeometry* CachedMesh = MoverWorldspaceCache.Find(SI.iSurf);

        if (!CachedMesh)
        {
            // First time: allocate once
            CachedMoverGeometry NewCache;
            NewCache.Verts.Empty();
            //NewCache.Verts.Reserve(SI.Verts.Num());
            NewCache.Verts.AddZeroed(SI.Verts.Num());
            ExtractMoverVertices(SI, NewCache.Verts);
            NewCache.LastPos = Mov->Location;
            NewCache.LastRot = Mov->Rotation;
            MoverWorldspaceCache.Set(SI.iSurf, NewCache);
            CachedMesh = MoverWorldspaceCache.Find(SI.iSurf);
        }
        else if (CachedMesh->LastPos != Mov->Location || CachedMesh->LastRot != Mov->Rotation)
        {
            // Mover moved: overwrite existing verts
            ExtractMoverVertices(SI, CachedMesh->Verts);
            CachedMesh->LastPos = Mov->Location;
            CachedMesh->LastRot = Mov->Rotation;
        }

        for (INT vi = 0; vi < NumPts; ++vi)
        {
            FVector Vert   = CachedMesh->Verts(vi).TransformPointBy(Frame->Coords);
            PolyVertices(vi)      = glm::vec3(Vert.X, Vert.Y, Vert.Z);
        }
    }
    else
    {
        if (SI.ViewSpaceCacheStamp == ViewSpaceCacheGeneration &&
            SI.ViewSpaceVerts.Num() == NumPts && SI.ViewSpaceNormals.Num() == NumPts)
        {
            for (INT vi = 0; vi < NumPts; ++vi)
            {
                PolyVertices(vi) = SI.ViewSpaceVerts(vi);
                PolyVertexNormals(vi) = glm::vec3(SI.ViewSpaceNormals(vi));
            }
        }
        else
        {
            if (SI.ViewSpaceVerts.Num() != NumPts)
            {
                SI.ViewSpaceVerts.Empty();
                SI.ViewSpaceVerts.AddZeroed(NumPts);
            }
            if (SI.ViewSpaceNormals.Num() != NumPts)
            {
                SI.ViewSpaceNormals.Empty();
                SI.ViewSpaceNormals.AddZeroed(NumPts);
            }
            if (SI.ViewSpaceTangents.Num() != NumPts)
            {
                SI.ViewSpaceTangents.Empty();
                SI.ViewSpaceTangents.AddZeroed(NumPts);
            }
            if (SI.ViewSpaceBitangents.Num() != NumPts)
            {
                SI.ViewSpaceBitangents.Empty();
                SI.ViewSpaceBitangents.AddZeroed(NumPts);
            }
            for (INT vi = 0; vi < NumPts; ++vi)
            {
                const FVector Vert = SI.Verts(vi).TransformPointBy(Frame->Coords);
                const FVector Normal = SI.VertexNormals(vi).TransformVectorBy(Frame->Coords).SafeNormal();
                const FVector Tangent = SI.Tangents(vi).TransformVectorBy(Frame->Coords).SafeNormal();
                const FVector Bitangent = SI.Bitangents(vi).TransformVectorBy(Frame->Coords).SafeNormal();

                SI.ViewSpaceVerts(vi) = glm::vec3(Vert.X, Vert.Y, Vert.Z);
                SI.ViewSpaceNormals(vi) = glm::vec4(Normal.X, Normal.Y, Normal.Z, 0.0f);
                SI.ViewSpaceTangents(vi) = glm::vec4(Tangent.X, Tangent.Y, Tangent.Z, 0.0f);
                SI.ViewSpaceBitangents(vi) = glm::vec4(Bitangent.X, Bitangent.Y, Bitangent.Z, 0.0f);
                PolyVertices(vi) = SI.ViewSpaceVerts(vi);
                PolyVertexNormals(vi) = glm::vec3(SI.ViewSpaceNormals(vi));
            }
            SI.ViewSpaceCacheStamp = ViewSpaceCacheGeneration;
        }
    }

	const INT numNodes = SI.Nodes.Num();
	for (INT ni = 0; ni < numNodes; ++ni)
	{
		const FNodeInfo& NI = SI.Nodes(ni);

		INT numTriVerts = NI.TriCount;
		INT triStart = NI.TriStart;
		INT triEnd = triStart + numTriVerts;

		if (NI.VertIndices.Num() < 3)
			continue;

		const INT NeededVerts = numTriVerts;

        if (Shader->DrawBuffer.IsFull() || !Shader->VertBuffer.CanBuffer(NeededVerts))
        {
            // Flush what we have so far
            Shader->DrawBuffer.EndDrawCall(FacetVertexCount);
            Shader->Flush(true);
            Shader->DrawBuffer.StartDrawCall();
            //DrawID = Shader->DrawBuffer.GetDrawID();

            if (NeededVerts >= Shader->VertexBufferSize)
            {
                debugf(TEXT("DrawPrepassSurface facet too big (%d verts, need %d, buffer %d)"),
                       NumPts, NeededVerts, Shader->VertexBufferSize);
                continue;
            }

            FacetVertexCount = 0;
        }

        auto Out = Shader->VertBuffer.GetCurrentElementPtr();
        for (INT ti = triStart; ti < triEnd; ti += 3)
		{
			const INT ia = TriIdx(ti);
			const INT ib = TriIdx(ti + 1);
			const INT ic = TriIdx(ti + 2);

            if (SI.IsMover)
            {
                glm::vec3 A = PolyVertices(ia);
                glm::vec3 B = PolyVertices(ib);
                glm::vec3 C = PolyVertices(ic);
                glm::vec3 N = glm::normalize(glm::cross(C - A, B - A));
                glm::vec3 viewDir = glm::normalize(-A);  // A is view-space position
                if (glm::dot(N, viewDir) < 0.0f) N = -N; // ensure normals always face us.  Possibly overkill
                PolyVertexNormals(ia) = N;
                PolyVertexNormals(ib) = N;
                PolyVertexNormals(ic) = N;
            }

			Out->Coords     = PolyVertices(ia);
            Out->Normal     = PolyVertexNormals(ia);
            Out++;
			Out->Coords     = PolyVertices(ib);
            Out->Normal     = PolyVertexNormals(ib);
            Out++;
			Out->Coords     = PolyVertices(ic);
            Out->Normal     = PolyVertexNormals(ic);
            Out++;
        }

        FacetVertexCount += NeededVerts;
        Shader->VertBuffer.Advance(NeededVerts);
    }

    Shader->DrawBuffer.EndDrawCall(FacetVertexCount);

    unguard;
}

/*-----------------------------------------------------------------------------
	Prepass Surface Shader
-----------------------------------------------------------------------------*/

UXOpenGLRenderDevice::DrawPrepassProgram::DrawPrepassProgram(
    const TCHAR* Name,
    UXOpenGLRenderDevice* RenDev)
    : ShaderProgramImpl(Name, RenDev)
{
    VertexBufferSize             = DRAWCOMPLEX_SIZE * sizeof(DrawPrepassVertex);
    ParametersBufferSize         = 0;      // no per-draw params
    ParametersBufferBindingIndex = 0;      // unused
    NumTextureSamplers           = 0;
    DrawMode                     = GL_TRIANGLES;
    UseSSBOParametersBuffer      = false;
    ParametersInfo               = nullptr;
    bUseExternalShaders          = true;
    VertexShaderFunc             = nullptr;// &BuildVertexShader;
    GeoShaderFunc                = nullptr;
    FragmentShaderFunc           = nullptr;//&BuildFragmentShader;
    ExternalVertexPath           = TEXT("xopengl/shaders/prepass.vert");
    ExternalFragmentPath         = TEXT("xopengl/shaders/prepass.frag");
}

void UXOpenGLRenderDevice::DrawPrepassProgram::CreateInputLayout()
{
    // Position (location = 0)
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0, 3, GL_FLOAT, GL_FALSE,
        sizeof(DrawPrepassVertex),
        (GLvoid*)offsetof(DrawPrepassVertex, Coords)
    );

    // Normal (location = 1)
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 3, GL_FLOAT, GL_FALSE,
        sizeof(DrawPrepassVertex),
        (GLvoid*)offsetof(DrawPrepassVertex, Normal)
    );

    VertBuffer.SetInputLayoutCreated();
}

void UXOpenGLRenderDevice::DrawPrepassProgram::MapBuffers()
{
    VertBuffer.GenerateVertexBuffer(RenDev);
    VertBuffer.MapVertexBuffer(RenDev->UsingPersistentBuffers, VertexBufferSize);
    VertBuffer.Bind();
    CreateInputLayout();
    // no ParametersBuffer at all
}

void UXOpenGLRenderDevice::DrawPrepassProgram::UnmapBuffers()
{
    VertBuffer.DeleteBuffer();
    // no ParametersBuffer to delete
}

void UXOpenGLRenderDevice::DrawPrepassProgram::Flush(bool Rotate)
{
    const auto HavePendingData = DrawBuffer.TotalCommands > 0;

    if (!HavePendingData && !Rotate)
        return;

#if MACOSX || __LINUX_ARM__ || __LINUX_ARM64__
    Rotate = true;
#endif

    if (HavePendingData)
    {
        VertBuffer.BufferData(false);
        DrawBuffer.Draw(DrawMode, RenDev);
        //        glDrawArrays(GL_TRIANGLES,
        //             VertBuffer.SubBufferOffset,   // or 0 to start
        //             VertBuffer.NextElemIndex);    // total verts written

    }

    if (Rotate)
    {
        VertBuffer.Lock();
        VertBuffer.Rotate(true);
    }

    DrawBuffer.Reset(
        VertBuffer.SubBufferOffset + VertBuffer.NextElemIndex,
        0); // no ParametersBuffer
}

void UXOpenGLRenderDevice::DrawPrepassProgram::ActivateShader()
{
    VertBuffer.Wait();
    VertBuffer.Bind();
    if (!VertBuffer.IsInputLayoutCreated())
        CreateInputLayout();
    UseShader();
}

void UXOpenGLRenderDevice::DrawPrepassProgram::DeactivateShader()
{
    Flush(false);
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
