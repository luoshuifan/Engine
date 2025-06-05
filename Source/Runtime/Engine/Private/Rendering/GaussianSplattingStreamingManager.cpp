// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianSplattingStreamingManager.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "Async/ParallelFor.h"
#include "RenderUtils.h"
#include "Rendering/GaussianSplattingResources.h"
#include "ShaderCompilerCore.h"
#include "Stats/StatsTrace.h"
#include "RHIGPUReadback.h"
#include "HAL/PlatformFileManager.h"
#include "ShaderPermutationUtils.h"

#if WITH_EDITOR
#include "DerivedDataCache.h"
#include "DerivedDataRequestOwner.h"
using namespace UE::DerivedData;
#endif

#define MAX_LEGACY_REQUESTS_PER_UPDATE		32u		// Legacy IO requests are slow and cause lots of bubbles, so we NEED to limit them.

#define LRU_INDEX_MASK						0x7FFFFFFFu
#define LRU_FLAG_REFERENCED_THIS_UPDATE		0x80000000u

DECLARE_STATS_GROUP_SORTBYNAME(	TEXT("GaussianSplattingStreaming"),					STATGROUP_GaussianSplattingStreaming,								STATCAT_Advanced);

DECLARE_DWORD_COUNTER_STAT(		TEXT("Readback Size"),						STAT_GaussianSplattingStreaming41_ReadbackSize,					STATGROUP_GaussianSplattingStreaming);
DECLARE_DWORD_COUNTER_STAT(		TEXT("Readback Buffer Size"),				STAT_GaussianSplattingStreaming42_ReadbackBufferSize,				STATGROUP_GaussianSplattingStreaming);


DECLARE_CYCLE_STAT(				TEXT("BeginAsyncUpdate"),					STAT_GaussianSplattingStreaming_BeginAsyncUpdate,					STATGROUP_GaussianSplattingStreaming);
DECLARE_CYCLE_STAT(				TEXT("AsyncUpdate"),						STAT_GaussianSplattingStreaming_AsyncUpdate,						STATGROUP_GaussianSplattingStreaming);
DECLARE_CYCLE_STAT(				TEXT("ProcessRequests"),					STAT_GaussianSplattingStreaming_ProcessRequests,					STATGROUP_GaussianSplattingStreaming);
DECLARE_CYCLE_STAT(				TEXT("InstallReadyPages"),					STAT_GaussianSplattingStreaming_InstallReadyPages,					STATGROUP_GaussianSplattingStreaming);
DECLARE_CYCLE_STAT(				TEXT("UploadTask"),							STAT_GaussianSplattingStreaming_UploadTask,						STATGROUP_GaussianSplattingStreaming);
DECLARE_CYCLE_STAT(				TEXT("ApplyFixup"),							STAT_GaussianSplattingStreaming_ApplyFixup,						STATGROUP_GaussianSplattingStreaming);

DECLARE_CYCLE_STAT(				TEXT("EndAsyncUpdate"),						STAT_GaussianSplattingStreaming_EndAsyncUpdate,					STATGROUP_GaussianSplattingStreaming);
DECLARE_CYCLE_STAT(				TEXT("AddParentRequests"),					STAT_GaussianSplattingStreaming_AddParentRequests,					STATGROUP_GaussianSplattingStreaming);
DECLARE_CYCLE_STAT(				TEXT("AddParentRegisteredRequests"),		STAT_GaussianSplattingStreaming_AddParentRegisteredRequests,		STATGROUP_GaussianSplattingStreaming);
DECLARE_CYCLE_STAT(				TEXT("AddParentNewRequests"),				STAT_GaussianSplattingStreaming_AddParentNewRequests,				STATGROUP_GaussianSplattingStreaming);
DECLARE_CYCLE_STAT(				TEXT("ClearReferencedArray"),				STAT_GaussianSplattingStreaming_ClearReferencedArray,				STATGROUP_GaussianSplattingStreaming);

DECLARE_CYCLE_STAT(				TEXT("CompactLRU"),							STAT_GaussianSplattingStreaming_CompactLRU,						STATGROUP_GaussianSplattingStreaming);
DECLARE_CYCLE_STAT(				TEXT("UpdateLRU"),							STAT_GaussianSplattingStreaming_UpdateLRU,							STATGROUP_GaussianSplattingStreaming);
DECLARE_CYCLE_STAT(				TEXT("ProcessGPURequests"),					STAT_GaussianSplattingStreaming_ProcessGPURequests,				STATGROUP_GaussianSplattingStreaming);
DECLARE_CYCLE_STAT(				TEXT("SelectHighestPriority"),				STAT_GaussianSplattingStreaming_SelectHighestPriority,				STATGROUP_GaussianSplattingStreaming);

DECLARE_CYCLE_STAT(				TEXT("Heapify"),							STAT_GaussianSplattingStreaming_Heapify,							STATGROUP_GaussianSplattingStreaming);
DECLARE_CYCLE_STAT(				TEXT("VerifyLRU"),							STAT_GaussianSplattingStreaming_VerifyLRU,							STATGROUP_GaussianSplattingStreaming);

static int32 GGaussianSplattingStreamingMaxPendingPages = 128;
static FAutoConsoleVariableRef CVarGaussianSplattingStreamingMaxPendingPages(
	TEXT("r.GaussianSplatting.Streaming.MaxPendingPages"),
	GGaussianSplattingStreamingMaxPendingPages,
	TEXT("Maximum number of pages that can be pending for installation."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
);

static int32 GGaussianSplattingStreamingMaxPageInstallsPerFrame = 128;
static FAutoConsoleVariableRef CVarGaussianSplattingStreamingMaxPageInstallsPerFrame(
	TEXT("r.GaussianSplatting.Streaming.MaxPageInstallsPerFrame"),
	GGaussianSplattingStreamingMaxPageInstallsPerFrame,
	TEXT("Maximum number of pages that can be installed per frame. Limiting this can limit the overhead of streaming."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
);

static int32 GGaussianSplattingStreamingGPURequestsBufferMinSize = 128 * 1024;
static FAutoConsoleVariableRef CVarGaussianSplattingStreamingGPURequestsBufferMinSize(
	TEXT("r.GaussianSplatting.Streaming.GPURequestsBufferMinSize"),
	GGaussianSplattingStreamingGPURequestsBufferMinSize,
	TEXT("The minimum number of elements in the buffer used for GPU feedback.")
	TEXT("Setting Min=Max disables any dynamic buffer size adjustment."),
	ECVF_RenderThreadSafe
);

static int32 GGaussianSplattingStreamingGPURequestsBufferMaxSize = 1024 * 1024;
static FAutoConsoleVariableRef CVarGaussianSplattingStreamingGPURequestsBufferMaxSize(
	TEXT("r.GaussianSplatting.Streaming.GPURequestsBufferMaxSize"),
	GGaussianSplattingStreamingGPURequestsBufferMaxSize,
	TEXT("The maximum number of elements in the buffer used for GPU feedback.")
	TEXT("Setting Min=Max disables any dynamic buffer size adjustment."),
	ECVF_RenderThreadSafe
);

static int32 GGaussianSplattingStreamingPoolSize = 512;
static FAutoConsoleVariableRef CVarGaussianSplattingStreamingPoolSize(
	TEXT("r.GaussianSplatting.Streaming.StreamingPoolSize"),
	GGaussianSplattingStreamingPoolSize,
	TEXT("Size of streaming pool in MB. Does not include memory used for root pages.")
	TEXT("Be careful with setting this close to the GPU resource size limit (typically 2-4GB) as root pages are allocated from the same physical buffer."),
	ECVF_RenderThreadSafe
);

static int32 GGaussianSplattingStreamingNumInitialRootPages = 2048;
static FAutoConsoleVariableRef CVarGaussianSplattingStreamingNumInitialRootPages(
	TEXT("r.GaussianSplatting.Streaming.NumInitialRootPages"),
	GGaussianSplattingStreamingNumInitialRootPages,
	TEXT("Number of root pages in initial allocation."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
);

static float GGaussianSplattingStreamingBandwidthLimit = -1.0f;
static FAutoConsoleVariableRef CVarNaniteStreamingBandwidthLimit(
	TEXT("r.GaussianSplatting.Streaming.BandwidthLimit" ),
	GGaussianSplattingStreamingBandwidthLimit,
	TEXT("Streaming bandwidth limit in megabytes per second. Negatives values are interpreted as unlimited. "),
	ECVF_RenderThreadSafe
);

namespace GS
{
	#define MAX_RUNTIME_RESOURCE_VERSIONS_BITS	8								// Just needs to be large enough to cover maximum number of in-flight versions
	#define MAX_RUNTIME_RESOURCE_VERSIONS_MASK	((1 << MAX_RUNTIME_RESOURCE_VERSIONS_BITS) - 1)	

	
// Round up to smallest value greater than or equal to x of the form k*2^s where k < 2^NumSignificantBits.
// This is the same as RoundUpToPowerOfTwo when NumSignificantBits=1.
// For larger values of NumSignificantBits each po2 bucket is subdivided into 2^(NumSignificantBits-1) linear steps.
// This gives more steps while still maintaining an overall exponential structure and keeps numbers nice and round (in the po2 sense).

// Example:
// Representable values for different values of NumSignificantBits.
// 1: ..., 16, 32, 64, 128, 256, 512, ...
// 2: ..., 16, 24, 32,  48,  64,  96, ...
// 3: ..., 16, 20, 24,  28,  32,  40, ...
static uint32 RoundUpToSignificantBits(uint32 x, uint32 NumSignificantBits)
{
	check(NumSignificantBits <= 32);

	const int32_t Shift = FMath::Max((int32)FMath::CeilLogTwo(x) - (int32)NumSignificantBits, 0);
	const uint32 Mask = (1u << Shift) - 1u;
	return (x + Mask) & ~Mask;
}

	static uint32 GetMaxPagePoolSizeInMB()
	{
		return 2048;
	}

	static uint32 GPUPageIndexToGPUOffset(uint32 MaxStreamingPages, uint32 PageIndex)
	{
		return (FMath::Min(PageIndex, MaxStreamingPages) << GAUSSIANSPLATTING_STREAMING_PAGE_GPU_SIZE_BITS) + ((uint32)FMath::Max((int32)PageIndex - (int32)MaxStreamingPages, 0) << GAUSSIANSPLATTING_ROOT_PAGE_GPU_SIZE_BITS);
	}

	class FTranscodePageToGPU_CS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FTranscodePageToGPU_CS);
		SHADER_USE_PARAMETER_STRUCT(FTranscodePageToGPU_CS, FGlobalShader);

		class FTranscodePassDim : SHADER_PERMUTATION_SPARSE_INT("GAUSSIANSPLATTING_TRANSCODE_PASS", GAUSSIANSPLATTING_TRANSCODE_PASS_INDEPENDENT, GAUSSIANSPLATTING_TRANSCODE_PASS_PARENT_DEPENDENT);
		class FGroupSizeDim : SHADER_PERMUTATION_SPARSE_INT("GROUP_SIZE", 4, 8, 16, 32, 64, 128);
		using FPermutationDomain = TShaderPermutationDomain<FTranscodePassDim, FGroupSizeDim>;

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32,													StartClusterIndex)
			SHADER_PARAMETER(uint32,													NumClusters)
			SHADER_PARAMETER(uint32,													ZeroUniform)
			SHADER_PARAMETER(FIntVector4,												PageConstants)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPackedClusterInstallInfo>,ClusterInstallInfoBuffer)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>,						PageDependenciesBuffer)
			SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer,							SrcPageBuffer)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer,						DstPageBuffer)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			FPermutationDomain PermutationVector(Parameters.PermutationId);

			if (!UE::ShaderPermutationUtils::ShouldCompileWithWaveSize(Parameters, PermutationVector.Get<FGroupSizeDim>()))
			{
				return false;
			}
		
			return true;
		}

		static EShaderPermutationPrecacheRequest ShouldPrecachePermutation(const FShaderPermutationParameters& Parameters)
		{
			FPermutationDomain PermutationVector(Parameters.PermutationId);

			if (!UE::ShaderPermutationUtils::ShouldPrecacheWithWaveSize(Parameters, PermutationVector.Get<FGroupSizeDim>()))
			{
				return EShaderPermutationPrecacheRequest::NotUsed;
			}

			return FGlobalShader::ShouldPrecachePermutation(Parameters);
		}

		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
		{
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
			OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);

			OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
			OutEnvironment.CompilerFlags.Add(CFLAG_HLSL2021);
			OutEnvironment.CompilerFlags.Add(CFLAG_WarningsAsErrors);
		}
	};
IMPLEMENT_GLOBAL_SHADER(FTranscodePageToGPU_CS, "/Engine/Private/GaussianSplatting/GaussianSplattingTranscode.usf", "TranscodePageToGPU", SF_Compute);

	class FClearStreamingRequestCount_CS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FClearStreamingRequestCount_CS);
		SHADER_USE_PARAMETER_STRUCT(FClearStreamingRequestCount_CS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FStreamingRequest>, OutStreamingRequests)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return true;
		}
	};
IMPLEMENT_GLOBAL_SHADER(FClearStreamingRequestCount_CS, "/Engine/Private/GaussianSplatting/GaussianSplattingStreaming.usf", "ClearStreamingRequestCount", SF_Compute);

	class FUpdateClusterLeafFlags_CS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FUpdateClusterLeafFlags_CS);
		SHADER_USE_PARAMETER_STRUCT(FUpdateClusterLeafFlags_CS, FGlobalShader);

		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER(uint32, NumClusterUpdates)
			SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, PackedClusterUpdates)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, ClusterPageBuffer)
		END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return true;
		}
	};

	IMPLEMENT_GLOBAL_SHADER(FUpdateClusterLeafFlags_CS, "/Engine/Private/GaussianSplatting/GaussianSplattingStreaming.usf", "UpdateClusterLeafFlags", SF_Compute);

	static void AddPass_ClearStreamingRequestCount(FRDGBuilder& GraphBuilder, FRDGBufferUAVRef BufferUAVRef)
	{
		// Need to always clear streaming requests on all GPUs.  We sometimes write to streaming request buffers on a mix of
		// GPU masks (shadow rendering on all GPUs, other passes on a single GPU), and we need to make sure all are clear
		// when they get used again.
		RDG_GPU_MASK_SCOPE(GraphBuilder, FRHIGPUMask::All());

		FClearStreamingRequestCount_CS::FParameters* PassParameters = GraphBuilder.AllocParameters<FClearStreamingRequestCount_CS::FParameters>();
		PassParameters->OutStreamingRequests = BufferUAVRef;

		auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FClearStreamingRequestCount_CS>();
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("ClearStreamingRequestCount"),
			ComputeShader,
			PassParameters,
			FIntVector(1, 1, 1)
		);
	}

	static void AddPass_UpdateClusterLeafFlags(FRDGBuilder& GraphBuilder, FRDGBufferUAVRef ClusterPageBufferUAV, const TArray<uint32>& PackedUpdates)
	{
		const uint32 NumClusterUpdates = PackedUpdates.Num();
		if (NumClusterUpdates == 0u)
		{
			return;
		}

		const uint32 NumUpdatesBufferElements = FMath::RoundUpToPowerOfTwo(NumClusterUpdates);
		FRDGBufferRef UpdatesBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("GS.PackedClusterUpdateBuffer"), PackedUpdates.GetTypeSize(), 
																NumUpdatesBufferElements, PackedUpdates.GetData(), PackedUpdates.Num() * PackedUpdates.GetTypeSize());

		FUpdateClusterLeafFlags_CS::FParameters* PassParameters = GraphBuilder.AllocParameters<FUpdateClusterLeafFlags_CS::FParameters>();
		PassParameters->NumClusterUpdates = NumClusterUpdates;
		PassParameters->PackedClusterUpdates = GraphBuilder.CreateSRV(UpdatesBuffer);
		PassParameters->ClusterPageBuffer = ClusterPageBufferUAV;

		auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FUpdateClusterLeafFlags_CS>();
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("UpdateClusterLeafFlags"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(NumClusterUpdates, 64)
		);
	}

	class FHierarchyDepthManager
	{
	public:
		FHierarchyDepthManager(uint32 MaxDepth)
		{
			DepthHistogram.SetNumZeroed(MaxDepth + 1);
		}

		void Add(uint32 Depth)
		{
			++DepthHistogram[Depth];
		}

		void Remove(uint32 Depth)
		{
			uint32& Count = DepthHistogram[Depth];
			check(Count > 0u);
			--Count;
		}

		uint32 CalculateNumLevels() const
		{
			for (int32 Depth = uint32(DepthHistogram.Num() - 1); Depth >= 0; --Depth)
			{
				if (DepthHistogram[Depth] != 0u)
				{
					return uint32(Depth) + 1u;
				}
			}
			return 0u;
		}

	private:
		TArray<uint32> DepthHistogram;
	};


	struct FPackedClusterInstallInfo
	{
		uint32 LocalPageIndex_LocalClusterIndex;
		uint32 SrcPageOffset;
		uint32 DstPageOffset;
		uint32 PageDependenciesOffset;
	};

	class FStreamingPageUploader
	{
		struct FAddedPageInfo
		{
			FPageKey GPUPageKey;
			uint32 SrcPageOffset;
			uint32 DstPageOffset;
			uint32 GaussiansOffset;
			uint32 NumGaussians;
			uint32 InstallPassIndex;
		};

		struct FPassInfo
		{
			uint32 NumPages;
			uint32 NumGaussians;
		};

	public:
		FStreamingPageUploader()
		{
			ResetState();
		}

		void Init(FRDGBuilder& GraphBuilder, uint32 InMaxPages, uint32 InMaxPageBytes, uint32 InMaxStreamingPages)
		{
			ResetState();
			MaxPages = InMaxPages;
			MaxPageBytes = MaxPageBytes;
			MaxStreamingPages = InMaxStreamingPages;

			if (IsRegistered(GraphBuilder, PageUploadBuffer))
			{
				ClusterInstallInfoUploadBuffer = nullptr;
				PageUploadBuffer = nullptr;
			}

			const uint32 PageAllocationSize = FMath::RoundUpToPowerOfTwo(MaxPageBytes);

			FRDGBufferDesc BufferDes = FRDGBufferDesc::CreateByteAddressUploadDesc(PageAllocationSize);
			AllocatePooledBuffer(BufferDes, PageUploadBuffer, TEXT("GaussianSplatting.PageUploadBuffer"));
			
			PageDataPtr = (uint8*)GraphBuilder.RHICmdList.LockBuffer(PageUploadBuffer->GetRHI(), 0, MaxPageBytes, RLM_WriteOnly);
		}

		uint8* Add_GetRef(uint32 PageSize, uint32 NumGaussians, uint32 DstPageOffset, const FPageKey& GPUPageKey)
		{
			check(IsAligned(PageSize, 4));
			check(IsAligned(DstPageOffset, 4));

			const uint32 PageIndex = AddedPageInfos.Num();

			check(PageIndex < MaxPages);
			check(NextPageByteOffset + PageSize < MaxPageBytes);

			FAddedPageInfo& Info = AddedPageInfos.AddDefaulted_GetRef();
			Info.GPUPageKey = GPUPageKey;
			Info.SrcPageOffset = NextPageByteOffset;
			Info.DstPageOffset = DstPageOffset;
			Info.NumGaussians = NumGaussians;
			Info.GaussiansOffset = NextClusterIndex;
			Info.InstallPassIndex = 0xFFFFFFFFu;

			uint8* ResultPtr = PageDataPtr + NextPageByteOffset;
			NextPageByteOffset += PageSize;
			NextClusterIndex += NumGaussians;

			return ResultPtr;
		}

		void ResourceUploadTo(FRDGBuilder& GraphBuilder, FRDGBuffer* DstBuffer)
		{
			RDG_EVENT_SCOPE(GraphBuilder, "GaussianSplatting::Transcode");
			GraphBuilder.RHICmdList.UnlockBuffer(PageUploadBuffer->GetRHI());

			const uint32 NumPages = AddedPageInfos.Num();
			if (NumPages == 0)
			{
				ResetState();
				return;
			}

			const uint32 ClusterInstallInfoAllocationSize = FMath::RoundUpToPowerOfTwo(NextClusterIndex * sizeof(FPackedClusterInstallInfo));
			if (ClusterInstallInfoAllocationSize > TryGetSize(ClusterInstallInfoUploadBuffer))
			{
				const uint32 BytesPerElement = sizeof(FPackedClusterInstallInfo);
				
				AllocatePooledBuffer(FRDGBufferDesc::CreateStructuredUploadDesc(BytesPerElement, ClusterInstallInfoAllocationSize / BytesPerElement), ClusterInstallInfoUploadBuffer, TEXT("GaussianSplatting.ClusterInstallInfoUploadBuffer"));
			}

			FPackedClusterInstallInfo* ClusterInstallInfoPtr = (FPackedClusterInstallInfo*)GraphBuilder.RHICmdList.LockBuffer(ClusterInstallInfoUploadBuffer->GetRHI(), 0, ClusterInstallInfoAllocationSize, RLM_WriteOnly);

			check(PassInfos.Num() == 0);
			uint32 NumRemainingPages = NumPages;
			uint32 NumCluster = 0;
			uint32 NextSortedPageIndex = 0;
			
			while (NumRemainingPages > 0)
			{
				const uint32 CurrentPassIndex = PassInfos.Num();
				uint32 NumPassPages = 0;
				uint32 NumPassCluster = 0;
				
				for (FAddedPageInfo& PageInfo : AddedPageInfos)
				{
					if(PageInfo.InstallPassIndex < CurrentPassIndex)
						continue;

					bool bMissingDependency = false;
					if (!bMissingDependency)
					{
						PageInfo.InstallPassIndex = CurrentPassIndex;

						check(PageInfo.NumGaussians < GAUSSIANSPLATTING_MAX_CLUSTERS_PER_PAGE);
						for (uint32 i = 0; i < PageInfo.NumGaussians; ++i)
						{
							ClusterInstallInfoPtr->LocalPageIndex_LocalClusterIndex = (NextSortedPageIndex << GAUSSIANSPLATTING_MAX_CLUSTERS_PER_PAGE_BITS) | i;
							ClusterInstallInfoPtr->SrcPageOffset = PageInfo.SrcPageOffset;
							ClusterInstallInfoPtr->DstPageOffset = PageInfo.DstPageOffset;
							ClusterInstallInfoPtr->PageDependenciesOffset = 0u;
							ClusterInstallInfoPtr++;
						}
						NextSortedPageIndex++;
						NumPassPages++;
						NumPassCluster += PageInfo.NumGaussians;
					}
				}

				FPassInfo PassInfo;
				PassInfo.NumGaussians = NumPassCluster;
				PassInfo.NumPages = NumPassPages;

				PassInfos.Add(PassInfo);
				NumRemainingPages -= NumPassPages;
			}

			GraphBuilder.RHICmdList.UnlockBuffer(ClusterInstallInfoUploadBuffer->GetRHI());

			FRDGBufferSRV* PageUploadBufferSRV = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(PageUploadBuffer));
			FRDGBufferSRV* ClusterInstallInfoUploadBufferSRV = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ClusterInstallInfoUploadBuffer));
			FRDGBufferUAV* DstBufferUAV = GraphBuilder.CreateUAV(DstBuffer);

			const bool bAsyncCompute = GSupportsEfficientAsyncCompute;

			check(GRHISupportsWaveOperations);

			const uint32 PreferredGroupSize = GRHIMaximumWaveSize;

			FTranscodePageToGPU_CS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FTranscodePageToGPU_CS::FGroupSizeDim>(PreferredGroupSize);

			{
				FTranscodePageToGPU_CS::FParameters* Parameters = GraphBuilder.AllocParameters<FTranscodePageToGPU_CS::FParameters>();
				Parameters->ClusterInstallInfoBuffer = ClusterInstallInfoUploadBufferSRV;
				Parameters->DstPageBuffer = DstBufferUAV;
				Parameters->SrcPageBuffer = PageUploadBufferSRV;
				Parameters->StartClusterIndex = 0;
				Parameters->NumClusters = NextClusterIndex;
				Parameters->ZeroUniform = 0;
				Parameters->PageConstants = FIntVector4(0, MaxStreamingPages, 0, 0);

				PermutationVector.Set<FTranscodePageToGPU_CS::FTranscodePassDim>(GAUSSIANSPLATTING_TRANSCODE_PASS_INDEPENDENT);
				auto ComputeShader = GetGlobalShaderMap(GMaxRHIShaderPlatform)->GetShader<FTranscodePageToGPU_CS>(PermutationVector);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("TranscodePageToGPU Independent (ClusterCount: %u, GroupSize: %u)", NextClusterIndex, PreferredGroupSize),
					bAsyncCompute ? ERDGPassFlags::AsyncCompute : ERDGPassFlags::Compute,
					ComputeShader,
					Parameters,
					FComputeShaderUtils::GetGroupCountWrapped(NextClusterIndex));
			}
			Release();
		}

		void Release()
		{
			ClusterInstallInfoUploadBuffer.SafeRelease();
			PageUploadBuffer.SafeRelease();
			ResetState();
		}


		
	private:
		TRefCountPtr<FRDGPooledBuffer> ClusterInstallInfoUploadBuffer;
		TRefCountPtr<FRDGPooledBuffer> PageUploadBuffer;
		uint8* PageDataPtr;
		uint32 MaxPages;
		uint32 MaxPageBytes;
		uint32 MaxStreamingPages;
		uint32 NextPageByteOffset;
		uint32 NextClusterIndex;
		TArray<FAddedPageInfo> AddedPageInfos;
		TMap<FPageKey, uint32> GPUPageKeyToAddedIndex;
		TArray<FPassInfo> PassInfos;

		void ResetState()
		{
			PageDataPtr = nullptr;
			MaxPages = 0;
			MaxPageBytes = 0;
			MaxStreamingPages = 0;
			NextPageByteOffset = 0;
			NextClusterIndex = 0;
			GPUPageKeyToAddedIndex.Reset();
		}
	};

	struct FGPUStreamingRequest
	{
		uint32 RuntimeResourceID_Magic;
		uint32 PageIndex_NumPages_Magic;
		uint32 Priority_Magic;
	};

	class FReadbackManager
	{
	public:
		FReadbackManager(uint32 InNumBuffers) : NumBuffers(InNumBuffers)
		{
			ReadbackBuffers.SetNum(NumBuffers);
		}
			
		void PrepareRequestBuffer(FRDGBuilder& GraphBuilder)
		{
			const uint32 BufferSize = RoundUpToSignificantBits(BufferSizeManager.GetSize(), 2);
		
			if (!RequestsBuffer.IsValid() || RequestsBuffer->Desc.NumElements != BufferSize)
			{
				FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUStreamingRequest), BufferSize);
				Desc.Usage = EBufferUsageFlags(Desc.Usage | BUF_SourceCopy);
				FRDGBufferRef RequestsBufferRef = GraphBuilder.CreateBuffer(Desc, TEXT("GaussianSplatting.StreamingRequests"));

				AddPass_ClearStreamingRequestCount(GraphBuilder, GraphBuilder.CreateUAV(RequestsBufferRef));

				RequestsBuffer = GraphBuilder.ConvertToExternalBuffer(RequestsBufferRef);
			}
		}

		FGPUStreamingRequest* LockLatest(uint32& OutNumStreamingRequests)
		{
			OutNumStreamingRequests = 0;
			check(LastReadbackBuffer == nullptr);

			while (NumPendingBuffers > 0)
			{
				if (ReadbackBuffers[NextReadBufferIndex].Buffer->IsReady())
				{
					LastReadbackBuffer = &ReadbackBuffers[NextReadBufferIndex];
					NextReadBufferIndex = (NextReadBufferIndex + 1u) % NumBuffers;
					NumPendingBuffers--;
				}
				else
					break;
			}

			if (LastReadbackBuffer)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(LockBuffer);
				uint32* Ptr = (uint32*)LastReadbackBuffer->Buffer->Lock(LastReadbackBuffer->NumElements * sizeof(FGPUStreamingRequest));
				check(LastReadbackBuffer->NumElements > 0);

				const uint32 NumRequests = Ptr[0];
				BufferSizeManager.Update(NumRequests);

				OutNumStreamingRequests = FMath::Min(NumRequests, LastReadbackBuffer->NumElements - 1u);

				return (FGPUStreamingRequest*)Ptr + 1;
			}

			return nullptr;
		}

		void QueueReadback(FRDGBuilder& GraphBuilder)
		{
			if(NumPendingBuffers == NumBuffers)
				return;

			const uint32 WriteBufferIndex = (NextReadBufferIndex + NumPendingBuffers) % NumBuffers;
			FReadbackBuffer& ReadbackBuffer = ReadbackBuffers[WriteBufferIndex];

			if (ReadbackBuffer.Buffer == nullptr)
			{
				ReadbackBuffer.Buffer = MakeUnique<FRHIGPUBufferReadback>(TEXT("GaussianSplatting.StreamingRequestReadback"));
			}
			ReadbackBuffer.NumElements = RequestsBuffer->Desc.NumElements;

			FRDGBufferRef RDGRequestsBuffer = GraphBuilder.RegisterExternalBuffer(RequestsBuffer);

			AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("Readback"), RDGRequestsBuffer, 
				[&GPUReadback = ReadbackBuffer.Buffer, RDGRequestsBuffer](FRDGAsyncTask, FRHICommandList& RHICmdList)
				{
					GPUReadback->EnqueueCopy(RHICmdList, RDGRequestsBuffer->GetRHI(), 0u);
				});

			AddPass_ClearStreamingRequestCount(GraphBuilder, GraphBuilder.CreateUAV(RDGRequestsBuffer));

			NumPendingBuffers++;
			BufferVersion++;
		}

		void Unlock()
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(UnlockBuffer);
			check(LastReadbackBuffer);
			LastReadbackBuffer->Buffer->Unlock();
			LastReadbackBuffer = nullptr;
		}

		FRDGBuffer* GetStreamingRequestsBuffer(FRDGBuilder& GraphBuilder) const
		{
			return GraphBuilder.RegisterExternalBuffer(RequestsBuffer);
		}

		uint32 GetBufferVersion() const
		{
			return BufferVersion;
		}

	private:
		class FBufferSizeManager
		{
		public:
			FBufferSizeManager() :
				CurrentSize((float)GGaussianSplattingStreamingGPURequestsBufferMinSize)
			{
			}

			void Update(uint32 NumRequests)
			{
				const uint32 Target = uint32(NumRequests * 1.25f);

				const bool bOverBudget = Target > CurrentSize;
				const bool bUnderBudget = NumRequests < CurrentSize * 0.5f;

				OverBudgetCounter = bOverBudget ? (OverBudgetCounter + 1u) : 0u;
				UnderBudgetCounter = bUnderBudget ? (UnderBudgetCounter + 1u) : 0u;

				if (OverBudgetCounter > 2u)
				{
					CurrentSize = FMath::Max(Target, CurrentSize);
				}
				else if (UnderBudgetCounter > 30u)
				{
					CurrentSize *= 0.98f;
				}

				const int32 LimitMinSize = 4u * 1024u;
				const int32 LimitMaxSize = 1024u * 1024u;
				const int32 MinSize = FMath::Clamp(GGaussianSplattingStreamingGPURequestsBufferMinSize, LimitMinSize, LimitMaxSize);
				const int32 MaxSize = FMath::Clamp(GGaussianSplattingStreamingGPURequestsBufferMaxSize, MinSize, LimitMaxSize);

				CurrentSize = FMath::Clamp(CurrentSize, MinSize, MaxSize);
			}


			uint32 GetSize() const
			{
				return uint32(CurrentSize);
			}
		private:
			float CurrentSize = 0;
			uint32 OverBudgetCounter = 0;
			uint32 UnderBudgetCounter = 0;
		};

		struct FReadbackBuffer
		{
			TUniquePtr<class FRHIGPUBufferReadback> Buffer;
			uint32 NumElements = 0u;
		};

		TRefCountPtr<FRDGPooledBuffer> RequestsBuffer;
		TArray<FReadbackBuffer> ReadbackBuffers;

		FReadbackBuffer* LastReadbackBuffer;
		uint32 NumBuffers = 0;
		uint32 NumPendingBuffers = 0;
		uint32 NextReadBufferIndex = 0;
		uint32 BufferVersion = 0;

		FBufferSizeManager BufferSizeManager;
	};

	class FRingBufferAllocator
	{
	public:
		FRingBufferAllocator(uint32 Size) :
			BufferSize(Size)
		{
			Reset();
		}

		void Reset()
		{
			ReadOffset = 0;
			WriteOffset = 0;
		}

		bool TryAllocate(uint32 Size, uint32& AllocatedOffset)
		{
			if (WriteOffset < ReadOffset)
			{
				if (Size + 1u > ReadOffset - WriteOffset)
				{
					return false;
				}
			}
			else
			{
				if (Size + (ReadOffset == 0u ? 1u : 0u) > BufferSize - WriteOffset)
				{
					if (Size + 1u > ReadOffset)
					{
						return false;
					}
					WriteOffset = 0u;
				}
			}

			AllocatedOffset = WriteOffset;
			WriteOffset += Size;
			check(AllocatedOffset + Size <= BufferSize);
			return true;
		}

		void Free(uint32 Size)
		{
			const uint32 Next = ReadOffset + Size;
			ReadOffset = (Next <= BufferSize) ? Next : Size;
		}

	private:
		uint32 BufferSize;
		uint32 ReadOffset;
		uint32 WriteOffset;
	};


	FStreamingManager::FStreamingManager()
	{

	}

	void FStreamingManager::ResetStreamingStateCPU()
	{
		RegisteredVirtualPages.Empty();
		RegisteredVirtualPages.SetNum(VirtualPageAllocator.GetMaxSize());

		RegisteredPages.Empty();
		RegisteredPages.SetNum(MaxStreamingPages);

		RegisteredPageIndexToLRU.Empty();
		RegisteredPageIndexToLRU.SetNum(MaxStreamingPages);

		LRUToRegisteredPageIndex.Empty();
		LRUToRegisteredPageIndex.SetNum(MaxStreamingPages);
		for (uint32 i = 0; i < MaxStreamingPages; ++i)
		{
			RegisteredPageIndexToLRU[i] = i;
			LRUToRegisteredPageIndex[i] = i;
		}

	}

	FResources* FStreamingManager::GetResources(uint32 RuntimeResourceID)
	{
		if (RuntimeResourceID != INDEX_NONE)
		{
			const uint32 RootPageIndex = RuntimeResourceID & GAUSSIANSPLATTING_MAX_GPU_PAGES_MASK;
			if (RootPageIndex < (uint32)RootPageInfos.Num())
			{
				FRootPageInfo& RootPageInfo = RootPageInfos[RootPageIndex];
				if (RootPageInfo.RuntimeResourceID == RuntimeResourceID)
				{
					return RootPageInfo.Resource;
				}
			}

		}

		return nullptr;
	}

	FStreamingManager::FRootPageInfo* FStreamingManager::GetRootPage(uint32 RuntimeResourceID)
	{
		if (RuntimeResourceID != INDEX_NONE)
		{
			const uint32 RootPageIndex = RuntimeResourceID & GAUSSIANSPLATTING_MAX_GPU_PAGES_MASK;
			if (RootPageIndex < uint32(RootPageInfos.Num()))
			{
				FRootPageInfo& RootPageInfo = RootPageInfos[RootPageIndex];
				if (RootPageInfo.RuntimeResourceID == RuntimeResourceID)
				{
					return &RootPageInfo;
				}
			}
		}

		return nullptr;
	}

	void FStreamingManager::InitRHI(FRHICommandListBase& RHICmdList)
	{
		LLM_SCOPE_BYTAG(GaussianSplatting);

		UpdatePageConfiguration();

		MaxPendingPages = GGaussianSplattingStreamingMaxPendingPages;
		MaxPageInstallsPerUpdate = uint32(FMath::Max(GGaussianSplattingStreamingMaxPageInstallsPerFrame, GGaussianSplattingStreamingMaxPendingPages));

		ResetStreamingStateCPU();

		const bool bReservedResource = GRHIGlobals.ReservedResources.Supported;
		
		FRDGBufferDesc ClusterDataBufferDesc = {};
		if (bReservedResource)
		{
			const uint64 MaxSizeInBytes = uint64(GetMaxPagePoolSizeInMB()) << 20;
			ClusterDataBufferDesc = FRDGBufferDesc::CreateByteAddressDesc(MaxSizeInBytes);
			ClusterDataBufferDesc.Usage |= EBufferUsageFlags::ReservedResource;
		}
		else
		{
			ClusterDataBufferDesc = FRDGBufferDesc::CreateByteAddressDesc(4);
		}

		Hierarchy.Allocator = FSpanAllocator(true);

		if (!bReservedResource)
		{
			ClusterPageData.Allocator = FSpanAllocator(true);
		}

		ClusterPageData.DataBuffer = AllocatePooledBuffer(ClusterDataBufferDesc, TEXT("GaussianSplatting.StreamingManager.ClusterPageData"));
		Hierarchy.DataBuffer = AllocatePooledBuffer(FRDGBufferDesc::CreateByteAddressDesc(4), TEXT("GaussianSplatting.StreamingManager.HierarchyData"));
	}

	void FStreamingManager::ReleaseRHI()
	{

	}
	
	void FStreamingManager::UpdatePageConfiguration()
	{
		const uint32 MaxPoolSizeInMB = GetMaxPagePoolSizeInMB();
		const uint32 StreamingPoolSizeInMB = GGaussianSplattingStreamingPoolSize;

		const uint32 OldMaxStreamingPages = MaxStreamingPages;
		const uint32 OldNumInitialRootPages = NumInitialRootPages;
		
		const uint64 MaxRootPoolSizeInMB = MaxPoolSizeInMB - StreamingPoolSizeInMB;
		MaxStreamingPages = uint32(uint64(StreamingPoolSizeInMB << 20) >> GAUSSIANSPLATTING_STREAMING_PAGE_GPU_SIZE_BITS);
		MaxRootPages = uint32(uint64(MaxRootPoolSizeInMB << 20) >> GAUSSIANSPLATTING_ROOT_PAGE_GPU_SIZE_BITS);

		NumInitialRootPages = GGaussianSplattingStreamingNumInitialRootPages;
		if (NumInitialRootPages > MaxRootPages)
		{
			NumInitialRootPages = MaxRootPages;
		}

		PrevNumInitialRootPages = GGaussianSplattingStreamingNumInitialRootPages;
	}


	void FStreamingManager::Add(FResources* Resources)
	{
		check(Resources != nullptr);
		check(IsInRenderingThread());
		check(!AsyncState.bUpdateActive);

		LLM_SCOPE_BYTAG(GaussianSplatting);
		if (Resources->RuntimeResourceID == INDEX_NONE)
		{
			check(Resources->RootData.Num() > 0);
			Resources->HierarchyOffset = Hierarchy.Allocator.Allocate(Resources->HierarchyNodes.Num());
			Resources->NumHierarchyNodes = Resources->HierarchyNodes.Num();
			Hierarchy.TotalUpload += Resources->NumHierarchyNodes;

			Resources->RootPageIndex = ClusterPageData.Allocator.Allocate(Resources->NumRootPages);
			
			RootPageInfos.SetNum(ClusterPageData.Allocator.GetMaxSize());
			RootPageVersions.SetNumZeroed(FMath::Max(RootPageVersions.Num(), ClusterPageData.Allocator.GetMaxSize()));

			const uint32 NumResourcePages = Resources->PageStreamingState.Num();
			const uint32 VirtualPageRangeStart = VirtualPageAllocator.Allocate(NumResourcePages);

			RegisteredVirtualPages.SetNum(VirtualPageAllocator.GetMaxSize());

			uint32 RuntimeResourceID;
			{
				uint8& RootPageNextVersion = RootPageVersions[Resources->RootPageIndex];
				
				RuntimeResourceID = (RootPageNextVersion << GAUSSIANSPLATTING_MAX_GPU_PAGES_BITS) | Resources->RootPageIndex;
				RootPageNextVersion = (RootPageNextVersion + 1u) & MAX_RUNTIME_RESOURCE_VERSIONS_MASK;
			}
			Resources->RuntimeResourceID = RuntimeResourceID;

			for (uint32 i = 0; i < Resources->NumRootPages; ++i)
			{
				FRootPageInfo& RootPageInfo = RootPageInfos[i];
				check(RootPageInfo.Resource == nullptr);
				check(RootPageInfo.RuntimeResourceID == INDEX_NONE);
				check(RootPageInfo.VirtualPageRangeStart == INDEX_NONE);
				check(RootPageInfo.NumClusters == 0);

				RootPageInfo.Resource = Resources;
				RootPageInfo.VirtualPageRangeStart = VirtualPageRangeStart + i;
				RootPageInfo.RuntimeResourceID = RuntimeResourceID;
				RootPageInfo.NumClusters = 0;
			}

			check(Resources->PersistentHash != GAUSSIANSPLATTING_INVALID_PERSISTENT_HASH);
			PersistenHashResourceMap.Add(Resources->PersistentHash, Resources);

			PendingAdds.Add(Resources);
			NumResources++;
		}
	}

	void FStreamingManager::Remove(FResources* Resources)
	{
		check(IsInRenderingThread());
		check(!AsyncState.bUpdateActive);

		LLM_SCOPE_BYTAG(GaussianSplatting);

		if (Resources->RuntimeResourceID != INDEX_NONE)
		{
			Hierarchy.Allocator.Free(Resources->HierarchyOffset, Resources->NumHierarchyNodes);
			Resources->HierarchyOffset = INDEX_NONE;

			const uint32 RootPageIndex = Resources->RootPageIndex;
			const uint32 NumRootPages = Resources->NumRootPages;
			ClusterPageData.Allocator.Free(RootPageIndex, NumRootPages);
			Resources->RootPageIndex = INDEX_NONE;

			const uint32 VirtualPageRangeStart = RootPageInfos[RootPageIndex].VirtualPageRangeStart;
			for (uint32 i = 0; i < NumRootPages; ++i)
			{
				FRootPageInfo& RootPageInfo =  RootPageInfos[RootPageIndex + i];
				RootPageInfo.Resource = nullptr;
				RootPageInfo.RuntimeResourceID = INDEX_NONE;
				RootPageInfo.VirtualPageRangeStart = INDEX_NONE;
				RootPageInfo.NumClusters = 0;
			}



		}
	}


	void FStreamingManager::BeginAsyncUpdate(FRDGBuilder& GraphBuilder)
	{
		check(IsInRenderingThread());

		LLM_SCOPE_BYTAG(GaussianSplatting);
		TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::BeginAsyncUpdate);

		RDG_EVENT_SCOPE_STAT(GraphBuilder, GaussianSplattingStreaming, "GaussianSplatting::Streaming");
		RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplattingStreaming);

		SCOPE_CYCLE_COUNTER(STAT_GaussianSplattingStreaming_BeginAsyncUpdate);

		check(!AsyncState.bUpdateActive);
		AsyncState = FAsyncState{};
		AsyncState.bUpdateActive = true;

		VirtualPageAllocator.Consolidate();
		RegisteredVirtualPages.SetNum(VirtualPageAllocator.GetMaxSize());

		FRDGBuffer* ClusterPageDataBuffer = ResizePoolAllocationIfNeeded(GraphBuilder);
		ProcessNewResources(GraphBuilder, ClusterPageDataBuffer);

		uint32 TotalPageSize;
		AsyncState.NumReadyPages = DetermineReadyPages(TotalPageSize);



		AsyncState.GPUStreamingRequestsPtr = ReadbackManager->LockLatest(AsyncState.NumGPUStreamingRequests);
		ReadbackManager->PrepareRequestBuffer(GraphBuilder);

		AsyncUpdate();
	}

	FRDGBuffer* FStreamingManager::ResizePoolAllocationIfNeeded(FRDGBuilder& GraphBuilder)
	{
		const uint32 OldMaxStreamingPool = MaxStreamingPages;

		ClusterPageData.Allocator.Consolidate();
		const uint32 NumRootPages = ClusterPageData.Allocator.GetMaxSize();
		const bool bReservedResource = EnumHasAnyFlags(ClusterPageData.DataBuffer->Desc.Usage, EBufferUsageFlags::ReservedResource);

		UpdatePageConfiguration();

		const bool bAllowGrow = true;
		const bool bIgnoreInitialRootPage = true;

		uint32 NumAllocatedRootPages = 0;
		if (bReservedResource)
		{
			const uint32 AllocationGranularityInPages = (16 << 20) / GAUSSIANSPLATTING_ROOT_PAGE_GPU_SIZE;

			NumAllocatedRootPages = bIgnoreInitialRootPage ? 0u : NumInitialRootPages;
			if (NumRootPages > NumAllocatedRootPages)
			{
				NumAllocatedRootPages = FMath::DivideAndRoundUp(NumRootPages, AllocationGranularityInPages) * AllocationGranularityInPages;
				NumAllocatedRootPages = FMath::Min(NumAllocatedRootPages, bAllowGrow ? MaxRootPages : NumInitialRootPages);
			}
		}
		else
		{
			NumAllocatedRootPages = NumInitialRootPages;
			if (NumRootPages > NumInitialRootPages)
			{
				NumAllocatedRootPages = FMath::Clamp(RoundUpToSignificantBits(NumRootPages, 2), NumInitialRootPages, MaxRootPages);
			}
		}

		const uint32 NumAllocatedPages = MaxStreamingPages + NumRootPages;
		const uint64 AllocatedPageSize = (uint64(NumRootPages) << GAUSSIANSPLATTING_ROOT_PAGE_GPU_SIZE_BITS) 
																+ (uint64(MaxStreamingPages) << GAUSSIANSPLATTING_STREAMING_PAGE_GPU_SIZE_BITS);

		FRDGBuffer* ClusterPageDataBuffer = nullptr;
		
		const bool bResetStreamingState = false;
		if (bResetStreamingState)
		{

		}
		else
		{
			ClusterPageDataBuffer = ResizeByteAddressBufferIfNeeded(GraphBuilder, ClusterPageData.DataBuffer, AllocatedPageSize, TEXT("GaussianSplatting.StreamingManager.ClusterPageData"));
		}

		RootPageInfos.SetNum(NumAllocatedRootPages);

		return ClusterPageDataBuffer;
	}

	void FStreamingManager::AddPendingGPURequests()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AddPendingGPURequests);
		SCOPE_CYCLE_COUNTER(STAT_GaussianSplattingStreaming_ProcessRequests);

		const uint32 NumStreamingRequests = AsyncState.NumGPUStreamingRequests;
		if(NumStreamingRequests == 0)
			return;

		const FGPUStreamingRequest* StreamingRequestsPtr = AsyncState.GPUStreamingRequestsPtr;
		const FGPUStreamingRequest* StreamingRequestsEndPtr = StreamingRequestsPtr + NumStreamingRequests;

		do
		{
			const FGPUStreamingRequest& GPURequest = *StreamingRequestsPtr;

			const uint32 RuntimeResourceID = GPURequest.RuntimeResourceID_Magic;
			const uint32 NumPages = GPURequest.PageIndex_NumPages_Magic & GAUSSIANSPLATTING_MAX_GROUP_PARTS_MASK;
			const uint32 FirstPageIndex = GPURequest.PageIndex_NumPages_Magic >> GAUSSIANSPLATTING_MAX_GROUP_PARTS_MASK;
			const uint32 Priority = GPURequest.Priority_Magic;

			check(Priority != 0 && Priority <= GAUSSIANSPLATTING_MAX_PRIORITY_BEFORE_PARENTS);

			FRootPageInfo* RootPageInfo = GetRootPage(RuntimeResourceID);
			if (RootPageInfo)
			{
				auto ProcessPage = [this](uint32 RuntimeResourceID, uint32 PageIndex, uint32 VirtualPageIndex, uint32 Priority)
				{
					FVirtualPage& VirtualPage = RegisteredVirtualPages[VirtualPageIndex];
					if (VirtualPage.RegisteredPageIndex != INDEX_NONE)
					{
						if (VirtualPage.Priority == 0u)
						{
							RequestedRegisteredPages.Add(VirtualPageIndex);
						}
					}
					else
					{
						if (VirtualPage.Priority == 0u)
						{
							RequestedNewPages.Add(FNewPageRequest{FPageKey{RuntimeResourceID, PageIndex}, VirtualPageIndex});
						}
					}
					RegisteredVirtualPages[VirtualPageIndex].Priority = FMath::Max(RegisteredVirtualPages[VirtualPageIndex].Priority,Priority);
				};

				const uint32 VirtualPageRangeStart = RootPageInfo->VirtualPageRangeStart;
				ProcessPage(RuntimeResourceID, FirstPageIndex, VirtualPageRangeStart, Priority);
				for (uint32 i = 1; i < NumPages; ++i)
				{
					const uint32 PageIndex = FirstPageIndex + i;
					const uint32 VirtualPageIndex = VirtualPageRangeStart + i;
					ProcessPage(RuntimeResourceID, PageIndex, VirtualPageIndex, Priority);
				}

			}
		}while(++StreamingRequestsPtr < StreamingRequestsEndPtr);

	}

	void FStreamingManager::AddPendingExplicitRequests()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AddPendingExplicitRequests);




	}

	void FStreamingManager::AddParentNewRequestsRecursive(const FResources& Resource, uint32 RuntimeResourcesID, uint32 PageIndex, uint32 VirtualPageRangeStart, uint32 Priority)
	{

	}

	void FStreamingManager::AddParentRegisteredRequestsRecursive(uint32 RegisteredPageIndex, uint32 Priority)
	{

	}

	void FStreamingManager::RegisterStreamingPage(uint32 RegisteredPageIndex, const FPageKey & Key)
	{
		LLM_SCOPE_BYTAG(GaussianSplatting);
		
		FResources* Resources = GetResources(Key.RuntimeResourceID);
		check(Resources != nullptr);
		check(!Resources->IsRootPage(Key.PageIndex));

		TArray<FPageStreamingState>& PageStreamingStates = Resources->PageStreamingState;
		FPageStreamingState& PageStreamingState = PageStreamingStates[Key.PageIndex];

		const uint32 VirtualPageRageStart = RootPageInfos[Resources->RootPageIndex].VirtualPageRangeStart;

		FRegisteredPage& RegisteredPage = RegisteredPages[RegisteredPageIndex];
		RegisteredPage = FRegisteredPage();
		RegisteredPage.Key = Key;
		RegisteredPage.VirtualPageIndex = VirtualPageRageStart + Key.PageIndex;

		RegisteredVirtualPages[RegisteredPage.VirtualPageIndex].RegisteredPageIndex = RegisteredPageIndex;
		MoveToEndOfLRUList(RegisteredPageIndex);
	}

	void FStreamingManager::UnregisterStreamingPage(const FPageKey& Key)
	{
		LLM_SCOPE_BYTAG(GaussianSplatting);

		if (Key.RuntimeResourceID == INDEX_NONE)
		{
			return;
		}

		const FRootPageInfo* RootPage = GetRootPage(Key.RuntimeResourceID);
		check(RootPage);
		const FResources* Resources = RootPage->Resource;
		check(Resources != nullptr);
		check(!Resources->IsRootPage(Key.PageIndex));

		const uint32 VirtualPageRageStart = RootPage->VirtualPageRangeStart;
		
		const uint32 RegisteredPageIndex = RegisteredVirtualPages[VirtualPageRageStart + Key.PageIndex].RegisteredPageIndex;
		check(RegisteredPageIndex != INDEX_NONE);
		FRegisteredPage& RegisteredPage = RegisteredPages[RegisteredPageIndex];

		RegisteredVirtualPages[RegisteredPage.VirtualPageIndex] = FVirtualPage();
		RegisteredPage = FRegisteredPage();
	}

	void FStreamingManager::SelectHighestPriorityPagesAndUpdateLRU(uint32 MaxSelectedPages)
	{
		SCOPE_CYCLE_COUNTER(STAT_GaussianSplattingStreaming_SelectHighestPriority);

		const auto StreamingRequestsPriorityPredicate = [](const FStreamingRequest& A, const FStreamingRequest& B)
		{
			return A.Priority > B.Priority;
		};

		PrioritizedRequestsHeap.Reset();

		for (const FNewPageRequest& NewPageRequest : RequestedNewPages)
		{
			FStreamingRequest StreamingRequest;
			StreamingRequest.Key = NewPageRequest.Key;
			StreamingRequest.Priority = RegisteredVirtualPages[NewPageRequest.VirtualPageIndex].Priority;

			PrioritizedRequestsHeap.Add(StreamingRequest);
		}

		{
			SCOPE_CYCLE_COUNTER(STAT_GaussianSplattingStreaming_Heapify);
			PrioritizedRequestsHeap.Heapify(StreamingRequestsPriorityPredicate);
		}

		{
			SCOPE_CYCLE_COUNTER(STAT_GaussianSplattingStreaming_UpdateLRU);
			for (const uint32 VirtualPageIndex : RequestedRegisteredPages)
			{
				const uint32 RegisteredPageIndex = RegisteredVirtualPages[VirtualPageIndex].RegisteredPageIndex;
				MoveToEndOfLRUList(RegisteredPageIndex);
			}
		}

		{
			SCOPE_CYCLE_COUNTER(STAT_GaussianSplattingStreaming_ClearReferencedArray);
			for (const uint32 VirtualPageIndex : RequestedRegisteredPages)
			{
				RegisteredVirtualPages[VirtualPageIndex].Priority = 0;
			}

			for (const FNewPageRequest& NewPageRequest : RequestedNewPages)
			{
				RegisteredVirtualPages[NewPageRequest.VirtualPageIndex].Priority = 0;
			}
		}

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(SelectStreamingPage);
			while (uint32(SelectedPages.Num()) < MaxSelectedPages && PrioritizedRequestsHeap.Num() > 0)
			{
				FStreamingRequest SelectRequest;
				PrioritizedRequestsHeap.HeapPop(SelectRequest, StreamingRequestsPriorityPredicate, EAllowShrinking::No);

				FResources* Resources = GetResources(SelectRequest.Key.RuntimeResourceID);
				if (Resources)
				{
					const uint32 NumResourcePages = uint32(Resources->PageStreamingState.Num());
					if (SelectRequest.Key.PageIndex < NumResourcePages)
					{
						SelectedPages.Push(SelectRequest.Key);
					}
				}
				check(uint32(SelectedPages.Num()) < MaxSelectedPages);
			}
		}

	}

	void FStreamingManager::MoveToEndOfLRUList(uint32 RegisteredPageIndex)
	{
		uint32 LRUIndex = RegisteredPageIndexToLRU[RegisteredPageIndex];
		check(LRUIndex != INDEX_NONE);
		check((LRUToRegisteredPageIndex[LRUIndex] & LRU_INDEX_MASK) == RegisteredPageIndex);

		LRUToRegisteredPageIndex[LRUIndex] = INDEX_NONE;
		LRUIndex = LRUToRegisteredPageIndex.Num();
		LRUToRegisteredPageIndex.Add(RegisteredPageIndex | LRU_FLAG_REFERENCED_THIS_UPDATE);
	}

	void FStreamingManager::CompactLRU()
	{
		SCOPE_CYCLE_COUNTER(STAT_GaussianSplattingStreaming_CompactLRU);

		uint32 WriteIndex = 0;
		const uint32 LRUBufferLength = LRUToRegisteredPageIndex.Num();
		for (uint32 i = 0; i < LRUBufferLength; ++i)
		{
			const uint32 Entry = LRUToRegisteredPageIndex[i];
			if (Entry != INDEX_NONE)
			{
				const uint32 RegisteredPageIndex = Entry & LRU_INDEX_MASK;
				LRUToRegisteredPageIndex[WriteIndex] = RegisteredPageIndex;
				RegisteredPageIndexToLRU[RegisteredPageIndex] = WriteIndex;
				WriteIndex++;
			}
		}

		check(WriteIndex == MaxStreamingPages);
		LRUToRegisteredPageIndex.SetNum(WriteIndex);
	}

	uint32 FStreamingManager::DetermineReadyPages(uint32& TotalPageSize)
	{
		LLM_SCOPE_BYTAG(GaussianSplatting);
		TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::DetermineReadyPages);

		const uint32 StartPendingPageIndex = (NextPendingPageIndex + MaxPendingPages - NumPendingPages) % MaxPendingPages;
		uint32 NumReadyPages = 0;

		uint64 UpdateTick = FPlatformTime::Cycles64();
		uint64 DeltaTick = PrevUpdateTick ? UpdateTick - PrevUpdateTick : 0;
		PrevUpdateTick = UpdateTick;

		TotalPageSize = 0;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(CheckReadyPages);

			for (uint32 i = 0; i < NumPendingPages && NumReadyPages < MaxPendingPages; ++i)
			{
				uint32 PendingPageIndex = (StartPendingPageIndex + i) % MaxPendingPages;
				FPendingPage& PendingPage = PendingPages[PendingPageIndex];
				bool bFreePageFromStagingAllocator = false;

				{
					if (PendingPage.Request.IsCompleted())
					{
						if (!PendingPage.Request.IsOk())
						{
							FResources* Resources = GetResources(PendingPage.InstallKey.RuntimeResourceID);
							if (Resources)
							{
								const FPageStreamingState& PageStreamingState = Resources->PageStreamingState[PendingPage.InstallKey.PageIndex];
								FBulkDataBatchRequest::FBatchBuilder Batch = FBulkDataBatchRequest::NewBatch(1);

								Batch.Read(Resources->StreamablePages, PageStreamingState.BulkOffset, PageStreamingState.BulkSize, AIOP_Low, PendingPage.RequestBuffer, PendingPage.Request);
								Batch.Issue();
								break;
							}
						}
					}
					else
					{
						break;
					}
				}

				if (GGaussianSplattingStreamingBandwidthLimit >= 0.0f)
				{
					uint32 SimulatedBytesRemaining = FPlatformTime::ToSeconds64(DeltaTick) * GGaussianSplattingStreamingBandwidthLimit * 1048576.0f;
					uint32 SimulatedBytesRead = FMath::Min(PendingPage.BytesLeftToStream, SimulatedBytesRemaining);
					PendingPage.BytesLeftToStream -= SimulatedBytesRead;
					SimulatedBytesRemaining -= SimulatedBytesRead;
					if(PendingPage.BytesLeftToStream > 0)
						break;
				}

				if (bFreePageFromStagingAllocator)
				{
					PendingPageStagingAllocator->Free(PendingPage.RequestBuffer.DataSize());
				}

				FResources* Resources = GetResources(PendingPage.InstallKey.RuntimeResourceID);
				if (Resources)
				{
					const FPageStreamingState& PageStreamingState = Resources->PageStreamingState[PendingPage.InstallKey.PageIndex];
					TotalPageSize += PageStreamingState.PageSize;
				}

				++NumReadyPages;
			}
		}

		return NumReadyPages;
	}

	void FStreamingManager::InstallReadyPages(uint32 NumReadyPages)
	{
		LLM_SCOPE_BYTAG(GaussianSplatting);
		TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::InstallReadyPages);
		SCOPE_CYCLE_COUNTER(STAT_GaussianSplattingStreaming_InstallReadyPages);

		if(NumReadyPages == 0)
			return;

		const uint32 StartPendingPageIndex = (NextPendingPageIndex + MaxPendingPages - NumPendingPages) % MaxPendingPages;

		struct FUploadTask
		{
			FPendingPage* PendingPage = nullptr;
			uint8* Dst = nullptr;
			const uint8* Src = nullptr;
			uint32 SrcSize = 0;
		};

		TArray<FUploadTask> UploadTasks;
		UploadTasks.AddDefaulted(NumReadyPages);

		{
			TMap<uint32, uint32> GPUPageToLastPendingPageIndex;
			for (uint32 i = 0; i < NumReadyPages; ++i)
			{
				uint32 PendingPageIndex = (StartPendingPageIndex + i) % MaxPendingPages;
				FPendingPage& PendingPage = PendingPages[PendingPageIndex];

				GPUPageToLastPendingPageIndex.Add(PendingPage.GPUPageIndex, PendingPageIndex);
			}

			TSet<FPageKey> BatchNewPageKeys;
			for (auto& Elem : GPUPageToLastPendingPageIndex)
			{
				uint32 GPUPageIndex = Elem.Key;

				FResidentPage& ResidentPage = ResidentPages[GPUPageIndex];
				if (ResidentPage.Key.RuntimeResourceID != INDEX_NONE)
				{
					ResidentPageMap.Remove(ResidentPage.Key);
				}

				FPendingPage& PendingPage = PendingPages[Elem.Value];
				BatchNewPageKeys.Add(PendingPage.InstallKey);
			}

			{
				TRACE_CPUPROFILER_EVENT_SCOPE(UninstallFixup);
				for (auto& Elem : GPUPageToLastPendingPageIndex)
				{
					const uint32 GPUPageIndex = Elem.Key;

					const bool bApplyFixup = !BatchNewPageKeys.Contains(ResidentPages[GPUPageIndex].Key);
					UninstallGPUPage(GPUPageIndex, bApplyFixup);
				}
			}

			for(auto& Elem : GPUPageToLastPendingPageIndex)
			{
				uint32 GPUPageIndex = Elem.Key;
				uint32 LastPendingPageIndex = Elem.Value;
				FPendingPage& PendingPage = PendingPages[LastPendingPageIndex];

				FResources* Resources = GetResources(PendingPage.InstallKey.RuntimeResourceID);
				if (Resources)
				{
					ResidentPageMap.Add(PendingPage.InstallKey, GPUPageIndex);
				}
			}

			{
				TRACE_CPUPROFILER_EVENT_SCOPE(InstallReadyPages);
				uint32 NumInstalledPages = 0;
				for (uint32 TaskIndex = 0; TaskIndex < NumReadyPages; ++TaskIndex)
				{
					uint32 PendingPageIndex = (StartPendingPageIndex + TaskIndex) % MaxPendingPages;
				}


			}

		}

	}

	void FStreamingManager::UninstallGPUPage(uint32 GPUPageIndex, bool bApplyFixup)
	{

	}
	
	void FStreamingManager::AddClusterLeafFlagUpdate(uint32 MaxStreamingPages, uint32 GPUPageIndex, uint32 ClusterIndex, uint32 NumCluster, bool bReset, bool bUninstall)
	{

	}

	void FStreamingManager::FlushClusterLeafFlagUpdates(FRDGBuilder& GraphBuilder, FRDGBuffer* ClusterPageDataBuffer)
	{
		AddPass_UpdateClusterLeafFlags(GraphBuilder, GraphBuilder.CreateUAV(ClusterPageDataBuffer), ClusterLeafFlagUpdates);
		ClusterLeafFlagUpdates.Empty();
	}

	void FStreamingManager::AddParentRequests()
	{
		SCOPE_CYCLE_COUNTER(STAT_GaussianSplattingStreaming_AddParentRequests);

		if (RequestedNewPages.Num() > 0)
		{
			SCOPE_CYCLE_COUNTER(STAT_GaussianSplattingStreaming_AddParentNewRequests);

			//const uint32 
		}

		if (RequestedRegisteredPages.Num() > 0)
		{

		}

	}

	void FStreamingManager::ProcessNewResources(FRDGBuilder& GraphBuilder, FRDGBuffer* ClusterPageDataBuffer)
	{
		LLM_SCOPE_BYTAG(GaussianSplatting);
		TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::ProcessNewResources);

		Hierarchy.Allocator.Consolidate();
		const uint32 NumAllocatedHierarchyNodes = FMath::RoundUpToPowerOfTwo(Hierarchy.Allocator.GetMaxSize());
		FRDGBuffer* HierarchyDataBuffer = ResizeByteAddressBufferIfNeeded(GraphBuilder, Hierarchy.DataBuffer, NumAllocatedHierarchyNodes * sizeof(FPackedHierarchyNode), TEXT("GaussianSplatting.StreamingManager.Hierarchy"));

		Hierarchy.UploadBuffer.Init(GraphBuilder, NumAllocatedHierarchyNodes, sizeof(FPackedHierarchyNode), false, TEXT("GaussianSplatting.StreamingManager.HierarchyUpload"));

		uint32 TotalRootPageSize = 0;
		uint32 TotalRootPages = 0;
		for (FResources* Resources : PendingAdds)
		{
			for (uint32 i = 0; i < Resources->NumRootPages; ++i)
			{
				TotalRootPageSize += Resources->PageStreamingState[i].PageSize;
			}

			TotalRootPages += Resources->NumRootPages;
		}

		FStreamingPageUploader RootPageUploader;
		RootPageUploader.Init(GraphBuilder, TotalRootPages, TotalRootPageSize, MaxStreamingPages);

		for (FResources* Resources : PendingAdds)
		{
			Resources->NumResidentClusters = 0;

			for (uint32 LocalPageIndex = 0; LocalPageIndex < Resources->NumRootPages; ++LocalPageIndex)
			{
				const FPageStreamingState& PageStreamingState = Resources->PageStreamingState[LocalPageIndex];

				const uint32 RootPageIndex = Resources->RootPageIndex + LocalPageIndex;
				const uint32 GPUPageIndex = MaxStreamingPages + RootPageIndex;

				const uint8* Ptr = Resources->RootData.GetData() + PageStreamingState.BulkOffset;
				const FFixupChunk& FixupChunk = (*(FFixupChunk*)Ptr);
				const uint32 FixupChunkSize = FixupChunk.GetSize();
				const uint32 NumGaussians = FixupChunk.Header.NumGaussians;

				const FPageKey GPUPageKey = {Resources->RuntimeResourceID, GPUPageIndex};

				const uint32 PageDiskSize = PageStreamingState.PageSize;
				check(PageDiskSize == (PageStreamingState.BulkSize - FixupChunkSize));
				const uint32 PageOffset = GPUPageIndexToGPUOffset(MaxStreamingPages, GPUPageIndex);

				uint8* Dst = RootPageUploader.Add_GetRef(PageDiskSize, NumGaussians, PageOffset, GPUPageKey);

				FMemory::Memcpy(Dst, Ptr + FixupChunkSize, PageDiskSize);

				for (uint32 i = 0; i < FixupChunk.Header.NumHierachyFixups; ++i)
				{
					const FHierarchyFixup& HierarchyFixup = FixupChunk.GetHierarchyFixup(i);
					const uint32 HierarchyNodeIndex = HierarchyFixup.GetHierarchyNodeIndex();
					check(HierarchyNodeIndex < uint32(Resources->HierarchyNodes.Num()));
					const uint32 ChildIndex = HierarchyFixup.GetChildIndex();
					const uint32 TargetGPUPageIndex = MaxStreamingPages + Resources->RootPageIndex + HierarchyFixup.GetPageIndex();
					const uint32 ChildStartReference = (TargetGPUPageIndex << GAUSSIANSPLATTING_MAX_CLUSTERS_PER_PAGE_BITS) | HierarchyFixup.GetClusterGroupPartStartIndex();

					if (HierarchyFixup.GetPageDependencyNum() == 0)
					{
						Resources->HierarchyNodes[HierarchyNodeIndex].Misc1[ChildIndex].ChildStartReference = ChildStartReference;
					}
				}

				FRootPageInfo& RootPageInfo = RootPageInfos[RootPageIndex];
				RootPageInfo.RuntimeResourceID = Resources->RuntimeResourceID;
				RootPageInfo.NumClusters = NumGaussians;

				Resources->NumResidentClusters += NumGaussians;
			}

			ModifiedResource.Add(Resources->RuntimeResourceID, Resources->NumResidentClusters);

			Hierarchy.UploadBuffer.Add(Resources->HierarchyOffset, Resources->HierarchyNodes.GetData(), Resources->HierarchyNodes.Num());

		#if !WITH_EDITOR
			Resources->RootData.Empty();
			Resources->HierarchyNodes.Empty();
		#endif
		}

		{
			Hierarchy.TotalUpload = 0;
			Hierarchy.UploadBuffer.ResourceUploadTo(GraphBuilder, HierarchyDataBuffer);

			RootPageUploader.ResourceUploadTo(GraphBuilder, ClusterPageDataBuffer);
		}

		PendingAdds.Reset();
	}

	void FStreamingManager::AsyncUpdate()
	{
		LLM_SCOPE_BYTAG(GaussianSplatting);
		SCOPED_NAMED_EVENT(FSteamingManager_AsyncUpdate, FColor::Cyan);
		TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::AsyncUpdate);
		SCOPE_CYCLE_COUNTER(STAT_GaussianSplattingStreaming_AsyncUpdate);

		check(AsyncState.bUpdateActive);

		const uint32 StartTime = FPlatformTime::Cycles();

		if (AsyncState.GPUStreamingRequestsPtr)
		{
			RequestedRegisteredPages.Reset();
			RequestedNewPages.Reset();

			SCOPE_CYCLE_COUNTER(STAT_GaussianSplattingStreaming_ProcessRequests);
			AddPendingGPURequests();
			AddParentRequests();
		}
	
		const uint32 MaxSelectedPages = MaxPendingPages - NumPendingPages;
		SelectedPages.Reset();
		SelectHighestPriorityPagesAndUpdateLRU(MaxSelectedPages);

		uint32 NumLegacyRequestsIssued = 0;

		if (!SelectedPages.IsEmpty())
		{
			FBulkDataBatchRequest::FBatchBuilder Batch = FBulkDataBatchRequest::NewBatch(SelectedPages.Num());
			bool bIssueIOBatch = false;
			float TotalIORequestSizeMB = 0.0f;

			{
				TRACE_CPUPROFILER_EVENT_SCOPE(RegisterPages);

				int32 NextLRUTestIndex = 0;
				for (const FPageKey& SelectedKey : SelectedPages)
				{
					FResources* Resources = GetResources(SelectedKey.RuntimeResourceID);
					check(Resources);
					FByteBulkData& BulkData = Resources->StreamablePages;
					const bool bDiskRequest = true;

					const bool bLegacyRequest = bDiskRequest && !BulkData.IsUsingIODispatcher();
					if(bLegacyRequest && NumLegacyRequestsIssued == MAX_LEGACY_REQUESTS_PER_UPDATE)
						break;

					FRegisteredPage* Page = nullptr;
					while (NextLRUTestIndex < LRUToRegisteredPageIndex.Num())
					{
						const uint32 Entry = LRUToRegisteredPageIndex[NextLRUTestIndex++];
						if (Entry == INDEX_NONE || (Entry & LRU_FLAG_REFERENCED_THIS_UPDATE) != 0)
						{
							continue;
						}

						const uint32 RegisteredPageIndex = Entry & LRU_FLAG_REFERENCED_THIS_UPDATE;
						FRegisteredPage* CandidatePage = &RegisteredPages[RegisteredPageIndex];
						if (CandidatePage && CandidatePage->RefCount == 0)
						{
							Page = CandidatePage;
							break;
						}
					}

					if (Page == nullptr)
					{
						break;
					}

					const FPageStreamingState& PageStreamingState = Resources->PageStreamingState[SelectedKey.PageIndex];
					check(!Resources->IsRootPage(SelectedKey.PageIndex));

					FPendingPage& PendingPage = PendingPages[NextPendingPageIndex];
					PendingPage = FPendingPage();

					{
						uint32 AllocatedOffset;
						if (!PendingPageStagingAllocator->TryAllocate(PageStreamingState.BulkSize, AllocatedOffset))
						{
							break;
						}
						uint8* Dst = PendingPageStagingMemory.GetData() + AllocatedOffset;
						PendingPage.RequestBuffer = FIoBuffer(FIoBuffer::Wrap, Dst, PageStreamingState.BulkSize);
						Batch.Read(BulkData, PageStreamingState.BulkOffset, PageStreamingState.BulkSize, AIOP_Low, PendingPage.RequestBuffer);
						bIssueIOBatch = true;

						if (bLegacyRequest)
						{
							++NumLegacyRequestsIssued;
						}
					}

					UnregisterStreamingPage(Page->Key);

					PendingPage.InstallKey = SelectedKey;
					PendingPage.BytesLeftToStream = PageStreamingState.BulkSize;
					const uint32 GPUPageIndex = uint32(Page - RegisteredPages.GetData());
					PendingPage.GPUPageIndex = GPUPageIndex;

					NextPendingPageIndex = (NextPendingPageIndex + 1) % MaxPendingPages;
					++NumPendingPages;

					RegisterStreamingPage(GPUPageIndex, SelectedKey);
				}
			}

			if(bIssueIOBatch)
			{
			    TRACE_CPUPROFILER_EVENT_SCOPE(FIoBatch::Issue);
				Batch.Issue();
			}
		}

		CompactLRU();
	}

	void FStreamingManager::EndAsyncUpdate(FRDGBuilder& GraphBuilder)
	{
		check(IsInRenderingThread());
		if (!DoesPlatformSupportGS(GMaxRHIShaderPlatform))
		{
			return;
		}

		LLM_SCOPE_BYTAG(GaussianSplatting);
		TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::EndAsyncUpdate);

		RDG_EVENT_SCOPE_STAT(GraphBuilder, GaussianSplattingStreaming, "GS::EndAsyncUpdate");
		RDG_GPU_STAT_SCOPE(GraphBuilder, GaussianSplattingStreaming);

		RDG_GPU_MASK_SCOPE(GraphBuilder, FRHIGPUMask::All());
		SCOPE_CYCLE_COUNTER(STAT_GaussianSplattingStreaming_EndAsyncUpdate);

		check(AsyncState.bUpdateActive);

		AsyncTaskEvents.Empty();

		if (AsyncState.GPUStreamingRequestsPtr)
		{
			ReadbackManager->Unlock();
		}

		if (AsyncState.NumReadyPages > 0)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(UploadPages);

			const FRDGBufferRef ClusterPageDataBuffer = GraphBuilder.RegisterExternalBuffer(ClusterPageData.DataBuffer);
			PageUploader->ResourceUploadTo(GraphBuilder, ClusterPageDataBuffer);
			Hierarchy.UploadBuffer.ResourceUploadTo(GraphBuilder, GraphBuilder.RegisterExternalBuffer(Hierarchy.DataBuffer));
			
		}




	}


	TGlobalResource< FStreamingManager > GStreamingManager;
}
