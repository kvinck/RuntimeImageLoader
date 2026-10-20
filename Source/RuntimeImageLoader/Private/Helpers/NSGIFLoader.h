// Copyright 2023 Petr Leontev. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "GIFLoader.h"

THIRD_PARTY_INCLUDES_START
extern "C" {
	#include "nsgif.h"
}
THIRD_PARTY_INCLUDES_END

#define BYTES_PER_PIXEL 4


class FNSGIFLoader : public IGIFLoader
{
#if WITH_LIBNSGIF
public:
	FNSGIFLoader() {};
	virtual ~FNSGIFLoader();

	FNSGIFLoader(const FNSGIFLoader&) = delete;
	FNSGIFLoader& operator=(const FNSGIFLoader&) = delete;

public: /** Gif Data Method */
	const int32 GetWidth() const override;
	const int32 GetHeight() const override;
	const int32 GetTotalFrames() const override;
    FString GetDecodeError() const override;

public: /** Get Next Frame Texture Data*/
    const FColor* GetNextFrame(int32 FrameIndex) override;
	const float GetNextFrameDelay(int32 FrameIndex) override;
	bool DecodeGIF(TArray<uint8>&& GifBytes) override;

protected: /** Bitmap Callbacks Methods */
	static void* bitmap_create(int width, int height);
	static unsigned char* bitmap_get_buffer(void* bitmap);
	static void bitmap_destroy(void* bitmap);

	void Warning(const char* context);

private:
	/** Reads every frame's delay from the scanned GIF without decoding pixels. */
	void ReadFrameDurations();

	/** Decodes every frame back to back into TextureData. Only used within the preload budget. */
	bool PreloadAllFrames(int32 TotalPixels);

	/** Decodes a single frame into the reused frame buffer. Caller must hold DecodeMutex. */
	const FColor* StreamFrame(int32 FrameIndex);

	/** Copies one libnsgif bitmap into FColor (BGRA) layout. */
	void CopyFramePixels(FColor* Dest, const uint8* Source) const;

	void ReleaseDecoder();

private: /** Gif Data*/
	/**
	 * Compressed source data. libnsgif scans this buffer in place rather than copying it,
	 * so it has to outlive the nsgif_t while streaming.
	 */
	TArray<uint8> EncodedData;

	nsgif_t* Gif = nullptr;
	const nsgif_info_t* Info = nullptr;

	/** Preload mode: every frame back to back. Streaming mode: a single reused frame. */
	TArray<FColor> TextureData;
	TArray<float> Timestamps;

	/** Guards the streaming decoder state: Gif, TextureData, CachedFrameIndex. */
	FCriticalSection DecodeMutex;

	bool bStreaming = false;

	/** Set when libnsgif could not give us BGRA directly and pixels need swizzling. */
	bool bNeedsSwizzle = false;

	/** Frame currently held in TextureData while streaming. */
	int32 CachedFrameIndex = INDEX_NONE;

	int32 Width = -1;
	int32 Height = -1;
	int32 TotalFrameCount = -1;

	nsgif_error LastError = NSGIF_OK;
	FString LastContext;

private:
	const nsgif_bitmap_cb_vt bitmap_callbacks = {
		bitmap_create,
		bitmap_destroy,
		bitmap_get_buffer,
	};

#endif //WITH_LIBNSGIF
};
