// ExternalTextureLoader - implementation
// See ExternalTextureLoader.h

#include "ExternalTextureLoader.h"
#include "XOpenGLDrv.h"
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <vector>
#include <memory>
#include <windows.h> // GetFileAttributesW
#include <sys/stat.h>
#include <string>    // added
#include <sstream>   // added for key building

#ifdef USE_TINYDDSLOADER
// Compile the tinyddsloader implementation into this TU exactly once.
#define TINYDDSLOADER_IMPLEMENTATION
// tinyddsloader: optional dependency. Place tinyddsloader.h in project and define USE_TINYDDSLOADER.
#include "thirdparty/tinyddsloader/tinyddsloader.h"
using namespace tinyddsloader;
#endif

// Optional: stb_image fallback for PNG/TGA
#ifdef USE_STB_IMAGE
#define STB_IMAGE_IMPLEMENTATION
#include "thirdparty/stb/stb_image.h"
#endif

namespace ExternalTexture
{
	/*struct FBindlessAuditEntry
	{
		GLuint TextureId = 0;
		GLuint64 Handle = 0;
		bool Resident = false;
		FString Tag; // e.g. "ExternalDiffuse", "ExternalBump", "EngineTexture"
	};
	static TMap<GLuint64, FBindlessAuditEntry> GBindlessAudit;

	void Audit_Create(GLuint texId, GLuint64 handle, const TCHAR* tag)
	{
		FBindlessAuditEntry E;
		E.TextureId = texId;
		E.Handle = handle;
		E.Resident = false;
		E.Tag = tag;

		GBindlessAudit.Set(handle, E);

		debugf(TEXT("AUDIT: Created handle %llu for tex %u (%s)"), handle, texId, tag);
	}

	void Audit_Resident(GLuint64 handle)
	{
		if (auto* Entry = GBindlessAudit.Find(handle))
		{
			Entry->Resident = true;
			debugf(TEXT("AUDIT: Handle %llu made RESIDENT"), handle);
		}
	}

	void Audit_NonResident(GLuint64 handle)
	{
		if (auto* Entry = GBindlessAudit.Find(handle))
		{
			Entry->Resident = false;
			debugf(TEXT("AUDIT: Handle %llu made NON-RESIDENT"), handle);
		}
	}

	void Audit_Delete(GLuint64 handle)
	{
		if (auto* Entry = GBindlessAudit.Find(handle))
		{
			debugf(TEXT("AUDIT: Deleting handle %llu (tex %u, tag %s)"),
				   handle, Entry->TextureId, *Entry->Tag);
			GBindlessAudit.Remove(handle);
		}
	}

	void Audit_Use(GLuint64 handle)
	{
		if (auto* Entry = GBindlessAudit.Find(handle))
		{
			if (!Entry->Resident)
			{
				debugf(TEXT("AUDIT ERROR: Using NON-RESIDENT handle %llu (tex %u, tag %s)"),
					   handle, Entry->TextureId, *Entry->Tag);
			}
		}
		else
		{
			debugf(TEXT("AUDIT WARNING: Using UNKNOWN handle %llu"), handle);
		}
	}

	void ExternalTexture::Audit_Error(GLuint64 Handle, const TCHAR* Reason)
	{
		if (!GIsEditor && !GIsRunning) return;

		FString Msg;
		Msg = FString::Printf(
			TEXT("AUDIT ERROR: Handle %llu — %ls"),
			Handle,
			Reason
		);

		GWarn->Log(Msg);
	}*/

	// suffixes exactly as Kentie defined them (plus ORM map)
	static const wchar_t* kSuffixes[] = { L"", L".detail", L".bump", L".height", L".orm" };
	static const int kNumSuffixes = 5; // main + 4 extras

	// Internal storage to keep allocated FMipmap/MipData alive until Shutdown.
	// Keyed by cacheID (Info.CacheID) to mimic Kentie's behaviour of replacing the game's cache entry.
	static std::map<unsigned long long, std::vector<FMipmap*>> g_allocatedMips;

	// Map parent CacheID -> base path (the basePath used to probe candidate files).
	// Stored when LoadExternalTexture detects a main override so GetExtra can derive extra filenames.
	static std::map<unsigned long long, std::wstring> g_parentBasePath;

	// Map stable storage for externally-created FTextureInfo objects for extras.
	// Key is string "<parentID>_<extraIdx>".
	static std::map<std::string, FTextureInfo*> g_extraInfos;

	// Forward declaration so stb fallback can call it before the full definition below.
	static FMipmap* MakeMipFromRGBA8(unsigned char* rgba, int width, int height);

	// CacheID -> whether extras exist (detail/bump/height)
	//static std::map<unsigned long long, bool> g_hasExtras;
	static std::map<uint64_t, bool> g_hasExtraState;

	inline uint64_t MakeExtraStateKey(uint64_t parentID, int extraIdx) {
		return parentID ^ (uint64_t(extraIdx + 1) << 60); 
	}

	static inline bool FileExistsW(const wchar_t* path)
	{
		DWORD attrs = GetFileAttributesW(path);
		return (attrs != INVALID_FILE_ATTRIBUTES) && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
	}

	static inline unsigned int NextPow2(unsigned int v)
	{
		if (v == 0) return 1;
		--v;
		v |= v >> 1;
		v |= v >> 2;
		v |= v >> 4;
		v |= v >> 8;
		v |= v >> 16;
		return v + 1;
	}

	static inline unsigned int CeilLog2(unsigned int v)
	{
		unsigned int p = 0;
		unsigned int x = 1;
		while (x < v) { x <<= 1; ++p; }
		return p;
	}

	// Helper: build a stable key for g_extraInfos
	static inline std::string MakeExtraKey(unsigned long long parentID, int extraIdx)
	{
		std::ostringstream ss;
		ss << parentID << "_" << extraIdx;
		return ss.str();
	}

	// Helper: convert wide path to UTF-8 std::string
	static bool WideToUtf8(const wchar_t* src, std::string& out)
	{
		int utf8len = WideCharToMultiByte(CP_UTF8, 0, src, -1, nullptr, 0, nullptr, nullptr);
		if (utf8len == 0) return false;
		out.resize(utf8len);
		if (!WideCharToMultiByte(CP_UTF8, 0, src, -1, &out[0], utf8len, nullptr, nullptr)) return false;
		return true;
	}

	// Helper: load an image file (DDS via tinyddsloader or fallback stb) into an FTextureInfo object.
	// On success returns true and leaves allocated FMipmap* in g_allocatedMips[key] so Shutdown will free them.
	static bool LoadImageFileToInfo(const wchar_t* filePathW, FTextureInfo* OutInfo, unsigned long long cacheKey)
	{
#ifdef USE_TINYDDSLOADER
		// Try tinyddsloader first
		{
			std::string utf8path;
			if (!WideToUtf8(filePathW, utf8path))
				return false;

			DDSFile dds;
			if (dds.Load(utf8path.c_str()) == Result::Success)
			{
				auto format = dds.GetFormat();
				int numMips = static_cast<int>(dds.GetMipCount());
				if (numMips <= 0) numMips = 1;

				// free any previous mips for cacheKey
				auto &vec = g_allocatedMips[cacheKey];
				for (auto *m : vec) { delete m; }
				vec.clear();

				OutInfo->NumMips = numMips;
				for (int mip = 0; mip < numMips && mip < MAX_MIPS; ++mip)
				{
					const auto* img = dds.GetImageData((uint32_t)mip, 0);
					unsigned int w = img ? img->m_width : 0;
					unsigned int h = img ? img->m_height : 0;
					unsigned int dataSize = img ? img->m_memSlicePitch : 0;
					const unsigned char* src = img ? reinterpret_cast<const unsigned char*>(img->m_mem) : nullptr;

					unsigned int USize = NextPow2(w);
					unsigned int VSize = NextPow2(h);
					BYTE UBits = (BYTE)CeilLog2(USize);
					BYTE VBits = (BYTE)CeilLog2(VSize);

					FMipmap* mipObj = new FMipmap(UBits, VBits);
					unsigned char* dataCopy = nullptr;
					if (src && dataSize > 0)
					{
						dataCopy = new unsigned char[dataSize];
						memcpy(dataCopy, src, dataSize);
					}
					mipObj->DataPtr = dataCopy;
					mipObj->USize = w;
					mipObj->VSize = h;

					OutInfo->Mips[mip] = mipObj;
					vec.push_back(mipObj);
				}

				// Map common DXGI formats to engine ETextureFormat
				switch (format)
				{
				case DDSFile::DXGIFormat::BC1_UNorm:
				case DDSFile::DXGIFormat::BC1_UNorm_SRGB:
					OutInfo->Format = (ETextureFormat)TEXF_BC1;
					break;
				case DDSFile::DXGIFormat::BC2_UNorm:
				case DDSFile::DXGIFormat::BC2_UNorm_SRGB:
					OutInfo->Format = (ETextureFormat)TEXF_BC2;
					break;
				case DDSFile::DXGIFormat::BC3_UNorm:
				case DDSFile::DXGIFormat::BC3_UNorm_SRGB:
					OutInfo->Format = (ETextureFormat)TEXF_BC3;
					break;
				case DDSFile::DXGIFormat::BC4_UNorm:
					OutInfo->Format = (ETextureFormat)TEXF_BC4;
					break;
				case DDSFile::DXGIFormat::BC5_UNorm:
					OutInfo->Format = (ETextureFormat)TEXF_BC5;
					break;
#ifdef DDS_FILE_SUPPORTS_BC6BC7
				case DDSFile::DXGIFormat::BC6H_UF16:
				case DDSFile::DXGIFormat::BC6H_SF16:
					OutInfo->Format = (ETextureFormat)TEXF_BC6H;
					break;
				case DDSFile::DXGIFormat::BC7_UNorm:
				case DDSFile::DXGIFormat::BC7_UNorm_SRGB:
					OutInfo->Format = (ETextureFormat)TEXF_BC7;
					break;
#endif
				default:
					// Unknown compressed format for tinyddsloader path
					return false;
				}

				// Fill size/scale/clamp for top-level mip
				if (OutInfo->NumMips > 0 && OutInfo->Mips[0])
				{
					OutInfo->USize = OutInfo->Mips[0]->USize;
					OutInfo->VSize = OutInfo->Mips[0]->VSize;
					OutInfo->UClamp = OutInfo->USize;
					OutInfo->VClamp = OutInfo->VSize;
					OutInfo->UScale = 1.0f;
					OutInfo->VScale = 1.0f;
				}
				return true;
			}
		}
#endif // USE_TINYDDSLOADER

#ifdef USE_STB_IMAGE
		// Fallback to stb_image
		{
			std::string utf8path;
			if (!WideToUtf8(filePathW, utf8path))
				return false;

			int x, y, n;
			unsigned char* data = stbi_load(utf8path.c_str(), &x, &y, &n, 4);
			if (!data) return false;

			FMipmap* topMip = MakeMipFromRGBA8(data, x, y);
			stbi_image_free(data);

			// store in global map to keep memory alive
			auto &vec = g_allocatedMips[cacheKey];
			for (auto* m : vec) { delete m; }
			vec.clear();
			vec.push_back(topMip);

			OutInfo->NumMips = 1;
			OutInfo->Mips[0] = topMip;
			OutInfo->USize = topMip->USize;
			OutInfo->VSize = topMip->VSize;
			OutInfo->UClamp = x;
			OutInfo->VClamp = y;
			OutInfo->UScale = 1.0f;
			OutInfo->VScale = 1.0f;
			OutInfo->Palette = nullptr;
			OutInfo->Format = (ETextureFormat)TEXF_BGRA8;
			return true;
		}
#endif

		return false;
	}

	bool Init()
	{
		// Register a process-exit hook so allocations owned by the external loader are released even if
		// the renderer doesn't explicitly call Shutdown(). Also safe if renderer calls Shutdown() — Shutdown()
		// is idempotent by design.
		static bool registered = []() -> bool
		{
			// atexit accepts a pointer to a function with signature void(void).
			// A capture-less lambda converts to such a pointer.
			std::atexit([]()
			{
				// call the namespace Shutdown; this will run at process exit
				ExternalTexture::Flush();
			});
			return true;
		}();

		// nothing else to do currently
		return true;
	}

	// Helper: read flags file (hex integer) and OR into InOutPolyFlags
	static void ReadFlagsFile(wchar_t const* filePath, DWORD& InOutPolyFlags)
	{
		FILE* f = nullptr;
		_wfopen_s(&f, filePath, L"r");
		if (!f)
			return;
		unsigned long flags = 0;
		if (fwscanf_s(f, L"%lx", &flags) == 1)
		{
			InOutPolyFlags |= static_cast<DWORD>(flags);
		}
		fclose(f);
	}

	// Helper: create a FMipmap containing BGRA8 pixel data (unpadded to nearest pow2 with replication of last column/row)
	static FMipmap* MakeMipFromRGBA8(unsigned char* rgba, int width, int height)
	{
		// Calculate padded sizes (next pow2) - engine expects power-of-two mip storage.
		unsigned int USize = NextPow2((unsigned int)width);
		unsigned int VSize = NextPow2((unsigned int)height);
		unsigned int UBits = CeilLog2(USize);
		unsigned int VBits = CeilLog2(VSize);

		FMipmap* mip = new FMipmap((BYTE)UBits, (BYTE)VBits);
		// allocate pixel memory as DWORDs (BGRA8)
		size_t pixels = (size_t)USize * (size_t)VSize;
		DWORD* buf = new DWORD[pixels];

		// Fill with last valid pixel for padding regions.
		for (unsigned int y = 0; y < VSize; ++y)
		{
			for (unsigned int x = 0; x < USize; ++x)
			{
				int srcX = x < (unsigned int)width ? x : (width - 1);
				int srcY = y < (unsigned int)height ? y : (height - 1);
				int srcIdx = (srcY * width + srcX) * 4;
				unsigned char r = rgba[srcIdx + 0];
				unsigned char g = rgba[srcIdx + 1];
				unsigned char b = rgba[srcIdx + 2];
				unsigned char a = rgba[srcIdx + 3];
				// store as DWORD in native memory layout expected by renderer (packed 0xAARRGGBB or similar).
				// The engine expects FMipmap->DataPtr to contain bytes compatible with UploadTexture's SourceFormat.
				// UploadTexture for TEXF_BGRA8 sets SourceFormat = GL_BGRA and uses DataPtr directly.
				// We therefore pack as 0xAARRGGBB but we also supply bytes in memory as BGRA per-pixel so reinterpretation will work.
				DWORD pixel = (DWORD)((a << 24) | (r << 16) | (g << 8) | (b));
				buf[y * USize + x] = pixel;
			}
		}

		mip->DataPtr = (BYTE*)buf;
		return mip;
	}

	bool LoadExternalTexture(FTextureInfo& Info, DWORD& InOutPolyFlags)
	{
		// Fast-fail: nothing to build a filename from.
		if (!Info.Texture)
			return false;

		// Kentie's check: name must start with "Texture "
		if (!Info.Texture || !Info.Texture->IsValid())
			return false;
		//debugf(TEXT("TryLoadExternalTexture: Texture=%p PolyFlags=%08X"), Info.Texture, InOutPolyFlags);
		const TCHAR* texName = Info.Texture->GetFullName();
		if (!texName)
			return false;

		const TCHAR* kPrefix = TEXT("Texture ");
		if (appStrnicmp(texName, kPrefix, appStrlen(kPrefix)) != 0)
			return false;

		// Build base override path as Kentie: "..\\textures\\" + (texName + 8)
		wchar_t basePath[MAX_PATH];
		errno_t err = wcscpy_s(basePath, MAX_PATH, L"..\\textures\\");
		if (err) return false;
		err = wcscat_s(basePath, MAX_PATH, texName + wcslen(kPrefix)); // skip "Texture "
		if (err) return false;

		// Replace '.' with '\\' (start at +2 as Kentie did)
		wchar_t* loc;
		while ((loc = wcschr(basePath + 2, L'.')) != nullptr)
			*loc = L'\\';

		// Build candidate filenames for main override + extras
		wchar_t candidate[MAX_PATH];
		wchar_t foundMain[MAX_PATH] = { 0 };
		bool foundMainFile = false;

		for (int i = 0; i < kNumSuffixes; ++i)
		{
			err = wcscpy_s(candidate, MAX_PATH, basePath);
			if (err) continue;
			err = wcscat_s(candidate, MAX_PATH, kSuffixes[i]);
			if (err) continue;
			err = wcscat_s(candidate, MAX_PATH, L".dds");
			if (err) continue;

			if (FileExistsW(candidate))
			{
				if (i == 0)
				{
					wcscpy_s(foundMain, MAX_PATH, candidate);
					foundMainFile = true;
					// store basePath so GetExtra can resolve extras:
					g_parentBasePath[(unsigned long long)Info.CacheID] = std::wstring(basePath);
					break; // Kentie loads the main override only if present; extras are loaded after main.
				}
			}
		}

		if (!foundMainFile)
		{
			// Still store basePath so extras can be probed later
			g_parentBasePath[(unsigned long long)Info.CacheID] = std::wstring(basePath);

			// Mark extras unknown for now (GetExtra will probe once)
			// Mark all extras as “unknown” for this parent
			for (int extraIdx = 1; extraIdx < ExternalTexture::DUMMY_NUM_EXTRAS; ++extraIdx)
			{
				uint64_t stateKey = MakeExtraStateKey(Info.CacheID, extraIdx);
				g_hasExtraState.erase(stateKey); // remove any stale state
			}

			return false;
		}

		// Read flags file if present: "<main>.flags"
		wchar_t flagsPath[MAX_PATH];
		wcscpy_s(flagsPath, MAX_PATH, foundMain);
		wcscat_s(flagsPath, MAX_PATH, L".flags");
		if (FileExistsW(flagsPath))
		{
			ReadFlagsFile(flagsPath, InOutPolyFlags);
		}

#ifdef USE_STB_IMAGE
		// local helper to run stb_image fallback. returns true on success (and sets Info appropriately)
		auto stb_fallback_func = [&](void)->bool
		{
			int x = 0, y = 0, n = 0;
			// Convert wide foundMain to UTF-8
			int utf8len = WideCharToMultiByte(CP_UTF8, 0, foundMain, -1, nullptr, 0, nullptr, nullptr);
			if (utf8len == 0) return false;
			std::vector<char> utf8(utf8len);
			if (!WideCharToMultiByte(CP_UTF8, 0, foundMain, -1, utf8.data(), utf8len, nullptr, nullptr)) return false;

			unsigned char* data = stbi_load(utf8.data(), &x, &y, &n, 4);
			if (!data) return false;

			// Convert RGBA -> BGRA and create FMipmap
			FMipmap* topMip = MakeMipFromRGBA8(data, x, y);

			// Free stb image data
			stbi_image_free(data);

			// store in global map to keep memory alive
			unsigned long long key = (unsigned long long)Info.CacheID;
			g_allocatedMips[key].push_back(topMip);

			// assign into Info
			Info.NumMips = 1;
			Info.Mips[0] = topMip;
			Info.USize = topMip->USize;
			Info.VSize = topMip->VSize;
			Info.UClamp = (INT)x;
			Info.VClamp = (INT)y;
			Info.UScale = 1.0f;
			Info.VScale = 1.0f;
			Info.Palette = nullptr;
			// Choose BGRA8 format so UploadTexture will pick GL_BGRA path.
			Info.Format = (ETextureFormat)TEXF_BGRA8;
			return true;
		};
#else
		// stub that always fails when stb not available
		auto stb_fallback_func = [&](void)->bool { return false; };
#endif

		//
		// Load the main image
		//
#ifdef USE_TINYDDSLOADER
		// Preferred path: tinyddsloader for reliable DDS support (BC1/2/3/4/5/6/7).
		// Caller must add tinyddsloader header into project and define USE_TINYDDSLOADER.
		DDSFile dds;
		// tinydds expects narrow path: convert foundMain (wchar_t) to UTF-8
		int utf8len_for_dds = WideCharToMultiByte(CP_UTF8, 0, foundMain, -1, nullptr, 0, nullptr, nullptr);
		if (utf8len_for_dds == 0)
		{
#ifdef USE_STB_IMAGE
			if (stb_fallback_func()) return true;
#endif
			return false;
		}
		std::vector<char> utf8path(utf8len_for_dds);
		WideCharToMultiByte(CP_UTF8, 0, foundMain, -1, utf8path.data(), utf8len_for_dds, nullptr, nullptr);

		auto loadResult = dds.Load(utf8path.data());
		if (loadResult != Result::Success)
		{
			// fallback: try stb or fail
#ifdef USE_STB_IMAGE
			if (stb_fallback_func()) return true;
#else
			return false;
#endif
		}

		// Prepare Info as compressed format if possible.
		// Map tinyddsloader format to engine TEXF_* enums.
		// We only implement common mappings here; extend as needed.
		auto format = dds.GetFormat();
		// Build per-mip FMipmap instances and copy the data into them (tinyddsloader gives raw pointers)
		int numMips = (int)dds.GetMipCount();
		if (numMips <= 0) numMips = 1;

		// Free previously allocated mips for this CacheID if any
		unsigned long long key = (unsigned long long)Info.CacheID;
		auto &vec = g_allocatedMips[key];
		for (auto *m : vec) { delete m; }
		vec.clear();

		Info.NumMips = numMips;
		for (int mip = 0; mip < numMips && mip < MAX_MIPS; ++mip)
		{
			const auto* img = dds.GetImageData((uint32_t)mip, 0);
			unsigned int w = img ? img->m_width : 0;
			unsigned int h = img ? img->m_height : 0;
			unsigned int dataSize = img ? img->m_memSlicePitch : 0;
			const unsigned char* src = img ? reinterpret_cast<const unsigned char*>(img->m_mem) : nullptr;

			// Create FMipmap with UBits/VBits based on actual sizes (pad to pow2)
			unsigned int USize = NextPow2(w);
			unsigned int VSize = NextPow2(h);
			BYTE UBits = (BYTE)CeilLog2(USize);
			BYTE VBits = (BYTE)CeilLog2(VSize);

			FMipmap* mipObj = new FMipmap(UBits, VBits);
			// For compressed formats we store the raw compressed data pointer in DataPtr and
			// set USize/VSize fields to the mip dims (UploadTexture will treat compressed case separately).
			// Allocate a buffer and copy the compressed blocks.
			unsigned char* dataCopy = nullptr;
			if (src && dataSize > 0)
			{
				dataCopy = new unsigned char[dataSize];
				memcpy(dataCopy, src, dataSize);
			}
			mipObj->DataPtr = dataCopy;
			// tinyddsloader stores sizes in ImageData; set Mip's reported sizes correctly:
			mipObj->USize = w;
			mipObj->VSize = h;

			Info.Mips[mip] = mipObj;
			vec.push_back(mipObj);
		}

		// Set format mapping (common cases)
		// tinyddsloader exposes DXGIFormat enum; map some common ones:
		switch (format)
		{
		case DDSFile::DXGIFormat::BC1_UNorm:
		case DDSFile::DXGIFormat::BC1_UNorm_SRGB:
			Info.Format = (ETextureFormat)TEXF_BC1;
			break;
		case DDSFile::DXGIFormat::BC2_UNorm:
		case DDSFile::DXGIFormat::BC2_UNorm_SRGB:
			Info.Format = (ETextureFormat)TEXF_BC2;
			break;
		case DDSFile::DXGIFormat::BC3_UNorm:
		case DDSFile::DXGIFormat::BC3_UNorm_SRGB:
			Info.Format = (ETextureFormat)TEXF_BC3;
			break;
		case DDSFile::DXGIFormat::BC4_UNorm:
			Info.Format = (ETextureFormat)TEXF_BC4;
			break;
		case DDSFile::DXGIFormat::BC5_UNorm:
			Info.Format = (ETextureFormat)TEXF_BC5;
			break;
#ifdef DDS_FILE_SUPPORTS_BC6BC7
		case DDSFile::DXGIFormat::BC6H_UF16:
		case DDSFile::DXGIFormat::BC6H_SF16:
			Info.Format = (ETextureFormat)TEXF_BC6H;
			break;
		case DDSFile::DXGIFormat::BC7_UNorm:
		case DDSFile::DXGIFormat::BC7_UNorm_SRGB:
			Info.Format = (ETextureFormat)TEXF_BC7;
			break;
#endif
		default:
			// If unknown compressed format, fail and fallback.
#ifdef USE_STB_IMAGE
			// Use the stb_image fallback helper instead of goto
			if (stb_fallback_func())
				return true;
			else
				return false;
#else
			return false;
#endif
		}

		// Fill size/scale/clamp fields for top-level mip
		if (Info.NumMips > 0 && Info.Mips[0])
		{
			Info.USize = Info.Mips[0]->USize;
			Info.VSize = Info.Mips[0]->VSize;
			Info.UClamp = Info.USize;
			Info.VClamp = Info.VSize;
			Info.UScale = 1.0f;
			Info.VScale = 1.0f;
		}
		// success
		return true;

#else // !USE_TINYDDSLOADER

		// Fallback: try to load image with stb_image (png/tga etc)
		if (stb_fallback_func())
		{
			return true;
		}
		else
		{
			// No DDS support compiled and no stb_image fallback: cannot load.
			return false;
		}
#endif // USE_TINYDDSLOADER
	}

	// Minimal stub used by SetTexture to query extras.
	// Will attempt to load an extra file based on the stored base path for parentID.
	// Returns pointer to a heap-allocated FTextureInfo that lives until Shutdown(); returns nullptr if not found.
	FTextureInfo* GetExtra(unsigned long long parentID, int extraIdx)
	{
		// Validate index
		if (extraIdx < 0 || extraIdx >= (int)ExternalTexture::DUMMY_NUM_EXTRAS)
			return nullptr;

		const int suffixIndex = extraIdx; // 1->detail, 2->bump, 3->height
		if (suffixIndex >= kNumSuffixes)
			return nullptr;

		// ---------------------------
		// NEW: Check extras-known state
		// ---------------------------
		uint64_t stateKey = MakeExtraStateKey(parentID, extraIdx);

		// If we already know THIS specific extra is missing, bail out
		auto itHas = g_hasExtraState.find(stateKey);
		if (itHas != g_hasExtraState.end() && itHas->second == false)
			return nullptr;

		// Check cached extras
		std::string key = MakeExtraKey(parentID, extraIdx);
		auto infoIt = g_extraInfos.find(key);
		if (infoIt != g_extraInfos.end())
			return infoIt->second;

		// Find base path
		auto it = g_parentBasePath.find(parentID);
		if (it == g_parentBasePath.end())
			return nullptr;

		std::wstring basePath = it->second;
		wchar_t candidate[MAX_PATH];

		if (wcscpy_s(candidate, MAX_PATH, basePath.c_str()) != 0)
			return nullptr;
		if (wcscat_s(candidate, MAX_PATH, kSuffixes[suffixIndex]) != 0)
			return nullptr;
		if (wcscat_s(candidate, MAX_PATH, L".dds") != 0)
			return nullptr;

		bool exists = FileExistsW(candidate);

	#ifdef USE_STB_IMAGE
		if (!exists)
		{
			// Try PNG
			wchar_t alt[MAX_PATH];
			if (wcscpy_s(alt, MAX_PATH, basePath.c_str()) != 0) return nullptr;
			if (wcscat_s(alt, MAX_PATH, kSuffixes[suffixIndex]) != 0) return nullptr;
			if (wcscat_s(alt, MAX_PATH, L".png") != 0) return nullptr;

			if (FileExistsW(alt))
			{
				wcscpy_s(candidate, MAX_PATH, alt);
				exists = true;
			}
			else
			{
				// Try TGA
				if (wcscpy_s(alt, MAX_PATH, basePath.c_str()) != 0) return nullptr;
				if (wcscat_s(alt, MAX_PATH, kSuffixes[suffixIndex]) != 0) return nullptr;
				if (wcscat_s(alt, MAX_PATH, L".tga") != 0) return nullptr;

				if (FileExistsW(alt))
				{
					wcscpy_s(candidate, MAX_PATH, alt);
					exists = true;
				}
			}
		}
	#endif

		// ---------------------------
		// NEW: If no extras exist, record that and never check again
		// ---------------------------
		if (!exists)
		{
			g_hasExtraState[stateKey] = false;
			return nullptr;
		}

		// Create and load FTextureInfo
		FTextureInfo* newInfo = new FTextureInfo();
		memset(newInfo, 0, sizeof(FTextureInfo));
		unsigned long long newCacheID = parentID ^ (static_cast<unsigned long long>(extraIdx + 1) << 48);
		newInfo->CacheID = newCacheID;
		newInfo->Texture = nullptr;
		newInfo->Palette = nullptr;

		if (!LoadImageFileToInfo(candidate, newInfo, newCacheID))
		{
			delete newInfo;
		    // Treat corrupt/unloadable files as missing
		    g_hasExtraState[stateKey] = false;
			return nullptr;
		}

		// Mark extras exist
		g_hasExtraState[stateKey] = true;

		g_extraInfos.emplace(key, newInfo);
		return newInfo;
	}



	// Shutdown: free allocated FMipmaps and any FTextureInfo created
	void Flush()
	{
		for (auto &p : g_allocatedMips)
		{
			for (FMipmap* m : p.second)
			{
				if (m)
				{
					if (m->DataPtr)
					{
						delete[] m->DataPtr;
					}
					delete m;
				}
			}
		}
		g_allocatedMips.clear();

		// free created FTextureInfo objects for extras
		for (auto &p : g_extraInfos)
		{
			if (p.second)
				delete p.second;
		}
		g_extraInfos.clear();
		g_parentBasePath.clear();
	}
}