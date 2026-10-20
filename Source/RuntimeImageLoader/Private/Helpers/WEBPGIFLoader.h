// Copyright 2023 Petr Leontev. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "GIFLoader.h"

THIRD_PARTY_INCLUDES_START
#include "webp/decode.h"
#include "webp/demux.h"
#include "webp/mux_types.h"
THIRD_PARTY_INCLUDES_END

#define BYTES_PER_PIXEL 4


class FWEBPGIFLoader : public IGIFLoader
{
#if WITH_LIBWEBP
public:
	FWEBPGIFLoader() {};
	virtual ~FWEBPGIFLoader();

	FWEBPGIFLoader(const FWEBPGIFLoader&) = delete;
	FWEBPGIFLoader& operator=(const FWEBPGIFLoader&) = delete;

public: /** Gif Data Method */
	const int32 GetWidth() const override;
	const int32 GetHeight() const override;
	const int32 GetTotalFrames() const override;
	FString GetDecodeError() const override;

public: /** Get Next Frame Texture Data*/
	const FColor* GetNextFrame(int32 FrameIndex) override;
	const float GetNextFrameDelay(int32 FrameIndex) override;
	bool DecodeGIF(TArray<uint8>&& GifBytes) override;

	static bool HasValidWebpHeader(const TArray<uint8>& GifBytes);

private:
	bool DecodeAnimation(TArray<uint8>&& GifBytes);
	bool DecodeStillImage(const TArray<uint8>& GifBytes);

	/** Reads per frame durations straight out of the demuxer, without decoding pixels. */
	void ReadFrameDurations();

	/** Decodes every frame back to back into TextureData. Only used within the preload budget. */
	bool PreloadAllFrames(int32 TotalPixels, int64 BytesPerFrame);

	/** Decodes a single frame into the reused frame buffer. Caller must hold DecodeMutex. */
	const FColor* StreamFrame(int32 FrameIndex);

	void ReleaseDecoder();

	void SetError(const char* error);

private:
	/**
	 * Compressed source data. The demuxer inside WebPAnimDecoder points into this buffer
	 * instead of copying it, so it has to outlive the decoder while streaming.
	 */
	TArray<uint8> EncodedData;
	WebPAnimDecoder* AnimDecoder = nullptr;

	/** Preload mode: every frame back to back. Streaming mode: a single reused frame. */
	TArray<FColor> TextureData;
	TArray<float> Timestamps;

	/** Guards the streaming decoder state: AnimDecoder, TextureData, CachedFrameIndex, NextFrameToDecode. */
	FCriticalSection DecodeMutex;

	bool bStreaming = false;

	/** Frame currently held in TextureData while streaming. */
	int32 CachedFrameIndex = INDEX_NONE;

	/** Index the decoder will produce on the next WebPAnimDecoderGetNext() call. */
	int32 NextFrameToDecode = 0;

	int32 Width = -1;
	int32 Height = -1;
	int32 TotalFrameCount = -1;

	FString LastError;

#endif //WITH_LIBWEBP
};
