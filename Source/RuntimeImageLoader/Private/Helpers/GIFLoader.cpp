// Copyright 2023 Petr Leontev. All Rights Reserved.

#include "GIFLoader.h"
#include "NSGIFLoader.h"
#include "WEBPGIFLoader.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"

static int32 GRuntimeGifPreloadBudgetMB = 256;
static FAutoConsoleVariableRef CVarRuntimeGifPreloadBudgetMB(
    TEXT("RuntimeImageLoader.GifPreloadBudgetMB"),
    GRuntimeGifPreloadBudgetMB,
    TEXT("Maximum size in MB of fully decoded GIF/WebP animation data kept in RAM.\n")
    TEXT("Animations that would exceed this are decoded frame by frame during playback.\n")
    TEXT("Set to 0 to always stream."),
    ECVF_Default
);

int64 RuntimeGifDecode::GetPreloadBudgetBytes()
{
    const int64 ConfiguredBytes = (int64)FMath::Max(GRuntimeGifPreloadBudgetMB, 0) * 1024 * 1024;

    // Even when the configured budget is generous, never try to preload more than a
    // quarter of what the OS can currently hand out. A failed allocation inside UE is
    // fatal, so the decision has to be made before asking for the memory.
    const FPlatformMemoryStats MemoryStats = FPlatformMemory::GetStats();
    const int64 AvailableBytes = (int64)(MemoryStats.AvailablePhysical / 4);

    return FMath::Min(ConfiguredBytes, AvailableBytes);
}

TUniquePtr<IGIFLoader> FGIFLoaderFactory::CreateLoader(const FString& GifURI, const TArray<uint8>& GifData)
{
    if (GifURI.EndsWith(".gif"))
    {
        return MakeUnique<FNSGIFLoader>();
    }
    else if (GifURI.EndsWith(".webp"))
    {
        return MakeUnique<FWEBPGIFLoader>();
    }

    if (FWEBPGIFLoader::HasValidWebpHeader(GifData))
    {
        return MakeUnique<FWEBPGIFLoader>();
    }

    // construct .gif loader as it is more popular than .webp
    return MakeUnique<FNSGIFLoader>();
}
