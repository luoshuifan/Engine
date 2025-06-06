// Copyright Epic Games, Inc. All Rights Reserved.

#include "ExrImgMediaReader.h"
#include "ExrImgMediaReaderGpu.h"

#if IMGMEDIA_EXR_SUPPORTED_PLATFORM

#include "Async/Async.h"
#include "Misc/Paths.h"
#include "ExrReaderGpu.h"
#include "OpenExrWrapper.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "HardwareInfo.h"
#include "IImgMediaModule.h"
#include "Loader/ImgMediaLoader.h"
#include "Assets/ImgMediaMipMapInfo.h"

DECLARE_MEMORY_STAT(TEXT("EXR Reader Pool Memory."), STAT_ExrMediaReaderPoolMem, STATGROUP_ImgMediaPlugin);

static TAutoConsoleVariable<bool> CVarEnableUncompressedExrGpuReader(
	TEXT("r.ExrReadAndProcessOnGPU"),
	true,
	TEXT("Allows reading of Large Uncompressed EXR files directly into Structured Buffer.\n")
	TEXT("and be processed on GPU\n"));


/* FExrImgMediaReader structors
 *****************************************************************************/

FExrImgMediaReader::FExrImgMediaReader(const TSharedRef<FImgMediaLoader, ESPMode::ThreadSafe>& InLoader)
	: LoaderPtr(InLoader)
{
	const UImgMediaSettings* Settings = GetDefault<UImgMediaSettings>();
	
	FOpenExr::SetGlobalThreadCount(Settings->ExrDecoderThreads == 0
		? FPlatformMisc::NumberOfCoresIncludingHyperthreads()
		: Settings->ExrDecoderThreads);
}

FExrImgMediaReader::~FExrImgMediaReader()
{
	FScopeLock RegionScopeLock(&CanceledFramesCriticalSection);
	CanceledFrames.Reset();
}

/* FExrImgMediaReader interface
 *****************************************************************************/

bool FExrImgMediaReader::GetFrameInfo(const FString& ImagePath, FImgMediaFrameInfo& OutInfo)
{
	return GetInfo(ImagePath, OutInfo);
}

FExrImgMediaReader::EReadResult FExrImgMediaReader::ReadTiles
	( uint16* Buffer
	, int64 BufferSize
	, const FString& ImagePath
	, const TArray<FIntRect>& TileRegions
	, FSampleConverterParameters& ConverterParams
	, const int32 CurrentMipLevel
	, TArray<UE::Math::TIntPoint<int64>>& OutBufferRegionsToCopy)
{
#if defined(PLATFORM_WINDOWS) && PLATFORM_WINDOWS
	EReadResult Result = Success;

	FExrReader ChunkReader;
	int MipLevelDiv = 1 << CurrentMipLevel;

	FIntPoint MipResolution = ConverterParams.FullResolution / MipLevelDiv;

	FIntPoint DimensionInTiles
		( FMath::CeilToInt(float(MipResolution.X) / ConverterParams.TileDimWithBorders.X)
		, FMath::CeilToInt(float(MipResolution.Y) / ConverterParams.TileDimWithBorders.Y));

	TArray<int32> NumTilesPerLevel;
	int32 NumMipLevels = ConverterParams.bMipsInSeparateFiles ? 1 : ConverterParams.NumMipLevels;

	FExrReader::CalculateTileOffsets(
		NumTilesPerLevel,
		ConverterParams.TileInfoPerMipLevel,
		ConverterParams.FullResolution,
		ConverterParams.TileDimWithBorders,
		NumMipLevels,
		ConverterParams.PixelSize);

	{
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("FExrImgMediaReader_ReadTiles_OpenFile");
		if (!ChunkReader.OpenExrAndPrepareForPixelReading(ImagePath, NumTilesPerLevel))
		{
			return Fail;
		}
	}
	{
		int64 CurrentBufferPos = 0;
		TRACE_CPUPROFILER_EVENT_SCOPE_STR("FExrImgMediaReader_ReadTiles_ReadTiles");
		for (const FIntRect& RawTileRegion : TileRegions)
		{
			// This clamp is to make sure that tile region is not out of bounds in case the region wasn't calculated incorrectly for some reason.
			const FIntRect& TileRegion = FIntRect(
				FIntPoint(FMath::Max(RawTileRegion.Min.X, 0), FMath::Max(RawTileRegion.Min.Y, 0)),
				FIntPoint(FMath::Min(RawTileRegion.Max.X, DimensionInTiles.X), FMath::Min(RawTileRegion.Max.Y, DimensionInTiles.Y)));

			for (int32 TileRow = TileRegion.Min.Y; TileRow < TileRegion.Max.Y; TileRow++)
			{
				// Check to see if the frame was canceled.
				{
					FScopeLock RegionScopeLock(&CanceledFramesCriticalSection);
					if (CanceledFrames.Remove(ConverterParams.FrameId) > 0)
					{
						UE_LOG(LogImgMedia, Verbose, TEXT("Reader %p: Canceling Frame %i At tile row # %i"), this, ConverterParams.FrameId, TileRow);
						Result = Cancelled;
						break;
					}
				}

				const uint16 Padding = FExrReader::TILE_PADDING;
				const int StartTileIndex = TileRow * DimensionInTiles.X + TileRegion.Min.X;

				bool bLastTile = TileRow + 1 == DimensionInTiles.Y && TileRegion.Max.X == DimensionInTiles.X;
				const int EndTileIndex = TileRow * DimensionInTiles.X + TileRegion.Max.X;

				int MipLevel = ConverterParams.bMipsInSeparateFiles ? 0 : CurrentMipLevel;
				ChunkReader.SeekTileWithinFile(StartTileIndex, MipLevel, CurrentBufferPos);
				int64 ByteOffsetStart = 0;
				int64 ByteOffsetEnd = 0;

				if (!ChunkReader.GetByteOffsetForTile(StartTileIndex, MipLevel, ByteOffsetStart)
					|| !ChunkReader.GetByteOffsetForTile(EndTileIndex, MipLevel, ByteOffsetEnd))
				{
					Result = Fail;
					break;
				}

				// If this is the last tile, make sure chunk reader reads till the end of the buffer we write into.
				int64 ByteChunkToRead = bLastTile
					? FMath::Min(ByteOffsetEnd - ByteOffsetStart, BufferSize - CurrentBufferPos)
					: (ByteOffsetEnd - ByteOffsetStart);

				if (!ChunkReader.ReadExrImageChunk(reinterpret_cast<char*>(Buffer) + CurrentBufferPos, ByteChunkToRead))
				{
					Result = Fail;
					break;
				}
				OutBufferRegionsToCopy.Add({ CurrentBufferPos, ByteChunkToRead });
			}

		}
	}
	{
		TRACE_CPUPROFILER_EVENT_SCOPE_TEXT(*FString::Printf(TEXT("FExrImgMediaReaderGpu_CloseFile %d"), CurrentMipLevel));

		if (!ChunkReader.CloseExrFile())
		{
			return Fail;
		}

		return Result;
	}
#else
	return Fail;
#endif
}

bool FExrImgMediaReader::ReadFrame(int32 FrameId, const TMap<int32, FImgMediaTileSelection>& InMipTiles, TSharedPtr<FImgMediaFrame, ESPMode::ThreadSafe> OutFrame)
{
	TSharedPtr<FImgMediaLoader, ESPMode::ThreadSafe> Loader = LoaderPtr.Pin();
	if (Loader.IsValid() == false)
	{
		return false;
	}
	
	// Get tile info.
	int32 NumTilesX = Loader->GetNumTilesX();
	int32 NumTilesY = Loader->GetNumTilesY();
	bool bHasTiles = (NumTilesX * NumTilesY) > 1;

	int32 BytesPerPixelPerChannel = sizeof(uint16);
	int32 NumChannels = 4;
	int32 BytesPerPixel = BytesPerPixelPerChannel * NumChannels;
	FImgMediaFrameInfo FrameInfo;

	// Do we already have our buffer?
	if (OutFrame->Data.IsValid() == false)
	{
		// Nope. Create it.
		const FString& LargestImage = Loader->GetImagePath(FrameId, 0);
		if (!GetInfo(LargestImage, FrameInfo, OutFrame))
		{
			return false;
		}

		const FIntPoint& Dim = FrameInfo.Dim;

		if (Dim.GetMin() <= 0)
		{
			return false;
		}

		// allocate frame buffer
		// FRgbaInputFile loads any exr as rgba 16 bit per channel.
		// UncompressedSize is used for cache, therefore it needs to be set to the actual size of the buffer.
		FrameInfo.UncompressedSize = GetMipBufferTotalSize(Dim, Loader->GetNumMipLevels() > 1);
		SIZE_T BufferSize = FrameInfo.UncompressedSize;
		void* Buffer = FMemory::Malloc(BufferSize, PLATFORM_CACHE_LINE_SIZE);

		auto BufferDeleter = [BufferSize](void* ObjectToDelete) {
#if USE_IMGMEDIA_DEALLOC_POOL
			if (FQueuedThreadPool* ImgMediaThreadPoolSlow = GetImgMediaThreadPoolSlow())
			{
				// free buffers on the thread pool, because memory allocators may perform
				// expensive operations, such as filling the memory with debug values
				TFunction<void()> FreeBufferTask = [ObjectToDelete]()
				{
					FMemory::Free(ObjectToDelete);
				};
				AsyncPool(*ImgMediaThreadPoolSlow, FreeBufferTask);
			}
			else
			{
				FMemory::Free(ObjectToDelete);
			}
#else
			FMemory::Free(ObjectToDelete);
#endif
		};
		
		// The EXR RGBA interface only outputs RGBA data.
		OutFrame->Format = EMediaTextureSampleFormat::FloatRGBA;
		OutFrame->Data = MakeShareable(Buffer, MoveTemp(BufferDeleter));
		OutFrame->MipTilesPresent.Reset();
		OutFrame->Stride = FrameInfo.Dim.X * BytesPerPixel;
	}
	else
	{
		FrameInfo = OutFrame->GetInfo();
	}

	// Loop over all mips.
	uint8* MipDataPtr = (uint8*)(OutFrame->Data.Get());
	FIntPoint Dim = FrameInfo.Dim;
	
	int32 NumMipLevels = Loader->GetNumMipLevels();
	for (int32 CurrentMipLevel = 0; CurrentMipLevel < NumMipLevels; ++CurrentMipLevel)
	{
		if (InMipTiles.Contains(CurrentMipLevel))
		{
			const FImgMediaTileSelection& CurrentTileSelection = InMipTiles[CurrentMipLevel];

			const int MipLevelDiv = 1 << CurrentMipLevel;

			// Avoid reads if the cached frame already contains the current tiles for this mip level.
			const bool ReadThisMip = !OutFrame->MipTilesPresent.ContainsTiles(CurrentMipLevel, CurrentTileSelection);
			if (ReadThisMip)
			{
				FString Image = Loader->GetImagePath(FrameId, CurrentMipLevel);
				FString BaseImage;

				if (bHasTiles)
				{
					UE_LOG(LogImgMedia, Error, TEXT("Non-GPU reader doesn't currently support natively-tiled EXR file %s"), *Image);
				}
				else
				{
					// Get for our frame/mip level.
					FRgbaInputFile InputFile(Image, 2);
					if (InputFile.HasInputFile())
					{
						// read frame data
						InputFile.SetFrameBuffer(MipDataPtr, Dim);
						InputFile.ReadPixels(0, Dim.Y - 1);
						OutFrame->MipTilesPresent.Emplace(CurrentMipLevel, CurrentTileSelection);
						OutFrame->NumTilesRead++;
					}
					else
					{
						UE_LOG(LogImgMedia, Error, TEXT("Could not load %s"), *Image);
					}
				}
			}
		}

		// Next level.
		MipDataPtr += Dim.X * Dim.Y * BytesPerPixel;
		Dim /= 2;
	}

	return true;
}

void FExrImgMediaReader::CancelFrame(int32 FrameNumber)
{
	UE_LOG(LogImgMedia, Verbose, TEXT("Reader %p: Canceling Frame. %i"), this, FrameNumber);
	FScopeLock RegionScopeLock(&CanceledFramesCriticalSection);
	CanceledFrames.Add(FrameNumber);
}

void FExrImgMediaReader::UncancelFrame(int32 FrameNumber)
{
	FScopeLock RegionScopeLock(&CanceledFramesCriticalSection);
	CanceledFrames.Remove(FrameNumber);
}

/** Gets reader type (GPU vs CPU) depending on size of EXR and its compression. */
TSharedPtr<IImgMediaReader, ESPMode::ThreadSafe> FExrImgMediaReader::GetReader(const TSharedRef <FImgMediaLoader, ESPMode::ThreadSafe>& InLoader, FString FirstImageInSequencePath)
{
#if defined(PLATFORM_WINDOWS) && PLATFORM_WINDOWS
	if (!FPaths::FileExists(FirstImageInSequencePath))
	{
		return nullptr;
	}
	
	FImgMediaFrameInfo Info;
	if (!GetInfo(FirstImageInSequencePath, Info))
	{
		return MakeShareable(new FExrImgMediaReader(InLoader));
	}

	const bool bIsDevelopmentCustomFormat = Info.FormatName.Equals(TEXT("EXR CUSTOM"));
	if (bIsDevelopmentCustomFormat)
	{
		UE_LOG(LogImgMedia, Error, TEXT("Support for the (development-only) custom EXR format has been removed."));
		return nullptr;
	}

	const bool bIsOptimizedForGpu = Info.FormatName.Equals(TEXT("EXR GPU"));
	// Check GetCompressionName of OpenExrWrapper for other compression names.
	// todo: Add and test Vulkan support
	if (GDynamicRHI && GDynamicRHI->GetInterfaceType() == ERHIInterfaceType::D3D12
		&& Info.CompressionName == "Uncompressed" 
		&& CVarEnableUncompressedExrGpuReader.GetValueOnAnyThread()
		&& bIsOptimizedForGpu
		)
	{
		TSharedRef<FExrImgMediaReaderGpu, ESPMode::ThreadSafe> GpuReader = 
			MakeShared<FExrImgMediaReaderGpu, ESPMode::ThreadSafe>(InLoader);
		return GpuReader;
	}
#endif

	return MakeShareable(new FExrImgMediaReader(InLoader));
}

/* FExrImgMediaReader implementation
 *****************************************************************************/

bool FExrImgMediaReader::GetInfo(const FString& FilePath, FImgMediaFrameInfo& OutInfo, const TSharedPtr<FImgMediaFrame, ESPMode::ThreadSafe>& OutFrame)
{
	FOpenExrHeaderReader HeaderReader(FilePath);
	if (HeaderReader.HasInputFile() == false)
	{
		return false;
	}

	OutInfo.CompressionName = HeaderReader.GetCompressionName();
	OutInfo.Dim = HeaderReader.GetDataWindow();
	OutInfo.FrameRate = HeaderReader.GetFrameRate(ImgMedia::DefaultFrameRate);
	OutInfo.Srgb = false;
	OutInfo.UncompressedSize = HeaderReader.GetUncompressedSize();
	OutInfo.NumChannels = HeaderReader.GetNumChannels();
	OutInfo.NumMipLevels = 1;

	int32 CustomFormat = 0;
	HeaderReader.GetIntAttribute(IImgMediaModule::CustomFormatAttributeName.Resolve().ToString(), CustomFormat);
	bool bIsCustomFormat = CustomFormat > 0;

	if (bIsCustomFormat)
	{
		OutInfo.FormatName = TEXT("EXR CUSTOM");
	}
	else
	{
		// Can GPU reader be utilized to read this EXR file.
		if (HeaderReader.IsOptimizedForGpu())
		{
			OutInfo.FormatName = TEXT("EXR GPU");
		}
		else
		{
			OutInfo.FormatName = TEXT("EXR");
		}

		OutInfo.bHasTiles = HeaderReader.GetTileSize(OutInfo.TileDimensions);
		OutInfo.TileBorder = 0;
	}

	if (OutInfo.bHasTiles)
	{
		OutInfo.NumTiles =
		{FMath::CeilToInt(float(OutInfo.Dim.X) / (OutInfo.TileDimensions.X + OutInfo.TileBorder * 2))
		, FMath::CeilToInt(float(OutInfo.Dim.Y) / (OutInfo.TileDimensions.Y + OutInfo.TileBorder * 2))};
		if (HeaderReader.ContainsMips())
		{
			OutInfo.NumMipLevels = HeaderReader.CalculateNumMipLevels(OutInfo.NumTiles);

			SIZE_T SizeMip0 = OutInfo.UncompressedSize;
			for (int32 Level = 1; Level < OutInfo.NumMipLevels; Level++)
			{
				OutInfo.UncompressedSize += SizeMip0 >> (2 * Level);
			}
		}
	}
	else
	{
		OutInfo.TileDimensions = OutInfo.Dim;
		OutInfo.NumTiles = FIntPoint(1, 1);
	}

	if (OutFrame.IsValid())
	{
		OutFrame->SetInfo(OutInfo);
	}

	return (OutInfo.UncompressedSize > 0) && (OutInfo.Dim.GetMin() > 0);
}

SIZE_T FExrImgMediaReader::GetMipBufferTotalSize(FIntPoint Dim, bool bInHasMips)
{
	SIZE_T Size = 0;
	if (bInHasMips)
	{
		Size = ((Dim.X * Dim.Y * 4) / 3) * sizeof(uint16) * 4;
	}
	else
	{
		Size = (Dim.X * Dim.Y ) * sizeof(uint16) * 4;
	}

	return Size;
}

#endif //IMGMEDIA_EXR_SUPPORTED_PLATFORM
