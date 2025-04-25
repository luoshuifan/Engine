// Copyright Epic Games, Inc. All Rights Reserved.

#include "RCExternalTexture.h"

#include "DeviceProfiles/DeviceProfile.h"
#include "DeviceProfiles/DeviceProfileManager.h"
#include "ImageCoreUtils.h"
#include "ImageUtils.h"
#include "TextureResource.h"
#include "UObject/Package.h"

namespace UE::RCExternalTexture
{
	struct FResourceCacheEntry
	{
		FRCExternalTextureInfo CachedInfo;
		FTextureRHIRef TextureRHI;
		FSamplerStateRHIRef SamplerStateRHI;

		FResourceCacheEntry() = default;

		FResourceCacheEntry(const FRCExternalTextureInfo& InCachedInfo)
			: CachedInfo(InCachedInfo) {}
	};

	/**
	 * This class implements a simple cache for the RHI resources.
	 * It allows multiple instance of an external texture to use the same RHI resources.
	 */
	class FResourceCache
	{
	public:
		TSharedPtr<FResourceCacheEntry> Find(const FString& InPath) const
		{
			FScopeLock ScopeLock(&EntriesSection);
			const TWeakPtr<FResourceCacheEntry>* Found = EntriesWeak.Find(InPath);
			return Found ? (*Found).Pin() : nullptr;
		}

		void Add(const FString& InPath, const TSharedRef<FResourceCacheEntry>& InEntry)
		{
			FScopeLock ScopeLock(&EntriesSection);
			EntriesWeak.Add(InPath, InEntry);
		}

		// Todo: Multi-gpu support.
		// Do we need a cache per GPU or is this internally handled by RHI?
		static TSharedPtr<FResourceCache> Get()
		{
			static TSharedRef<FResourceCache> Instance = MakeShared<FResourceCache>();
			return Instance;
		}
	
	private:
		mutable FCriticalSection EntriesSection;
		TMap<FString, TWeakPtr<FResourceCacheEntry>> EntriesWeak;
	};
}

/**
 * This implementation of texture resource, while based on Texture2DDynamic
 * also support sharing the RHI resource for all textures of the same path
 * so the actual texture is loaded only once and reused in each RCExternalTexture instance.
 */
class FRCExternalTextureResource : public FTextureResource
{
public:
	using FResourceCacheEntry = UE::RCExternalTexture::FResourceCacheEntry; 
	using FResourceCache = UE::RCExternalTexture::FResourceCache; 
	
	FRCExternalTextureResource(URCExternalTexture* InOwner, const FString& InPath)
		: RCTextureOwner(InOwner)
		, Path(InPath)
	{
		
	}

	virtual uint32 GetSizeX() const override
	{
		return RCTextureOwner->SizeX;
	}

	virtual uint32 GetSizeY() const override
	{
		return RCTextureOwner->SizeY;
	}

	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		const TSharedPtr<FResourceCache> RHICache = FResourceCache::Get();
		CacheEntry = RHICache->Find(Path);

		// Fast path: texture already cached.
		if (CacheEntry && CacheEntry->TextureRHI.IsValid())
		{
			UpdateRHIFromCacheEntry();
			return;
		}

		// >>> Copied from FTexture2DDynamicResource::InitRHI
		// Create the sampler state RHI resource.
		ESamplerAddressMode SamplerAddressMode = RCTextureOwner->SamplerAddressMode;
		FSamplerStateInitializerRHI SamplerStateInitializer
		(
			(ESamplerFilter)UDeviceProfileManager::Get().GetActiveProfile()->GetTextureLODSettings()->GetSamplerFilter( RCTextureOwner ),
			SamplerAddressMode,
			SamplerAddressMode,
			SamplerAddressMode
		);
		SamplerStateRHI = GetOrCreateSamplerState( SamplerStateInitializer );

		FString Name = RCTextureOwner->GetName();

		FRHITextureCreateDesc Desc =
			FRHITextureCreateDesc::Create2D(*Name, GetSizeX(), GetSizeY(), RCTextureOwner->Format)
			.SetNumMips(RCTextureOwner->NumMips);

		if (RCTextureOwner->bIsResolveTarget)
		{
			Desc.AddFlags(ETextureCreateFlags::ResolveTargetable);
		}
		else if (RCTextureOwner->SRGB)
		{
			Desc.AddFlags(ETextureCreateFlags::SRGB);
		}

		if (RCTextureOwner->bNoTiling)
		{
			Desc.AddFlags(ETextureCreateFlags::NoTiling);
		}

		Texture2DRHI = RHICreateTexture(Desc);

		TextureRHI = Texture2DRHI;
		TextureRHI->SetName(RCTextureOwner->GetFName());
		RHIUpdateTextureReference(RCTextureOwner->TextureReference.TextureReferenceRHI,TextureRHI);
		// <<< Copied from FTexture2DDynamicResource::InitRHI

		UpdateCacheEntry();
	}
	
	virtual void ReleaseRHI() override
	{
		CacheEntry.Reset();

		// >>> Copied from FTexture2DDynamicResource::ReleaseRHI
		RHIUpdateTextureReference(RCTextureOwner->TextureReference.TextureReferenceRHI, nullptr);
		FTextureResource::ReleaseRHI();
		Texture2DRHI.SafeRelease();
	}

	void SetCacheEntry(const FString& InNewPath, const TSharedPtr<FResourceCacheEntry>& InNewEntry)
	{
		ReleaseRHI();

		Path = InNewPath;
		CacheEntry = InNewEntry;
		UpdateRHIFromCacheEntry();
	}

	void UpdateRHIFromCacheEntry()
	{
		if (CacheEntry)
		{
			TextureRHI = CacheEntry->TextureRHI;
			SamplerStateRHI = CacheEntry->SamplerStateRHI;
			RHIUpdateTextureReference(RCTextureOwner->TextureReference.TextureReferenceRHI,TextureRHI);
		}
	}

	void UpdateCacheEntry()
	{
		if (CacheEntry)
		{
			CacheEntry->TextureRHI = GetTextureRHI();
			CacheEntry->SamplerStateRHI = SamplerStateRHI;
		}
	}

#if !UE_SERVER
	// >>> Copied from FTexture2DDynamicResource::WriteRawToTexture_RenderThread
	void WriteRawToTexture_RenderThread(TArrayView64<const uint8> RawData)
	{
		check(IsInRenderingThread());

		const uint32 Width = Texture2DRHI->GetSizeX();
		const uint32 Height = Texture2DRHI->GetSizeY();

		// Prevent from locking texture if the source is empty or of size 0 or if source is is too small.
		const uint64 SourceSize = RawData.Num();
		if (!ensure(Width * Height != 0 && SourceSize >= Width * Height && Texture2DRHI->GetDesc().Format == EPixelFormat::PF_B8G8R8A8))
		{
			return;
		}

		uint32 DestStride = 0;
		uint8* DestData = reinterpret_cast<uint8*>(RHILockTexture2D(Texture2DRHI, 0, RLM_WriteOnly, DestStride, false, false));


		for (uint32 y = 0; y < Height; y++)
		{
			const uint64 CurrentLine = ((uint64)Height - 1 - y);
			uint8* DestPtr = &DestData[CurrentLine * DestStride];

			const FColor* SrcPtr = &((FColor*)(RawData.GetData()))[CurrentLine * Width];
			for (uint32 x = 0; x < Width; x++)
			{
				*DestPtr++ = SrcPtr->B;
				*DestPtr++ = SrcPtr->G;
				*DestPtr++ = SrcPtr->R;
				*DestPtr++ = SrcPtr->A;
				SrcPtr++;
			}
		}

		RHIUnlockTexture2D(Texture2DRHI, 0, false, false);
	}
#endif

	URCExternalTexture* RCTextureOwner;
	FTextureRHIRef Texture2DRHI;
	FString Path;
	TSharedPtr<FResourceCacheEntry> CacheEntry;
};

void URCExternalTexture::PostLoad()
{
	SizeX = CachedInfo.SizeX;
	SizeY = CachedInfo.SizeY;
	Format = CachedInfo.Format;
	NumMips = 1;
	bIsResolveTarget = false;

	Super::PostLoad(); // will call UpdateResource	

	if (!Path.IsEmpty())
	{
		LoadFromPath(Path);
	}
}

FTextureResource* URCExternalTexture::CreateResource()
{
	return new FRCExternalTextureResource(this, Path);
}

void URCExternalTexture::LoadFromPath(const FString& InPath)
{
	using namespace UE::RCExternalTexture;
	// Fast path: Check if texture is already in cache.
	if (const TSharedPtr<FResourceCacheEntry> ExistingEntry = FResourceCache::Get()->Find(InPath))
	{
		Path = InPath;
		CachedInfo = ExistingEntry->CachedInfo;

		// Only recreate resource if something changed.
		if (SizeX != CachedInfo.SizeX || SizeY != CachedInfo.SizeY || Format != CachedInfo.Format || !GetResource())
		{
			Init(CachedInfo.SizeX, CachedInfo.SizeY, CachedInfo.Format);	// Calls UpdateResource
		}
		
		if (FRCExternalTextureResource* TextureResource = static_cast<FRCExternalTextureResource*>(GetResource()))
		{
			ENQUEUE_RENDER_COMMAND(FUpdateSharedTextureEntry)(
				[TextureResource, ExistingEntry, NewPath=Path](FRHICommandListImmediate& RHICmdList)
				{
					TextureResource->SetCacheEntry(NewPath, ExistingEntry);
				});
		}
		return;
	}


	// This will be loaded on the main thread for now. Most likely causing game thread spike.
	// As an optimization for loading, we could try to hook this up to a streaming manager.
	// The RCExternalTexture is typically embedded in a level, can get access with GetTypedOuter<ULevel>.
	
	FImage Image;	
	if (FImageUtils::LoadImage(*InPath, Image))
	{
		// Todo: keep file time and hash to detect content change and reload if necessary.
		Path = InPath;
		
		SRGB = Image.GetGammaSpace() == EGammaSpace::Linear ? 0 : 1;
		
		ERawImageFormat::Type PixelFormatRawFormat;
		const EPixelFormat PixelFormat = FImageCoreUtils::GetPixelFormatForRawImageFormat(Image.Format,&PixelFormatRawFormat);
		CachedInfo.Set(Image.GetWidth(), Image.GetHeight(), PixelFormat);
		
		if (SizeX != Image.GetWidth() || SizeY != Image.GetHeight() || Format != PixelFormat || !GetResource())
		{
			Init(Image.GetWidth(), Image.GetHeight(), PixelFormat);
		}

		if (FRCExternalTextureResource* TextureResource = static_cast<FRCExternalTextureResource*>(GetResource()))
		{
			TSharedRef<FResourceCacheEntry> NewEntry = MakeShared<FResourceCacheEntry>(CachedInfo);
			FResourceCache::Get()->Add(Path, NewEntry);
			
			ENQUEUE_RENDER_COMMAND(FWriteRawDataToTexture)(
				[TextureResource, NewEntry, NewPath=Path, NewImage = MoveTemp(Image)](FRHICommandListImmediate& InRHICmdList)
				{
#if !UE_SERVER
					// Todo: support mipmaps. some format like dds can have mipmaps (but not supported by FImageUtils for now).
					TextureResource->WriteRawToTexture_RenderThread(NewImage.RawData);
#endif
					TextureResource->Path = NewPath;
					TextureResource->CacheEntry = NewEntry;
					TextureResource->UpdateCacheEntry();
				});
		}
		return;
	}
	
	// Fallback: Allocate texture with no data.
	// Todo: should we create a checkerboard pattern?
	Path.Reset();
	Init(CachedInfo.SizeX, CachedInfo.SizeY, CachedInfo.Format);
}

URCExternalTexture* URCExternalTexture::Create(const FTexture2DDynamicCreateInfo& InCreateInfo)
{
	URCExternalTexture* NewTexture = NewObject<URCExternalTexture>(GetTransientPackage(), NAME_None, RF_Transient);
	if (NewTexture)
	{
		NewTexture->Filter = InCreateInfo.Filter;
		NewTexture->SamplerAddressMode = InCreateInfo.SamplerAddressMode;
		NewTexture->SRGB = InCreateInfo.bSRGB;

		// Disable compression
		NewTexture->CompressionSettings = TC_Default;
#if WITH_EDITORONLY_DATA
		NewTexture->CompressionNone = true;
		NewTexture->MipGenSettings = TMGS_NoMipmaps;
		NewTexture->CompressionNoAlpha = true;
		NewTexture->DeferCompression = false;
#endif // #if WITH_EDITORONLY_DATA
		if (InCreateInfo.bIsResolveTarget)
		{
			NewTexture->bNoTiling = false;
		}
		else
		{
			// Untiled format
			NewTexture->bNoTiling = true;
		}
		NewTexture->bIsResolveTarget = InCreateInfo.bIsResolveTarget;
	}
	return NewTexture;
}

URCExternalTexture* URCExternalTexture::Create(const FString& InPath, const FTexture2DDynamicCreateInfo& InCreateInfo)
{
	URCExternalTexture* NewTexture = Create(InCreateInfo);
	if (NewTexture)
	{
		NewTexture->LoadFromPath(InPath);
	}
	return NewTexture;
}
