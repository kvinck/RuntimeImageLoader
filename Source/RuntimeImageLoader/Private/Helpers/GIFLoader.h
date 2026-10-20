// Copyright 2023 Petr Leontev. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class IGIFLoader
{
public:
	virtual ~IGIFLoader() = default;

	virtual bool DecodeGIF(TArray<uint8>&& GifBytes) = 0;
	virtual FString GetDecodeError() const = 0;
	virtual const int32 GetWidth() const = 0;
	virtual const int32 GetHeight() const = 0;
	virtual const int32 GetTotalFrames() const = 0;

	/**
	 * Returns the pixels of a single frame in FColor (BGRA) layout.
	 *
	 * The returned buffer is owned by the loader. Large animations are decoded on
	 * demand into a single reused frame buffer, so the pointer is only guaranteed to
	 * be valid until the next GetNextFrame() call - callers must copy the data out
	 * before handing it to another thread.
	 */
	virtual const FColor* GetNextFrame(int32 FrameIndex) = 0;
	virtual const float GetNextFrameDelay(int32 FrameIndex) = 0;
};

namespace RuntimeGifDecode
{
	/**
	 * Maximum number of bytes a loader may spend on holding every decoded frame in RAM.
	 * Animations bigger than this are decoded frame by frame during playback instead,
	 * which trades a small per-frame CPU cost for a bounded memory footprint.
	 */
	int64 GetPreloadBudgetBytes();
}

class FGIFLoaderFactory
{
public:
	static TUniquePtr<IGIFLoader> CreateLoader(const FString& GifURI, const TArray<uint8>& GifData);
};
