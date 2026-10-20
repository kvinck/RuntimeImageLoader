// Copyright 2023 Petr Leontev. All Rights Reserved.

#include "Texture2DAnimation/AnimatedTexture2D.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHI.h"
#include "AnimatedTextureResource.h"

DEFINE_LOG_CATEGORY_STATIC(LogAnimatedTexture, Log, All);

void UAnimatedTexture2D::BeginDestroy()
{
	bPlaying = false;

	// Safe to destroy decoder here without flushing render commands because
	// RenderFrameToTexture() copies frame data into the render command's lambda,
	// so in-flight render commands own their own data and don't reference the decoder.
	// WARNING: Do NOT call FlushRenderingCommands() here — BeginDestroy is called
	// during GC, and flushing pumps the game thread queue, causing re-entrant UObject
	// operations that corrupt GC state.
	Decoder.Reset();
	Super::BeginDestroy();
}

void UAnimatedTexture2D::ReleaseResources()
{
	bPlaying = false;

	// Safe to destroy decoder without flushing — render commands copy their own frame data.
	Decoder.Reset();
	MarkAsGarbage();
}

float UAnimatedTexture2D::GetSurfaceWidth() const
{
	if (Decoder) return Decoder->GetWidth();
	return 1.0f;
}

float UAnimatedTexture2D::GetSurfaceHeight() const
{
	if (Decoder) return Decoder->GetHeight();
	return 1.0f;
}

FTextureResource* UAnimatedTexture2D::CreateResource()
{
	FScopeLock ResultsLock(&ResultsMutex);

	UE_LOG(LogAnimatedTexture, Log, TEXT("CreateResource: Creating FAnimatedTextureResource. Decoder valid=%d, Size=%dx%d"), 
		Decoder.IsValid(), SizeX, SizeY);

	// create RHI resource object
	FTextureResource* NewResource = new FAnimatedTextureResource(this);
	return NewResource;
}

void UAnimatedTexture2D::Tick(float DeltaTime)
{
	// DEBUG: Uncomment to verify Tick is being called
	// UE_LOG(LogAnimatedTexture, Verbose, TEXT("Tick called - bPlaying=%d, Decoder=%d"), bPlaying, Decoder.IsValid());
	
	if (!bPlaying)
	{
		return;
	}

	if (!Decoder)
	{
		UE_LOG(LogAnimatedTexture, Warning, TEXT("Tick: Decoder is null but bPlaying is true!"));
		return;
	}

	const int32 TotalFrames = Decoder->GetTotalFrames();
	bool bIsOutsideBounds = (CurrentFrame < 0) || (CurrentFrame >= TotalFrames - 1);
	
	UE_LOG(LogAnimatedTexture, Verbose, TEXT("Tick: CurrentFrame=%d, TotalFrames=%d, bIsOutsideBounds=%d, bLooping=%d"), 
		CurrentFrame, TotalFrames, bIsOutsideBounds, bLooping);
	
	if (bIsOutsideBounds && bLooping)
	{
		CurrentFrame = 0;
		UE_LOG(LogAnimatedTexture, Log, TEXT("Tick: Looping back to frame 0"));
	}

	FrameTime += DeltaTime * PlayRate;
	const float FrameDelaySec = Decoder->GetNextFrameDelay(CurrentFrame);
	
	if (FrameTime < FrameDelaySec)
	{
		return;
	}

	UE_LOG(LogAnimatedTexture, Verbose, TEXT("Tick: Advancing frame. FrameTime=%.4f, FrameDelay=%.4f, CurrentFrame=%d -> %d"), 
		FrameTime, FrameDelaySec, CurrentFrame, CurrentFrame + 1);

	FrameTime = 0;

	RenderFrameToTexture();

	CurrentFrame++;
}

UAnimatedTexture2D* UAnimatedTexture2D::Create(int32 InSizeX, int32 InSizeY, const FAnimatedTexture2DCreateInfo& InCreateInfo)
{
	EPixelFormat DesiredFormat = EPixelFormat(InCreateInfo.Format);
	if (InSizeX > 0 && InSizeY > 0)
	{
		UAnimatedTexture2D* NewTexture = NewObject<UAnimatedTexture2D>(
			(UObject*)GetTransientPackage(),
			MakeUniqueObjectName((UObject*)GetTransientPackage(), UAnimatedTexture2D::StaticClass()),
			RF_Public | RF_Transient
		);

		check(IsValid(NewTexture));
		{
			NewTexture->Filter = InCreateInfo.Filter;
			NewTexture->SamplerAddressMode = InCreateInfo.SamplerAddressMode;
			NewTexture->SRGB = InCreateInfo.bSRGB;

			// Disable compression
			NewTexture->CompressionSettings = TC_Default;
			if (InCreateInfo.bIsResolveTarget)
			{
				NewTexture->bNoTiling = false;
			}
			else
			{
				// Untiled format
				NewTexture->bNoTiling = true;
			}

			NewTexture->Init(InSizeX, InSizeY, DesiredFormat, InCreateInfo.bIsResolveTarget);
		}
		return NewTexture;
	}
	else
	{
		UE_LOG(LogAnimatedTexture, Warning, TEXT("Invalid parameters specified for UTexture2DDynamic::Create()"));
		return NULL;
	}
}

void UAnimatedTexture2D::SetDecoder(TUniquePtr<IGIFLoader> DecoderState)
{
	FScopeLock ResultsLock(&ResultsMutex);

	Decoder = MoveTemp(DecoderState);
	FirstFrameCache.Reset();

	if (!Decoder.IsValid())
	{
		UE_LOG(LogAnimatedTexture, Error, TEXT("SetDecoder: Decoder is invalid after move!"));
		return;
	}

	UE_LOG(LogAnimatedTexture, Log, TEXT("SetDecoder: Decoder set successfully. TotalFrames=%d, Size=%dx%d"),
		Decoder->GetTotalFrames(), Decoder->GetWidth(), Decoder->GetHeight());

	// Grab the seed frame now, on the game thread. InitRHI() runs on the render thread and
	// must not read the decoder's frame buffer directly — while streaming, that buffer is
	// reused and rewritten by every GetNextFrame() call from Tick().
	const int64 FrameBytes = (int64)Decoder->GetWidth() * Decoder->GetHeight() * sizeof(FColor);
	const FColor* FrameData = (FrameBytes > 0) ? Decoder->GetNextFrame(0) : nullptr;

	if (FrameData != nullptr)
	{
		FirstFrameCache.SetNumUninitialized((int32)FrameBytes);
		FMemory::Memcpy(FirstFrameCache.GetData(), FrameData, FrameBytes);
	}
	else
	{
		UE_LOG(LogAnimatedTexture, Warning, TEXT("SetDecoder: Could not read the first frame. Texture will be created without initial data."));
	}
}

void UAnimatedTexture2D::RenderFrameToTexture()
{
	FScopeLock ResultsLock(&ResultsMutex);

	if (!Decoder.IsValid())
	{
		UE_LOG(LogAnimatedTexture, Error, TEXT("RenderFrameToTexture: Decoder is null!"));
		return;
	}

	const int32 TotalFrames = Decoder->GetTotalFrames();
	if (TotalFrames <= 0)
	{
		return;
	}

	// Clamp frame index to valid range
	if (CurrentFrame < 0 || CurrentFrame >= TotalFrames)
	{
		if (bLooping)
		{
			CurrentFrame = 0;
		}
		else
		{
			CurrentFrame = FMath::Clamp(CurrentFrame, 0, TotalFrames - 1);
		}
	}

	FTextureResource* RHIResource = GetResource();
	const FColor* FrameData = Decoder->GetNextFrame(CurrentFrame);

	UE_LOG(LogAnimatedTexture, Verbose, TEXT("RenderFrameToTexture: Frame=%d, RHIResource=%p, RawData=%p"),
		CurrentFrame, RHIResource, FrameData);

	if (!RHIResource)
	{
		UE_LOG(LogAnimatedTexture, Error, TEXT("RenderFrameToTexture: RHIResource is null!"));
		return;
	}

	// Validate that FrameData points into the decoder's actual texture data, not the
	// single-pixel &FColor::Black fallback which would cause a buffer overread
	if (!FrameData)
	{
		UE_LOG(LogAnimatedTexture, Error, TEXT("RenderFrameToTexture: RawData is null for frame %d!"), CurrentFrame);
		return;
	}

	// Copy frame data so the render command owns its own buffer.
	// This prevents dangling pointer crashes if the decoder is destroyed before the
	// render command executes on the render thread.
	// Single frame only — SizeX * SizeY * 4 is well within int32 for any GPU-supported texture size.
	const int32 FrameSize = SizeX * SizeY * (int32)sizeof(FColor);
	TSharedPtr<TArray<uint8>, ESPMode::ThreadSafe> FrameBuffer = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
	FrameBuffer->SetNumUninitialized(FrameSize);
	FMemory::Memcpy(FrameBuffer->GetData(), FrameData, FrameSize);

	ENQUEUE_RENDER_COMMAND(AnimTexture2D_RenderFrame)(
		[RHIResource, FrameBuffer](FRHICommandListImmediate& RHICmdList)
		{
			if (!RHIResource || !RHIResource->TextureRHI)
				return;

			FTexture2DRHIRef Texture2DRHI = RHIResource->TextureRHI->GetTexture2D();
			if (!Texture2DRHI)
				return;

			uint32 TexWidth = Texture2DRHI->GetSizeX();
			uint32 TexHeight = Texture2DRHI->GetSizeY();
			uint32 SrcPitch = TexWidth * sizeof(FColor);

			FUpdateTextureRegion2D Region;
			Region.SrcX = Region.SrcY = Region.DestX = Region.DestY = 0;
			Region.Width = TexWidth;
			Region.Height = TexHeight;

			RHIUpdateTexture2D(Texture2DRHI, 0, Region, SrcPitch, FrameBuffer->GetData());
		}
	);
}

void UAnimatedTexture2D::Play()
{
	bPlaying = true;
	UE_LOG(LogAnimatedTexture, Log, TEXT("Play: Started playback. Decoder valid=%d, CurrentFrame=%d"), 
		Decoder.IsValid(), CurrentFrame);
}

void UAnimatedTexture2D::PlayFromStart()
{
	FrameTime = 0;
	FrameDelay = 0;
	bPlaying = true;
	CurrentFrame = 0;
	UE_LOG(LogAnimatedTexture, Log, TEXT("PlayFromStart: Started playback from beginning. Decoder valid=%d"), 
		Decoder.IsValid());
}

void UAnimatedTexture2D::StepForward()
{
	if (!Decoder.IsValid()) return;
	CurrentFrame++;
	if (CurrentFrame >= Decoder->GetTotalFrames())
	{
		CurrentFrame = bLooping ? 0 : Decoder->GetTotalFrames() - 1;
	}
	RenderFrameToTexture();
}

void UAnimatedTexture2D::StepBackward()
{
	if (!Decoder.IsValid()) return;
	CurrentFrame--;
	if (CurrentFrame < 0)
	{
		CurrentFrame = bLooping ? Decoder->GetTotalFrames() - 1 : 0;
	}
	RenderFrameToTexture();
}

void UAnimatedTexture2D::GotoFrame(const int32 NewFrame)
{
	if (!Decoder.IsValid()) return;
	const int32 TotalFrames = Decoder->GetTotalFrames();
	CurrentFrame = FMath::Clamp(NewFrame, 0, TotalFrames > 0 ? TotalFrames - 1 : 0);
	RenderFrameToTexture();
}


void UAnimatedTexture2D::Stop()
{
	bPlaying = false;
}

void UAnimatedTexture2D::Init(int32 InSizeX, int32 InSizeY, EPixelFormat InFormat/*=2*/, bool InIsResolveTarget/*=false*/)
{
	FScopeLock ResultsLock(&ResultsMutex);

	SizeX = InSizeX;
	SizeY = InSizeY;
	Format = (EPixelFormat)InFormat;
	NumMips = 1;
	bIsResolveTarget = InIsResolveTarget;

	UE_LOG(LogAnimatedTexture, Log, TEXT("Init: Size=%dx%d, Format=%d, bIsResolveTarget=%d, Decoder valid=%d"),
		SizeX, SizeY, (int32)Format, bIsResolveTarget, Decoder.IsValid());

	// Do NOT call UpdateResource() here — Decoder is not set yet.
	// UpdateResource() is called later from CreateTextureOnGameThread() after SetDecoder().
}

const uint8* UAnimatedTexture2D::GetFirstFrameData() const
{
	// Served from the cache filled in SetDecoder() — see FirstFrameCache for why the
	// render thread must not reach into the decoder itself.
	if (FirstFrameCache.Num() != (int32)GetFrameSize())
	{
		UE_LOG(LogAnimatedTexture, Warning, TEXT("GetFirstFrameData: Seed frame is %d bytes but the texture needs %u. Returning nullptr."),
			FirstFrameCache.Num(), GetFrameSize());
		return nullptr;
	}

	return FirstFrameCache.GetData();
}

uint32 UAnimatedTexture2D::GetFrameSize() const
{
	return CalculateImageBytes(SizeX, SizeY, 1, Format);
}
