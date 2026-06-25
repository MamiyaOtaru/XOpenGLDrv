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

    Fbo*      ShadowFbo;

    GLuint64 BindlessMaskHandle = 0;
    UBOOL    bIsHandleResident = FALSE;

    BYTE CurrentFaceMask = 0;
    
    TArray<CachedActorState> LastFrameActors;
    BYTE LastFrameFaceMask; // Which faces had actors last frame

    TArray<BYTE> AffectedZones;
    TArray<INT> AffectedBSPSurfaces;
    TArray<INT> AffectedFaceBSPSurfaces[6];

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

    UBOOL HasValidShadowMap() const { return (ShadowFbo != nullptr); }

    // The core pipeline call: Evaluates the scene state and 
    // updates the textures only when absolutely necessary.
    // Returns TRUE if a valid shadowmap exists for the final shader.
    UBOOL PrepareShadowMap(ULevel* Level, FSceneNode* Frame, BYTE& OutActiveFaces);

    // High-utility master call: Checks visibility, tests caches, 
    // and loops through FBO attachments internally if updates are required.
    void UpdateShadowMap(FSceneNode* Frame, UXOpenGLRenderDevice* GL);

    // Public bindings for screen space composite shader pass
    void BindTextures(GLuint BaseTextureUnit) const;
    ALight* GetActor() const { return LightActor; }

    GLuint64 GetBindlessMaskHandle() const { return BindlessMaskHandle; }
    UBOOL HasActiveShadowMap() const { return (ShadowFbo != nullptr && BindlessMaskHandle != 0 && CurrentFaceMask > 0); }

    // Replaces your old multi-slot BindTextures function entirely
    void MakeTextureResident();
};

