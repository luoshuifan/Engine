// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IO/IoDispatcher.h"
#include "Memory/SharedBuffer.h"
#include "GaussianSplattingResources.h"
#include "UnifiedBuffer.h"
#include "SpanAllocator.h"

namespace UE
{
	namespace DerivedData
	{
		class FRequestOwner; // Can't include DDC headers from here, so we have to forward declare
		struct FCacheGetChunkRequest;
	}
}

class FRDGBuilder;

namespace GS
{

struct FPageKey
{
	uint32 RuntimeResourceID	= INDEX_NONE;
	uint32 PageIndex			= INDEX_NONE;

	friend FORCEINLINE uint32 GetTypeHash(const FPageKey& Key)
	{
		return Key.RuntimeResourceID * 0xFC6014F9u + Key.PageIndex * 0x58399E77u;
	}

	FORCEINLINE bool operator==(const FPageKey& Other) const 
	{
		return RuntimeResourceID == Other.RuntimeResourceID && PageIndex == Other.PageIndex;
	}

	FORCEINLINE bool operator!=(const FPageKey& Other) const
	{
		return !(*this == Other);
	}

	FORCEINLINE bool operator<(const FPageKey& Other) const
	{
		return RuntimeResourceID != Other.RuntimeResourceID ? RuntimeResourceID < Other.RuntimeResourceID : PageIndex < Other.PageIndex;
	}
};

struct FStreamingRequest
{
	FPageKey	Key;
	uint32		Priority;
	
	FORCEINLINE bool operator<(const FStreamingRequest& Other) const 
	{
		return Key != Other.Key ? Key < Other.Key : Priority > Other.Priority;
	}
};

/*
 * Streaming manager for Gaussian Splatting.
 */
class FStreamingManager : public FRenderResource
{
public:
	FStreamingManager();
	
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;

	void Add(FResources* Resources);
	void Remove(FResources* Resources);

	ENGINE_API void BeginAsyncUpdate(FRDGBuilder& GraphBuilder);
	ENGINE_API void EndAsyncUpdate(FRDGBuilder& GraphBuilder);
	ENGINE_API void SubmitFrameStreamingRequests(FRDGBuilder& GraphBuilder);

	ENGINE_API FRDGBuffer* GetStreamingRequestsBuffer(FRDGBuilder& GraphBuilder) const;

	ENGINE_API uint32 GetStreamingRequestsBufferVersion() const;

private:
	friend class FStreamingUpdateTask;

	struct FAsyncState
	{
		struct FGPUStreamingRequest* GPUStreamingRequestsPtr = nullptr;
		uint32 NumGPUStreamingRequests = 0;
		uint32 NumReadyPages = 0;
		bool bUpdateActive = false;
		bool bBuffersTransitionedToWrite = false;
	};

	struct FPendingPage
	{
	#if WITH_EDITOR
		FSharedBuffer SharedBuffer;
		enum class EState
		{
			None,
			DDC_Pending,
			DDC_Ready,
			DDC_Failed,
			Memory,
			Disk,
		}State = EState::None;
		uint32 RetryCount = 0;
	#endif
		FIoBuffer RequestBuffer;
		FBulkDataBatchReadRequest Request;

		uint32 GPUPageIndex = INDEX_NONE;
		FPageKey InstallKey;
		uint32 BytesLeftToStream = 0;
	};

	struct FHeapBuffer
	{
		int32 TotalUpload = 0;
		FSpanAllocator Allocator;
		FRDGScatterUploadBuffer UploadBuffer;
		TRefCountPtr<FRDGPooledBuffer> DataBuffer;

		void Release()
		{
			UploadBuffer = {};
			DataBuffer = {};
		}
	};

	struct FVirtualPage
	{
		uint32 Priority = 0u;
		uint32 RegisteredPageIndex = INDEX_NONE;

		FORCEINLINE bool operator==(const FVirtualPage& Other) const
		{
			return Priority == Other.Priority && RegisteredPageIndex == Other.RegisteredPageIndex;
		}
	};

	struct FNewPageRequest
	{
		FPageKey Key;
		uint32 VirtualPageIndex = INDEX_NONE;
	};

	struct FRegisteredPage
	{
		FPageKey Key;
		uint32 VirtualPageIndex = INDEX_NONE;
		uint8 RefCount = 0;
	};

	struct FResidentPage
	{
		FPageKey Key;
		uint8 MaxHierarchyDepth = 0xFF;
	};

	struct FRootPageInfo
	{
		FResources* Resource = nullptr;
		uint32 RuntimeResourceID = INDEX_NONE;
		uint32 VirtualPageRangeStart = INDEX_NONE;
		uint16 NumClusters = 0u;
		uint8 MaxHierarchyDepth = 0xFF;
	};

	TArray<FResources*> PendingAdds;

	TMultiMap<uint32, FResources*> PersistenHashResourceMap;

	//Key = RuntimeResourceID, Value = NumResidentGaussians
	TMap<uint32, uint32> ModifiedResource; 

	FSpanAllocator VirtualPageAllocator;
	TArray<FVirtualPage> RegisteredVirtualPages;

	TArray<FRootPageInfo> RootPageInfos;
	TArray<uint8> RootPageVersions;

	TArray<FNewPageRequest> RequestedNewPages;
	TArray<uint32> RequestedRegisteredPages;

	FHeapBuffer ClusterPageData;
	FHeapBuffer Hierarchy;
	TArray<uint32> ClusterLeafFlagUpdates;

	TPimplPtr<class FStreamingPageUploader> PageUploader;
	TPimplPtr<class FReadbackManager> ReadbackManager;

	TArray<FPageKey> SelectedPages;
	TArray<FStreamingRequest> PrioritizedRequestsHeap;

	FGraphEventArray AsyncTaskEvents;
	FAsyncState AsyncState;

	TArray<uint32> RegisteredPageIndexToLRU;
	TArray<uint32> LRUToRegisteredPageIndex;

	TArray<FRegisteredPage> RegisteredPages;

	TArray<FPendingPage> PendingPages;
	TArray<uint8> PendingPageStagingMemory;
	TPimplPtr<class FRingBufferAllocator> PendingPageStagingAllocator;

	uint32 MaxStreamingPages = 0;
	uint32 MaxRootPages = 0;
	uint32 NumInitialRootPages = 0;
	uint32 PrevNumInitialRootPages = 0;
	uint32 MaxPendingPages = 0;
	uint32 MaxPageInstallsPerUpdate = 0;

	uint32 NumResources = 0;
	uint32 NumPendingPages = 0;
	uint32 NextPendingPageIndex = 0;

	void ProcessNewResources(FRDGBuilder& GraphBuilder, FRDGBuffer* ClusterPageDataBuffer);

	void UpdatePageConfiguration();

	void ResetStreamingStateCPU();

	FRDGBuffer* ResizePoolAllocationIfNeeded(FRDGBuilder& GraphBuilder);

	FRootPageInfo* GetRootPage(uint32 RuntimeResourceID);
	FResources* GetResources(uint32 RuntimeResourceID);

	void AddPendingGPURequests();

	//Debug Function Begin
	void AddPendingExplicitRequests();
	//Debug Function End

	void AddParentRequests();
	void AddParentNewRequestsRecursive(const FResources& Resource, uint32 RuntimeResourcesID, uint32 PageIndex, uint32 VirtualPageRangeStart, uint32 Priority);
	void AddParentRegisteredRequestsRecursive(uint32 RegisteredPageIndex, uint32 Priority);

	void RegisterStreamingPage(uint32 RegisteredPageIndex, const FPageKey& Key);
	void UnregisterStreamingPage(const FPageKey& Key);

	void SelectHighestPriorityPagesAndUpdateLRU(uint32 MaxSelectedPages);

	void MoveToEndOfLRUList(uint32 RegisteredPageIndex);
	void CompactLRU();

	void AsyncUpdate();
};

extern ENGINE_API TGlobalResource< FStreamingManager > GStreamingManager;

} // namespace GS