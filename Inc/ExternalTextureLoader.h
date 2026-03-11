#pragma once
// ExternalTextureLoader - filename/flags mapping and simple external image loader
//
// Ports Kentie's filename/flags behaviour verbatim (package.name -> ..\textures\package\name.dds)
// Loads .dds (via tinyddsloader if enabled) or fallbacks (png/tga) via stb_image if available.
// On success fills FTextureInfo (NumMips, Mips[0]->DataPtr, sizes, Format, UClamp/VClamp, UScale/VScale)
// and ORs any custom flags from the .flags file into the provided PolyFlags.
//
// Also stores any extra textures (.detail, .bump, .height) it finds and exposes them to the renderer
// so the renderer can bind them to the appropriate TMUs when the diffuse override is used.
//
// Usage:
//   DWORD poly = PolyFlags;
//   if (ExternalTexture::LoadExternalTexture(Info, poly))
//       PolyFlags = poly; // loader may have OR-ed extra flags
//   // after binding the diffuse texture (Multi == DiffuseTextureIndex), call:
//   // ExternalTexture::GetExtra(Info.CacheID, ExternalTexture::ExtraIndex::Detail) etc or
//   // call UXOpenGLRenderDevice::SetTexture(...) which will automatically bind extras when diffuse is bound.
//
// Notes:
//  - The loader keeps allocated FMipmap + pixel buffers owned by the loader until Shutdown().
//  - Add tinyddsloader/stb_image to the project and define USE_TINYDDSLOADER / USE_STB_IMAGE to enable formats.

#ifndef GLuint
typedef unsigned int GLuint;
#endif
#ifndef GLuint64
typedef unsigned long long GLuint64;
#endif

#include <cwchar>
#include <cstdint>
#include "Core.h"

struct FTextureInfo;
typedef unsigned long DWORD;
typedef unsigned long long QWORD;

namespace ExternalTexture
{
	enum ExtraIndex
	{
		Extra_Main = 0,
		Extra_Detail = 1, // mapping to loader suffixes; API uses explicit enum
		Extra_Bump  = 2,
		Extra_Height= 3,
		DUMMY_NUM_EXTRAS = 4
	};

	// Initialize (optional).
	bool Init();

	// Try to load an external texture that overrides Info.Texture.
	// On success returns true and updates Info and InOutPolyFlags.
	bool LoadExternalTexture(FTextureInfo& Info, DWORD& InOutPolyFlags);

	// Returns pointer to an externally-loaded extra FTextureInfo for the given parent CacheID and extra index.
	// Caller must NOT delete the returned pointer. Pointer lifetime is until Shutdown().
	// Returns nullptr if no extra exists.
	FTextureInfo* GetExtra(QWORD ParentCacheID, int ExtraIndex);

	// Release all cached external allocations (call at shutdown).
	void Flush();

	/*void Audit_Create(GLuint texId, GLuint64 handle, const TCHAR* tag);
	void Audit_Resident(GLuint64 handle);
	void Audit_NonResident(GLuint64 handle);
	void Audit_Delete(GLuint64 handle);
	void Audit_Use(GLuint64 handle);
	void Audit_Error(GLuint64 Handle, const TCHAR* Reason);*/
}
