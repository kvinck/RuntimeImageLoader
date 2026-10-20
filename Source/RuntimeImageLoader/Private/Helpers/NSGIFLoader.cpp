// Copyright 2023 Petr Leontev. All Rights Reserved.

#include "NSGIFLoader.h"
#include "RuntimeImageLoaderLog.h"

DEFINE_LOG_CATEGORY(LibNsGifHelper);

#if WITH_LIBNSGIF

/** libnsgif reports NSGIF_INFINITE or a near-zero delay for some frames. Matches libnsgif's own defaults. */
static const uint32 GNsGifMinDelayCs = 2;
static const uint32 GNsGifDefaultDelayCs = 10;

void* FNSGIFLoader::bitmap_create(int width, int height)
{
	if (width > 4096 || height > 4096) {
		return NULL;
	}

	return calloc(width * height, BYTES_PER_PIXEL);
}

unsigned char* FNSGIFLoader::bitmap_get_buffer(void* bitmap)
{
	return (unsigned char*)bitmap;
}

void FNSGIFLoader::bitmap_destroy(void* bitmap)
{
	free(bitmap);
}

FNSGIFLoader::~FNSGIFLoader()
{
	ReleaseDecoder();
}

void FNSGIFLoader::ReleaseDecoder()
{
	if (Gif != nullptr)
	{
		nsgif_destroy(Gif);
		Gif = nullptr;
	}

	Info = nullptr;
	EncodedData.Empty();
	bStreaming = false;
}

void FNSGIFLoader::Warning(const char* context)
{
	LastContext = ANSI_TO_TCHAR(context);

	UE_LOG(LibNsGifHelper, Warning, TEXT("Decode error: %s"), *GetDecodeError());
}


FString FNSGIFLoader::GetDecodeError() const
{
	FString CurrError(nsgif_strerror(LastError));
	return FString::Printf(TEXT("FNsGifLoader: %s: %s"), *LastContext, *CurrError);
}

bool FNSGIFLoader::DecodeGIF(TArray<uint8>&& GifBytes)
{
	FScopeLock Lock(&DecodeMutex);

	/* load file into memory. Must be kept alive: nsgif_data_scan() references this
	 * buffer instead of copying it, and frames are decoded from it on demand. */
	EncodedData = MoveTemp(GifBytes);

	/* create our gif animation. B8G8R8A8 matches FColor's in-memory layout, so frames
	 * can be memcpy'd straight out. Older libnsgif builds only know R8G8B8A8. */
	LastError = nsgif_create(&bitmap_callbacks, NSGIF_BITMAP_FMT_B8G8R8A8, &Gif);
	if (LastError != NSGIF_OK)
	{
		bNeedsSwizzle = true;
		LastError = nsgif_create(&bitmap_callbacks, NSGIF_BITMAP_FMT_R8G8B8A8, &Gif);
	}

	if (LastError != NSGIF_OK)
	{
		Warning("nsgif_create");
		return false;
	}

	LastError = nsgif_data_scan(Gif, EncodedData.Num(), EncodedData.GetData());
	if (LastError != NSGIF_OK)
	{
		/* Not fatal; some GIFs are nasty. Can still try to decode
		 * any frames that were decoded successfully. */
		Warning("nsgif_data_scan");
		return false;
	}

	nsgif_data_complete(Gif);

	/** Gif Info */
	Info = nsgif_get_info(Gif);
	if (Info == nullptr || Info->frame_count == 0 || Info->width == 0 || Info->height == 0)
	{
		UE_LOG(LibNsGifHelper, Warning, TEXT("Failed to decode GIF! Please check input data is valid. Otherwise contact developers for an advice"));
		ReleaseDecoder();
		return false;
	}

	Width = (int32)Info->width;
	Height = (int32)Info->height;
	TotalFrameCount = (int32)Info->frame_count;

	ReadFrameDurations();

	// TArray uses int32 indexing, so a frame's pixel count must fit in int32 even though
	// only one frame is held at a time while streaming.
	const int64 PixelsPerFrame = (int64)Width * Height;
	const int64 TotalPixels = (int64)TotalFrameCount * PixelsPerFrame;
	const int64 TotalBytes = TotalPixels * BYTES_PER_PIXEL;
	const int64 PreloadBudget = RuntimeGifDecode::GetPreloadBudgetBytes();

	if (PixelsPerFrame > MAX_int32)
	{
		UE_LOG(LibNsGifHelper, Error, TEXT("Frame too large for TArray<FColor>: %dx%d = %lld pixels (max %d). Reduce resolution."),
			Width, Height, PixelsPerFrame, MAX_int32);
		ReleaseDecoder();
		return false;
	}

	// Holding every decoded frame is fastest during playback but costs
	// Frames * Width * Height * 4 bytes, which reaches hundreds of MB for large
	// animations and used to abort the process with a fatal out-of-memory error.
	// Only preload when the whole animation comfortably fits.
	if (TotalPixels <= MAX_int32 && TotalBytes <= PreloadBudget)
	{
		if (PreloadAllFrames((int32)TotalPixels))
		{
			// Every frame lives in TextureData now, so the decoder and source bytes are dead weight.
			ReleaseDecoder();
			return true;
		}

		UE_LOG(LibNsGifHelper, Warning, TEXT("Preloading GIF frames failed. Falling back to decoding frames on demand."));
	}
	else
	{
		UE_LOG(
			LibNsGifHelper, Log,
			TEXT("Streaming GIF: %d frames at %dx%d need %.1f MB preloaded, budget is %.1f MB. Decoding frames on demand instead."),
			TotalFrameCount, Width, Height, TotalBytes / (1024.0 * 1024.0), PreloadBudget / (1024.0 * 1024.0)
		);
	}

	bStreaming = true;
	CachedFrameIndex = INDEX_NONE;

	TextureData.Empty((int32)PixelsPerFrame);
	TextureData.AddUninitialized((int32)PixelsPerFrame);

	// Decode frame 0 up front so the texture has valid contents the moment it is created.
	if (StreamFrame(0) == nullptr)
	{
		UE_LOG(LibNsGifHelper, Warning, TEXT("Failed to decode GIF! Please check input data is valid. Otherwise contact developers for an advice"));
		ReleaseDecoder();
		return false;
	}

	return true;
}

const int32 FNSGIFLoader::GetWidth() const
{
	return Width;
}

const int32 FNSGIFLoader::GetHeight() const
{
	return Height;
}

const int32 FNSGIFLoader::GetTotalFrames() const
{
	return TotalFrameCount;
}

void FNSGIFLoader::ReadFrameDurations()
{
	Timestamps.Reset();
	Timestamps.Reserve(TotalFrameCount);

	// Delays come from the scanned frame headers, so the timeline is known without
	// decoding pixels - which is what the old decode-everything path relied on.
	for (int32 FrameInd = 0; FrameInd < TotalFrameCount; ++FrameInd)
	{
		const nsgif_frame_info_t* FrameInfo = nsgif_get_frame_info(Gif, (uint32_t)FrameInd);

		uint32 DelayCs = (FrameInfo != nullptr) ? FrameInfo->delay : GNsGifDefaultDelayCs;
		if (DelayCs == NSGIF_INFINITE || DelayCs < GNsGifMinDelayCs)
		{
			DelayCs = GNsGifDefaultDelayCs;
		}

		Timestamps.Add(DelayCs * 10.f / 1000.f);
	}
}

void FNSGIFLoader::CopyFramePixels(FColor* Dest, const uint8* Source) const
{
	const int64 NumPixels = (int64)Width * Height;

	if (!bNeedsSwizzle)
	{
		FPlatformMemory::Memcpy(Dest, Source, NumPixels * BYTES_PER_PIXEL);
		return;
	}

	// libnsgif handed us R8G8B8A8; FColor's constructor writes it out as BGRA.
	for (int64 PixelInd = 0; PixelInd < NumPixels; ++PixelInd)
	{
		const uint8* Pixel = Source + PixelInd * BYTES_PER_PIXEL;
		Dest[PixelInd] = FColor(Pixel[0], Pixel[1], Pixel[2], Pixel[3]);
	}
}

bool FNSGIFLoader::PreloadAllFrames(int32 TotalPixels)
{
	TextureData.Empty(TotalPixels);
	TextureData.AddZeroed(TotalPixels);

	const int64 PixelsPerFrame = (int64)Width * Height;
	int32 DecodedFrames = 0;

	for (int32 FrameInd = 0; FrameInd < TotalFrameCount; ++FrameInd)
	{
		nsgif_bitmap_t* Bitmap = nullptr;

		LastError = nsgif_frame_decode(Gif, (uint32_t)FrameInd, &Bitmap);
		if (LastError != NSGIF_OK || Bitmap == nullptr)
		{
			// Leave this frame black and keep going; some GIFs have broken frames.
			Warning("nsgif_frame_decode");
			continue;
		}

		CopyFramePixels(TextureData.GetData() + FrameInd * PixelsPerFrame, (const uint8*)Bitmap);
		++DecodedFrames;
	}

	if (DecodedFrames == 0)
	{
		TextureData.Empty();
		return false;
	}

	return true;
}

const FColor* FNSGIFLoader::StreamFrame(int32 FrameIndex)
{
	if (Gif == nullptr || TextureData.Num() == 0)
	{
		return nullptr;
	}

	if (FrameIndex == CachedFrameIndex)
	{
		return TextureData.GetData();
	}

	// libnsgif tracks which frame it last produced and replays whatever earlier frames
	// the disposal method needs, so any index is valid here.
	nsgif_bitmap_t* Bitmap = nullptr;

	LastError = nsgif_frame_decode(Gif, (uint32_t)FrameIndex, &Bitmap);
	if (LastError != NSGIF_OK || Bitmap == nullptr)
	{
		Warning("nsgif_frame_decode");

		// Keep showing the frame already in the buffer rather than flashing black.
		return CachedFrameIndex != INDEX_NONE ? TextureData.GetData() : nullptr;
	}

	CopyFramePixels(TextureData.GetData(), (const uint8*)Bitmap);
	CachedFrameIndex = FrameIndex;

	return TextureData.GetData();
}

const FColor* FNSGIFLoader::GetNextFrame(int32 FrameIndex)
{
	FScopeLock Lock(&DecodeMutex);

	if (TotalFrameCount <= 0)
	{
		return nullptr;
	}

	if (FrameIndex < 0 || FrameIndex > TotalFrameCount - 1)
	{
		FrameIndex = 0;
	}

	if (bStreaming)
	{
		return StreamFrame(FrameIndex);
	}

	// Calculate the starting index of the desired frame in the TextureData array
	const int64 StartIndex64 = (int64)FrameIndex * Width * Height;

	if (StartIndex64 >= 0 && StartIndex64 < TextureData.Num())
	{
		return &TextureData[(int32)StartIndex64];
	}
	else
	{
		return nullptr;
	}
}

const float FNSGIFLoader::GetNextFrameDelay(int32 FrameIndex)
{
	if (!Timestamps.IsValidIndex(FrameIndex))
	{
		FrameIndex = 0;
	}

	return Timestamps.IsValidIndex(FrameIndex) ? Timestamps[FrameIndex] : 0.1f;
}
#endif //WITH_LIBNSGIF
