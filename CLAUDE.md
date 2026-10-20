# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

RuntimeImageLoader is an Unreal Engine plugin (UE 5.1+ / 4.27) that loads images, GIFs, and animated WebPs at runtime without hitches. It supports loading from local files, HTTP URLs, and byte arrays. Platforms: Win64, Linux, Mac, Android.

## Build Commands

This is an Unreal Engine plugin — it builds as part of a host UE project. From the host project root:

```bash
# Generate project files (Windows)
"C:/Program Files/Epic Games/UE_5.7/Engine/Build/BatchFiles/GenerateProjectFiles.bat" "path/to/YourProject.uproject" -Game

# Build (Editor target, Win64)
"C:/Program Files/Epic Games/UE_5.7/Engine/Build/BatchFiles/Build.bat" YourProjectEditor Win64 Development -Project="path/to/YourProject.uproject"
```

There are no unit tests. Testing is manual via demo content in `Content/` (a demo level and UMG widget).

## Architecture

### Single Module

The plugin has one runtime module (`RuntimeImageLoader`) plus one external third-party module (`RuntimeGifLibrary`).

### Request Pipeline

```
Blueprint/C++ call (LoadImageAsync, LoadImageSync, etc.)
    -> URuntimeImageLoader (WorldSubsystem, manages request queue)
        -> URuntimeImageReader (FRunnable worker thread, decodes images)
            -> IImageReader (FImageReaderLocal or FImageReaderHttp)
            -> Format-specific helpers (stb_image, TGA, TIFF/FreeImage, QOI, PNG)
            -> RHI texture factory (FRuntimeRHITexture2DFactory / FRuntimeRHITextureCubeFactory)
        <- FImageReadResult (UTexture2D, UTextureCube, or raw pixels)
    <- Delegate callback on game thread
```

### Key Classes

- **`URuntimeImageLoader`** (`Public/RuntimeImageLoader.h`) — WorldSubsystem + FTickableGameObject. The main Blueprint-facing API. Queues requests and dispatches results via delegates.
- **`URuntimeImageReader`** (`Public/RuntimeImageReader.h`) — FRunnable worker thread. Processes the image decode queue off the game thread.
- **`URuntimeGifReader`** (`Public/RuntimeGifReader.h`) — BlueprintAsyncActionBase for async GIF/WebP loading. Runs decoding via TFuture, produces UAnimatedTexture2D.
- **`UAnimatedTexture2D`** (`Public/Texture2DAnimation/AnimatedTexture2D.h`) — Animated texture with playback controls (Play, Stop, SetPlayRate, GotoFrame, etc.). Ticks via FTickableGameObject.

### Image Source Strategy (Factory Pattern)

`FImageReaderFactory` creates either `FImageReaderLocal` (file system) or `FImageReaderHttp` (HTTP download) based on input path.

### GIF/WebP Animation

`FGIFLoaderFactory` selects between `FNSGIFLoader` (libnsgif) and `FWEBPGIFLoader` (libwebp) based on file format. Both implement `IGIFLoader`.

### Texture Creation

Two RHI factory paths:
- **2D**: `FRuntimeRHITexture2DFactory` -> `FRuntimeTexture2DResource`
- **Cubemap**: `FRuntimeRHITextureCubeFactory` -> `FRuntimeTextureCubeResource` (HDR equirectangular -> cubemap conversion)

### Format Support

Decoded via stb_image (PNG, JPEG, BMP, HDR) with format-specific helpers:
- `FTGAHelpers` — RLE decompression for TGA
- `FRuntimeTiffLoadHelper` — TIFF via FreeImage, supports RGBA16
- `FQOILoader` — QOI format
- `FPNGHelpers` — Alpha channel fill for transparent pixels

## Build Configuration Notes

- Unity build is disabled (`bUseUnity = false` in RuntimeImageLoader.Build.cs)
- The module accesses Engine private headers: `Source/Runtime/Renderer/Private`
- Third-party libs (libnsgif, libwebp) are precompiled static libraries per platform in `Source/ThirdParty/RuntimeGifLibrary/`
- Mac is missing libwebp support (marked TODO in RuntimeGifLibrary.Build.cs)
- Compile defines: `WITH_LIBNSGIF=1`, `WITH_LIBWEBP=1`

## Log Categories

- `RuntimeImageLoaderLibHandler` — main plugin
- `LibNsGifHelper` — libnsgif decoder
- `LibWebpGifHelper` — libwebp decoder
- `RuntimeGifReader` — GIF reader
