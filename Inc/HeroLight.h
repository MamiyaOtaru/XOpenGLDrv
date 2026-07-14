#pragma once
#include "FBO.h"
#include "Engine.h"

// forward declarations
class UXOpenGLRenderDevice;
struct FSurfInfo;

// Caching State for the Three-Tier System
struct CachedActorState {
    AActor*  Actor;
    INT      ActorIndex;
    FVector  Location;
    FRotator Rotation;
    FLOAT    AnimFrame;
    BOOL     bIsStaticMesh;
    INT      FaceMask;
};

class UXOpenGLHeroLight
{
private:
    ULevel*   OwnerLevel;
    ALight*   LightActor;
    FLOAT     LastRadius;
    FVector   LastLocation;

    Fbo*      FaceFbos[6];
    INT       shadowmapSize = 512;

    GLuint    ColorCubemapID = 0;
    GLuint    DepthCubemapID = 0;

    GLuint64 BindlessMaskHandle = 0;
    UBOOL    bIsHandleResident = FALSE;

    BOOL bspDrawn[6] = { FALSE, FALSE, FALSE, FALSE, FALSE, FALSE };
    
    TArray<CachedActorState> LastFrameActors;
    BYTE LastFrameFaceMask; // Which faces had actors last frame

    TArray<BYTE> AffectedZones;
    TArray<INT> AffectedBSPSurfaces;
    TArray<INT> AffectedFaceBSPSurfaces[6];
    
    // --- SPOTLIGHT ARCHITECTURE EXTENSION ---
    UBOOL   bIsSpotlight;       // Dynamic shape flag
    FVector SpotDirection;      // Clamped vector pointing down
    FLOAT   SpotCosOuter;       // Outer penumbra cosine cut
    FLOAT   SpotCosInner;       // Inner hotspot cosine cut
    FLOAT   ReachRadius;
    FVector SourceLocation;

    // internal helper to assign surfaces to faces of the cubemap
    void UXOpenGLHeroLight::PartitionBSPSurfaces(UModel* Model, UXOpenGLRenderDevice* GL);

    // Internal helper to render the geometry payload for a single face
    void UXOpenGLHeroLight::RenderFaceGeometry(ULevel* Level,
        FSceneNode* Frame,
        INT FaceIndex,
        TArray<CachedActorState> CurrentFrameActors,
        UXOpenGLRenderDevice* GL
    );

public:

    UXOpenGLHeroLight(ALight* InLight, ULevel* Level, const TMap<INT, TArray<AActor*>>& StaticLightsMap, UXOpenGLRenderDevice* GL);
    ~UXOpenGLHeroLight();

    BYTE CurrentFaceMask = 0;

    UBOOL HasValidShadowMap() const { return (ColorCubemapID != 0); }

    // High-utility master call: Checks visibility, tests caches, 
    // and loops through FBO attachments internally if updates are required.
    void UpdateShadowMap(FSceneNode* Frame, UXOpenGLRenderDevice* GL);

    // Public bindings for screen space composite shader pass
    void BindTextures(GLuint BaseTextureUnit) const;
    void BindDepthTexture(GLuint BaseTextureUnit) const;
    ALight* GetActor() const { return LightActor; }

    GLuint64 GetBindlessMaskHandle() const { return BindlessMaskHandle; }
    UBOOL HasActiveShadowMap() const { return (ColorCubemapID != 0 && BindlessMaskHandle != 0 && CurrentFaceMask > 0); }    
    
    // --- DYNAMIC PROPERTY LOOKUPS --- (unsure if needed yet)
    /*UBOOL IsSpotlight() const { return bIsSpotlight; }
    FLOAT GetSpotCosOuter() const { return SpotCosOuter; }
    FLOAT GetSpotCosInner() const { return SpotCosInner; }
    FVector GetSpotDirection() const { return SpotDirection; }*/

    void ClearShadowMapTexture();

    // Replaces your old multi-slot BindTextures function entirely
    void MakeTextureResident();
};

