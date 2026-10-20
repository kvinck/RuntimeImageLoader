// Copyright 2023 Petr Leontev. All Rights Reserved.

#include "WEBPGIFLoader.h"
#include "RuntimeImageLoaderLog.h"

DEFINE_LOG_CATEGORY(LibWebpGifHelper);

#if WITH_LIBWEBP

FWEBPGIFLoader::~FWEBPGIFLoader()
{
    ReleaseDecoder();
}

void FWEBPGIFLoader::ReleaseDecoder()
{
    if (AnimDecoder != nullptr)
    {
        WebPAnimDecoderDelete(AnimDecoder);
        AnimDecoder = nullptr;
    }

    EncodedData.Empty();
    bStreaming = false;
}

const int32 FWEBPGIFLoader::GetWidth() const
{
    return Width;
}


const int32 FWEBPGIFLoader::GetHeight() const
{
    return Height;
}


const int32 FWEBPGIFLoader::GetTotalFrames() const
{
    return TotalFrameCount;
}


FString FWEBPGIFLoader::GetDecodeError() const
{
    return FString::Printf(TEXT("WebpGifLoader: %s"), *LastError);
}


const FColor* FWEBPGIFLoader::GetNextFrame(int32 FrameIndex)
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

const float FWEBPGIFLoader::GetNextFrameDelay(int32 FrameIndex)
{
    if (!Timestamps.IsValidIndex(FrameIndex))
    {
        FrameIndex = 0;
    }

    return Timestamps.IsValidIndex(FrameIndex) ? Timestamps[FrameIndex] : 0.1f;
}


bool FWEBPGIFLoader::DecodeGIF(TArray<uint8>&& GifBytes)
{
    FScopeLock Lock(&DecodeMutex);

    if (WebPGetInfo(GifBytes.GetData(), GifBytes.Num(), &Width, &Height) == 0)
    {
        SetError("Failed to validate .webp header. Please check input data is valid!");
        return false;
    }

    WebPDecoderConfig DecoderConfig;
    WebPInitDecoderConfig(&DecoderConfig);

    if (WebPGetFeatures(GifBytes.GetData(), GifBytes.Num(), &DecoderConfig.input) != VP8_STATUS_OK)
    {
        SetError("Failed to retrieve features from .webp file. Please check input data is valid!");
        return false;
    }

    if (DecoderConfig.input.has_animation)
    {
        return DecodeAnimation(MoveTemp(GifBytes));
    }

    return DecodeStillImage(GifBytes);
}


bool FWEBPGIFLoader::DecodeAnimation(TArray<uint8>&& GifBytes)
{
    // Must be kept alive: WebPAnimDecoderNew() demuxes in place rather than copying.
    EncodedData = MoveTemp(GifBytes);

    WebPAnimDecoderOptions DecodingOptions;
    if (WebPAnimDecoderOptionsInit(&DecodingOptions) == 0)
    {
        SetError("Failed to initialize decoding options. Version mismatch. Please libwebp is valid!");
        return false;
    }

#if PLATFORM_WINDOWS
    DecodingOptions.color_mode = MODE_BGRA;  // Unreal on Windows will most often use DirectX so prefer BGRA, otherwise devs should change this line!
#else
    DecodingOptions.color_mode = MODE_RGBA;  // Default to RGBA for other platforms (Mobile, Vulkan, etc.)
#endif

    WebPData GifData;
    {
        GifData.bytes = EncodedData.GetData();
        GifData.size = EncodedData.Num();
    }

    // Tune 'dec_options' as needed.
    AnimDecoder = WebPAnimDecoderNew(&GifData, &DecodingOptions);
    if (AnimDecoder == nullptr)
    {
        SetError("Failed to create WebPAnimDecoder. Please check input data is valid!");
        return false;
    }

    WebPAnimInfo AnimationInfo;
    if (WebPAnimDecoderGetInfo(AnimDecoder, &AnimationInfo) == 0)
    {
        SetError("Failed to retrieve .webp animation info. Please check input data is valid!");
        ReleaseDecoder();
        return false;
    }

    Width = (int32)AnimationInfo.canvas_width;
    Height = (int32)AnimationInfo.canvas_height;
    TotalFrameCount = (int32)AnimationInfo.frame_count;

    if (Width <= 0 || Height <= 0 || TotalFrameCount <= 0)
    {
        SetError("Decoded .webp animation is empty. Please check input data is valid!");
        ReleaseDecoder();
        return false;
    }

    // TArray uses int32 indexing, so a frame's pixel count must fit in int32 even though
    // only one frame is held at a time while streaming. Byte offsets use int64 throughout
    // because they exceed int32 long before the pixel count does.
    const int64 PixelsPerFrame = (int64)Width * Height;
    const int64 BytesPerFrame = PixelsPerFrame * BYTES_PER_PIXEL;

    if (PixelsPerFrame > MAX_int32)
    {
        LastError = FString::Printf(TEXT("Frame too large for TArray<FColor>: %dx%d = %lld pixels (max %d). Reduce resolution."),
            Width, Height, PixelsPerFrame, MAX_int32);
        UE_LOG(LibWebpGifHelper, Warning, TEXT("Decode error: %s"), *LastError);
        ReleaseDecoder();
        return false;
    }

    ReadFrameDurations();

    const int64 TotalPixels = (int64)TotalFrameCount * PixelsPerFrame;
    const int64 TotalBytes = TotalPixels * BYTES_PER_PIXEL;
    const int64 PreloadBudget = RuntimeGifDecode::GetPreloadBudgetBytes();

    // Holding every decoded frame is fastest during playback but costs
    // Frames * Width * Height * 4 bytes, which reaches hundreds of MB for large
    // animations and used to abort the process with a fatal out-of-memory error.
    // Only preload when the whole animation comfortably fits.
    if (TotalPixels <= MAX_int32 && TotalBytes <= PreloadBudget)
    {
        if (!PreloadAllFrames((int32)TotalPixels, BytesPerFrame))
        {
            ReleaseDecoder();
            return false;
        }

        // Every frame lives in TextureData now, so the decoder and source bytes are dead weight.
        ReleaseDecoder();
        return true;
    }

    UE_LOG(
        LibWebpGifHelper, Log,
        TEXT("Streaming .webp animation: %d frames at %dx%d need %.1f MB preloaded, budget is %.1f MB. Decoding frames on demand instead."),
        TotalFrameCount, Width, Height, TotalBytes / (1024.0 * 1024.0), PreloadBudget / (1024.0 * 1024.0)
    );

    bStreaming = true;
    CachedFrameIndex = INDEX_NONE;
    NextFrameToDecode = 0;

    TextureData.Empty((int32)PixelsPerFrame);
    TextureData.AddUninitialized((int32)PixelsPerFrame);

    // Decode frame 0 up front so the texture has valid contents the moment it is created.
    if (StreamFrame(0) == nullptr)
    {
        SetError("Failed to decode first .webp frame. Please check input data is valid!");
        ReleaseDecoder();
        return false;
    }

    return true;
}


bool FWEBPGIFLoader::DecodeStillImage(const TArray<uint8>& GifBytes)
{
    // WebPDecode* writes back the real dimensions, so decode before sizing the buffer.
    // Channel order must match DecodeAnimation's color_mode.
#if PLATFORM_WINDOWS
    uint8_t* DecodedData = WebPDecodeBGRA(GifBytes.GetData(), GifBytes.Num(), &Width, &Height);
#else
    uint8_t* DecodedData = WebPDecodeRGBA(GifBytes.GetData(), GifBytes.Num(), &Width, &Height);
#endif
    if (DecodedData == nullptr)
    {
        SetError("Failed to decode .webp file. Please check input data is valid!");
        return false;
    }

    Timestamps.Reset();
    Timestamps.Add(100.f);
    TotalFrameCount = 1;

    const int64 PixelsPerFrame = (int64)Width * Height;

    TextureData.Empty((int32)PixelsPerFrame);
    TextureData.AddUninitialized((int32)PixelsPerFrame);

    FPlatformMemory::Memcpy((uint8*)TextureData.GetData(), DecodedData, PixelsPerFrame * BYTES_PER_PIXEL);

    WebPFree(DecodedData);

    return true;
}


void FWEBPGIFLoader::ReadFrameDurations()
{
    Timestamps.Reset();
    Timestamps.Reserve(TotalFrameCount);

    // The demuxer already knows every frame's duration, so the timeline can be built
    // without decoding pixels - which is what the old preload-everything path relied on.
    const WebPDemuxer* Demuxer = WebPAnimDecoderGetDemuxer(AnimDecoder);
    if (Demuxer != nullptr)
    {
        WebPIterator Iterator;
        if (WebPDemuxGetFrame(Demuxer, 1, &Iterator))
        {
            do
            {
                Timestamps.Add(Iterator.duration / 1000.f);
            }
            while (WebPDemuxNextFrame(&Iterator));

            WebPDemuxReleaseIterator(&Iterator);
        }
    }

    if (Timestamps.Num() < TotalFrameCount)
    {
        UE_LOG(
            LibWebpGifHelper, Warning,
            TEXT("Only %d of %d .webp frame durations could be read. Remaining frames use a 100 ms delay."),
            Timestamps.Num(), TotalFrameCount
        );

        while (Timestamps.Num() < TotalFrameCount)
        {
            Timestamps.Add(0.1f);
        }
    }
}


bool FWEBPGIFLoader::PreloadAllFrames(int32 TotalPixels, int64 BytesPerFrame)
{
    TextureData.Empty(TotalPixels);
    TextureData.AddUninitialized(TotalPixels);

    int32 FrameInd = 0;
    while (FrameInd < TotalFrameCount && WebPAnimDecoderHasMoreFrames(AnimDecoder))
    {
        uint8_t* DecodedData = nullptr;
        int Timestamp = 0;
        if (WebPAnimDecoderGetNext(AnimDecoder, &DecodedData, &Timestamp) == 0)
        {
            SetError("Failed to decode .webp frame. Please check input data is valid!");
            return false;
        }

        const int64 ByteOffset = (int64)FrameInd * BytesPerFrame;
        FPlatformMemory::Memcpy((uint8*)TextureData.GetData() + ByteOffset, DecodedData, BytesPerFrame);
        ++FrameInd;
    }

    if (FrameInd < TotalFrameCount)
    {
        SetError("Decoded fewer .webp frames than the animation reports. Please check input data is valid!");
        return false;
    }

    return true;
}


const FColor* FWEBPGIFLoader::StreamFrame(int32 FrameIndex)
{
    if (AnimDecoder == nullptr || TextureData.Num() == 0)
    {
        return nullptr;
    }

    if (FrameIndex == CachedFrameIndex)
    {
        return TextureData.GetData();
    }

    // WebPAnimDecoder only walks forward, so seeking backwards means replaying from
    // the start. Sequential playback - the common case - never hits this.
    if (FrameIndex < NextFrameToDecode)
    {
        WebPAnimDecoderReset(AnimDecoder);
        NextFrameToDecode = 0;
    }

    const int64 BytesPerFrame = (int64)Width * Height * BYTES_PER_PIXEL;
    const uint8* LatestFrame = nullptr;

    while (NextFrameToDecode <= FrameIndex)
    {
        if (WebPAnimDecoderHasMoreFrames(AnimDecoder) == 0)
        {
            break;
        }

        uint8_t* DecodedData = nullptr;
        int Timestamp = 0;
        if (WebPAnimDecoderGetNext(AnimDecoder, &DecodedData, &Timestamp) == 0)
        {
            SetError("Failed to decode .webp frame. Please check input data is valid!");
            break;
        }

        LatestFrame = DecodedData;
        ++NextFrameToDecode;
    }

    if (LatestFrame == nullptr)
    {
        // Nothing new was produced - keep showing the frame already in the buffer.
        return CachedFrameIndex != INDEX_NONE ? TextureData.GetData() : nullptr;
    }

    FPlatformMemory::Memcpy((uint8*)TextureData.GetData(), LatestFrame, BytesPerFrame);
    CachedFrameIndex = NextFrameToDecode - 1;

    return TextureData.GetData();
}


bool FWEBPGIFLoader::HasValidWebpHeader(const TArray<uint8>& GifBytes)
{
    int32 WidthTemp, HeightTemp;

    return WebPGetInfo(GifBytes.GetData(), GifBytes.Num(), &WidthTemp, &HeightTemp) == 1 ? true : false;
}

void FWEBPGIFLoader::SetError(const char* error)
{
    LastError = ANSI_TO_TCHAR(error);

    UE_LOG(LibWebpGifHelper, Warning, TEXT("Decode error: %s"), *LastError);
}

#endif //WITH_LIBWEBP
