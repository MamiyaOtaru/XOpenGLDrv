# XOpenGLDrv: Enhanced OpenGL Renderer for Unreal Engine 1 - Experimental graphics fork

Added features from upstream: 

# Soft Sprites.  
This is not configurable, it is just on.
## Off (upstream):
<img width="1274" height="664" alt="image" src="https://github.com/user-attachments/assets/84242612-62b9-4a5a-9cfa-a3bdd9b9a0d5" />
<img width="1196" height="699" alt="image" src="https://github.com/user-attachments/assets/9dc78440-0c29-4c37-9630-c9d07b05e82b" />

## On:
<img width="1053" height="578" alt="image" src="https://github.com/user-attachments/assets/8729ecac-1669-4b58-9193-745bcf1112d3" />
<img width="1047" height="578" alt="image" src="https://github.com/user-attachments/assets/caaebee4-ea78-460e-b6c4-922de6e05902" />

# Corona Scaling.
Toggleable.  If you prefer the original way (constant size) turn this off
## Off:
<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/ba2f68cf-35e3-4164-9dbf-ac8361940e22" />
<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/24c854d8-e5bf-44b3-8ca6-f4179850ef34" />

## On:
<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/66dbe46b-025e-4f9c-b926-06a0c0e45dae" />
<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/21163401-955e-4afc-b323-ee946353b3be" />

# Parallax Occlusion Mapping
Can load external heightmap files.  Improved to not cause holes in eg. DM-Gothic.  Minimizes skew at shallow grazing angles
## Off:
<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/5a0f511e-0517-4c29-8901-f5575d411356" />

## On:
<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/a5d3b6a1-3043-493e-b79f-2b75a5b09cda" />

# Per Pixel Lighting
Can be heavy on old hardware and on some third party maps
## On:
<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/3967d674-9a5b-4f5b-bfc7-7cd7db9e0d53" />

# Phong Shading
Rounds out the facets that the lambert term of per pixel lighting made too strong, and surfaces in general
## On:
<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/1612a7cc-4cf3-487b-93f0-020cff1447fb" />

# HD Occlusion Maps
Level geometry casts shadows
## On:
<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/6b299649-e0c7-4904-8b60-2a63cbd94fca" />

# Screen Space Ambient Occlusion
darkens corners a bit
## On:
<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/3054e0c1-9911-477f-aa91-76b87997e22d" />

# Real time Shadow Mapping
Players and pickups cast dynamic shadows.  Can be heavy.  Low medium and high increase the number shown, Holy Sh*t tries to keep above 60fps and can have many more shadow casters than High, but also less, depending on level complexity.  Set to off if it impacts frames too much
## On:
<img width="1280" height="720" alt="image" src="https://github.com/user-attachments/assets/85ab65c2-ad12-4c0a-9fc4-578c4cb7e4cd" />

Open in tabs and flip back and forth to catch the subtler changes.  
All features listed below Per Pixel Lighting require it to be on to function.




Original upstream readme follows:

# XOpenGLDrv: Enhanced OpenGL Renderer for Unreal Engine 1

This repository hosts the source code for XOpenGLDrv. Smirftsch originally wrote
this renderer for Unreal 1, but it has since been ported to other games
such as Unreal Tournament. The main purpose of this repository is to give
developers a convenient way to contribute to XOpenGL. We do not intend to
publish binaries here. The games XOpenGLDrv supports include XOpenGL binaries in
their patches. For example:

* OldUnreal's patches for Unreal include XOpenGL and can be downloaded
  [here](https://oldunreal.com/downloads/)

* OldUnreal's patches for Unreal Tournament also include XOpenGL and can be
  downloaded
  [here](https://github.com/OldUnreal/UnrealTournamentPatches/releases)

# Build Instructions

## Prerequisites

You should clone this repository into the root folder of the game you want to
build XOpenGLDrv for. You should also ensure that you've installed the latest
SDK for your game.

## Build Options

This repository contains two Visual Studio project files: one for Unreal 227,
and one for Unreal Tournament 469. Building XOpenGLDrv for those two games
should be fairly straightforward. Just open the Visual Studio project file and
build the Release or Debug configuration.

If you are building for Unreal Tournament 469 and you've installed the 469 SDK,
you can also delete the XOpenGLDrv folder that came with the SDK, and replace it
with a clone of this repository. Afterward, you should be able to build
XOpenGLDrv using the CMake project file included in the SDK.

# Settings

XOpenGLDrv can be configured by editing the [XOpenGLDrv.XOpenGLRenderDevice]
section in the game ini. The following settings are currently available:

* UseVSync [Default: Adaptive, Possible Values: On, Off, Adaptive]: Enables or
  disables vertical synchronization. VSync synchronizes the game's frame rate
  with the monitor refresh rate. Setting VSync to Off improves the game's
  responsiveness, but it can lead to screen tearing. Setting VSync to On
  eliminates screen tearing, but leads to higher input lag and potential
  stuttering. Adaptive VSync attempts to offer the best of two worlds (no screen
  tearing and no stuttering), but it requires a compatible GPU and monitor.

* RefreshRate [Default: 0, Type: Integer]: If set to a non-zero value, XOpenGL
  will attempt to set the monitor refresh rate to the configured value when
  switching to fullscreen mode. This setting only works on Windows.

## OpenGL Settings

* OpenGLVersion [Default: Core, Possible Values: Core, ES]: Selects the OpenGL
  profile. GPUs for embedded and mobile devices generally only support the ES
  profile, whereas desktop GPUs can often use both. 

* ShareLists [Default: True, Type: Boolean]: Setting this to true allows
  different instances of XOpenGL to share texture resources. This leads to lower
  overall resource consumption in Unreal Editor, but has no effect in-game.

* UseBufferInvalidation [Default: False, Type: Boolean]: If set to true, XOpenGL
  will tell the GPU (driver) when it can safely discard the contents of a
  rendering buffer. Setting this option to true can improve rendering
  performance for certain GPUs and GPU drivers.

## Brightness and Gamma Correction

* GammaMultiplier [Default: 1.0, Type: Float]: Applies gamma correction to game
  viewport. We currently do this by multiplying the game's brightness. Setting
  GammaMultiplier to values larger than 1.0 increases overall brightness. Values
  below 1.0 reduce overall brightness.

* GammaMultiplierUED [Default: 1.0, Type: Float]: Applies gamma correction to
  editor viewport. We currently do this by multiplying the game's
  brightness. Setting GammaMultiplierUED to values larger than 1.0 increases
  overall brightness. Values below 1.0 reduce overall brightness.

* GammaOffsetScreenshots [Default: 0.7, Type: Float]: Applies gamma correction
  to in-game screenshots. Contrary to the aforementioned viewport gamma
  correction, our screenshot gamma correction applies a non-linear correction to
  the image. Higher values lead to stronger corrections.

* AlwaysMipmap [Default: False, Type: Boolean]: If set to true, XOpenGL will
  automatically create mipmaps for high-resolution textures that were saved as
  single images (without lower-resolution mips). Enabling this option can
  improve the game frame rate, but cause some minor stuttering.

* GenerateMipMaps [Default: False, Type: Boolean]: If set to true, XOpenGL will
  only upload the highest-resolution image of each mipmap to the GPU, and it
  will use the GPU to generate the lower-resolution mips. Enabling this option
  can be useful when you're rendering textures that were saved with bad mipmap
  filters.

* OneXBlending [Default: False, Type: Boolean]: If set to true, XOpenGL will
  reduce the LightMap intensity for level geometry by 50%. This matches the
  behavior of the original Direct3D 7 Renderer.

* ActorXBlending [Default: True, Type: Boolean]: If set to false, XOpenGL will
  increase the light intensity for meshes by 50%. This makes meshes appear much
  brighter.

## Texture Detail

* LODBias [Default: 0, Type: Float]: Changes the level of detail for textures
  applied to distant surfaces and objects. Higher values increase the level of
  detail. Lower values decrease it. We recommend keeping the default setting.

* DetailMax [Default: 2, Type: Integer]: Sets the number of detail textures. If
  set to a non-zero value, this setting can increase the level of detail on
  certain meshes and surfaces. Detail texturing does, however, reduce the game
  frame rate.

* MacroTextures [Default: True, Type: Boolean]: If set to true, XOpenGL will
  apply macro textures to applicable surfaces and meshes. Macro textures can
  improve the level of rendering detail for large surfaces (such as
  terrain). Disabling macro textures can (slightly) improve rendering
  performance.

* BumpMaps [Default: True, Type: Boolean]: If set to true, XOpenGL will apply
  bump map textures to applicable surfaces and meshes. Bump maps create an
  illusion of depth on object surfaces, but were not widely used in vanilla
  content for Unreal and Unreal Tournament. Disabling this option can (slightly)
  improve rendering performance.

* ParallaxVersion [Default: Disabled, Possible Options: Disabled, Basic,
  Occlusion, Relief]: If enabled, XOpenGL will apply parallax height maps to
  applicable surfaces. Parallax mapping gives surfaces more apparent depth and
  looks vastly more realistic than bump mapping. None of the vanilla content for
  Unreal and Unreal Tournament used parallax mapping.

## Anti-Aliasing and Texture Filtering

* UseAA [Default: True, Type: Boolean]: If enabled, XOpenGL will use
  Multisampling Anti Aliasing (MSAA). MSAA improves image quality by smoothing
  jagged edges. It does, however, reduce the game's frame rate. We recommend
  disabling this option on lower-end GPUs.

* NumAASamples [Default: 4, Type: Integer]: Sets the number of MSAA
  samples. Higher values lead to stronger smoothing and, therefore, better image
  quality. Lower values lead to worse image quality but better frame rates. This
  setting has no effect if UseAA is set to False. NumAASamples is subject to
  hardware limitations. The typical minimum value is 2. The typical maximum
  value is 16.

* NoAATiles [Default: True, Type: Boolean]: If set to true, XOpenGL will not
  apply MSAA to 2D tiles such as HUD elements. You will probably want to keep
  this option enabled!

* NoFiltering [Default: False, Type: Boolean]: If set to true, XOpenGL will
  disable texture filtering. Texture filtering makes distant objects appear
  sharper, but also decreases the game's frame rate.

* UseTrilinear [Default: True, Type: Boolean]: If set to true, XOpenGL will use
  trilinear texture filtering. If set to false, XOpenGL will use bilinear
  filtering. Trilinear filtering improves image quality, but (slightly) reduces
  the game frame rate.

* MaxAnisotropy [Default: 4.0, Type: Float]: Sets the number of anisotropic
  filtering samples. If set to values higher than 0.0, XOpenGL will use
  anisotropic texture filtering. Anisotropic filtering produces even sharper
  images than trilinear filtering by accounting for the camera viewing
  angle. Higher values will increase the number of anisotropic samples and will
  lead to sharper images. Lower values decrease sharpness but improve frame
  rates. As with MSAA, anisotropic filtering is subject to hardware
  limitations. Typical GPUs support 2, 4, 8, or 16 anisotropic samples.

## Debugging Options

For expert users only! If XOpenGL does not work (well) on your system, these
options can help you figure out what is wrong. We recommend that you keep these
options set to their default values. Non-default values can massively reduce
rendering performance!

* UseOpenGLDebug [Default: False, Type: Boolean]: If set to true, XOpenGL
  enables the OpenGL driver's debug extension. This makes the driver run sanity
  checks on all OpenGL commands and report errors in the game if necessary. 

* DebugLevel [Default: 2, Type: Integer]: Sets the verbosity level for the
  OpenGL debug extension. If you set this option to 3, XOpenGL will report all
  warnings and errors. This option has no effect if UseOpenGLDebug is set to
  false.

* NoBuffering [Default: False, Type: Boolean]: If set to true, XOpenGL will
  immediately flush all rendering buffers and issue rendering calls after
  processing a call by the game's rendering subsystem.

* NoDrawComplexSurface [Default: False, Type: Boolean]: If set to true, XOpenGL
  will not render BSP surfaces.

* NoDrawGouraud [Default: False, Type: Boolean]: If set to true, XOpenGL will
  not render any of the gouraud meshes that are rendered via the DrawGouraudPolygon API.

* NoDrawGouraudList [Default: False, Type: Boolean]: If set to true, XOpenGL
  will not render any of the gouraud meshes that are rendered via Unreal 227's
  DrawGouraudPolyList or Unreal Tournament 469's DrawGouraudTriangles APIs.

* NoDrawTile [Default: False, Type: Boolean]: If set to true, XOpenGL will not
  render any tiles through the DrawTile API.

* NoDrawSimple [Default: False, Type: Boolean]: If set to true, XOpenGL will not
  render any lines and shapes through APIs such as Draw2DLine.

## Game-Specific Options

These options only apply to specific games. XOpenGL will ignore these options in
other games:

* UseHWClipping [Default: True, Type: Boolean, Supported Games: Unreal 227]: If
  set to true, XOpenGL will ask the rendering subsystem not to pre-clip mesh
  faces on the CPU. This setting improves rendering performance, and should be
  supported on all GPUs XOpenGL can feasibly run on.

* UseEnhancedLightmaps [Default: True, Type: Boolean, Supported Games: Unreal
  227]: If set to true, XOpenGL will ask the rendering subsystem to apply
  high-resolution lightmaps to applicable surfaces. This setting improves image
  quality but may (slightly) reduce performance.

* UseBindlessTextures [Default: True, Type: Boolean, Supported Games: Unreal
  227, Unreal Tournament 469]: If set to true, XOpenGL will attempt to render
  textures without binding them to a texture unit. This increases rendering
  throughput and performance, but requires a somewhat modern GPU. If the GPU
  does not support bindless textures, XOpenGL will automatically disable this
  option while the game/editor is running.

* UseShaderDrawParameters [Default: True, Type: Boolean, Supported Games: Unreal
  227, Unreal Tournament 469]: If set to true, XOpenGL will attempt to pass the
  parameters for every draw call in a shader storage buffer object (SSBO). This
  allows XOpenGL to issue multiple rendering commands without having to
  synchronize with the GPU. UseShaderDrawParameters typically increases
  rendering throughput and performance, but requires a modern GPU. If the GPU
  does not support SSBOs, XOpenGL will automatically disable this option while
  the game/editor is running.

* UseLightmapAtlas [Default: True, Type: Boolean, Supported Games: Unreal
  Tournament 469]: If set to true, XOpenGL will ask the rendering subsystem to
  merge all lightmaps into a single atlas texture. This option can massively
  increase rendering performance and lower overall resource consumption. It also
  makes XOpenGL use far fewer bindless textures.

## Experimental Options

These options control features we're still working on:

* UsesRGBTextures [Default: False, Type: Boolean]: If set to true, XOpenGL will
  use an sRGB frame buffer to render to.

* SimulateMultiPass [Default: False, Type: Boolean]: Enables multipass rendering.

# Bug Reports

If you discover any bugs in XOpenGLDrv, then please report them via the Unreal
or Unreal Tournament bug trackers if possible.

# Contributions

We invite you to contribute new features and bug fixes to XOpenGLDrv. Please
submit your contributions as pull requests.

# License

See LICENSE.md.
