// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/LowLevelMemTracker.h"
#include "GrowOnlySpanAllocator.h"
#include "IO/IoHash.h"
#include "UnifiedBuffer.h"
#include "RenderGraphDefinitions.h"
#include "SceneManagement.h"
#include "Materials/MaterialInterface.h"
#include "Serialization/BulkData.h"
#include "Misc/MemoryReadStream.h"
#include "Templates/DontCopy.h"
#include "VertexFactory.h"
#include "GaussianDefinitions.h"

#if PLATFORM_WINDOWS
#define GAUSSIANSPLATTING_ENABLE_DEBUG_RENDERING (!(UE_BUILD_SHIPPING || UE_BUILD_TEST) || WITH_EDITOR)
#else
#define GAUSSIANSPLATTING_ENABLE_DEBUG_RENDERING 0
#endif

DECLARE_STATS_GROUP( TEXT("GaussianSplatting"), STATGROUP_GaussianSplatting, STATCAT_Advanced );

DECLARE_GPU_STAT_NAMED_EXTERN(GaussianSplattingStreaming, TEXT("GaussianSplatting Streaming"));
DECLARE_GPU_STAT_NAMED_EXTERN(GaussianSplattingReadback, TEXT("GaussianSplatting Readback"));

LLM_DECLARE_TAG_API(GaussianSplatting, ENGINE_API);

class UStaticMesh;
class UBodySetup;
class FDistanceFieldVolumeData;
class UStaticMeshComponent;
class UInstancedStaticMeshComponent;
class UHierarchicalInstancedStaticMeshComponent;
class FVertexFactory;

namespace UE::DerivedData { class FRequestOwner; }

namespace GS
{
	struct FPackedHierarchyNode
	{
		//Sphere Center.xyz + Radii
		FVector4f LODBounds[GAUSSIANSPLATTING_MAX_BVH_NODE_FANOUT];

		struct
		{
			FVector3f BoxBoundsCenters;
			uint32 MinLODError_MaxParentLODError;
		}Misc0[GAUSSIANSPLATTING_MAX_BVH_NODE_FANOUT];

		struct
		{
			FVector3f BoxBoundsExtents;
			uint32 ChildStartReference;
		}Misc1[GAUSSIANSPLATTING_MAX_BVH_NODE_FANOUT];

		struct
		{
			uint32 ResourcePageIndex_NumPages_GroupPartSize;
		}Misc2[GAUSSIANSPLATTING_MAX_BVH_NODE_FANOUT];
	};
	
	struct FPackedLeafNode
	{
		FVector3f Position;

		FVector3f ColorSH[16];

		float Opacity;

		FVector2f Scale;

		FVector4f Quat;
	};


	FORCEINLINE uint32 GetBits(uint32 Value, uint32 NumBits, uint32 Offset)
	{
		uint32 Mask = (1u << NumBits) - 1u;
		return (Value >> Offset) & Mask;
	}

	FORCEINLINE void SetBits(uint32& Value, uint32 Bits, uint32 NumBits, uint32 Offset)
	{
		uint32 Mask = (1u << NumBits) - 1u;
		check(Bits <= Mask);
		Mask <<= Offset;
		Value = (Value & ~Mask) | (Bits << Offset);
	}
	
	struct FPageStreamingState
	{
		uint32 BulkOffset;
		uint32 BulkSize;
		uint32 PageSize;
		uint32 Flag;
	};

	class FHierarchyFixup
	{
	public:
		FHierarchyFixup() {}

		FHierarchyFixup(uint32 InPageIndex, uint32 NodeIndex, uint32 ChildIndex, uint32 InClusterGroupPartStartIndex, uint32 PageDependencyStart, uint32 PageDependencyNum)
		{
			check(InPageIndex < GAUSSIANSPLATTING_MAX_RESOURCE_PAGES);
			PageIndex = InPageIndex;

			check(NodeIndex < (1 << (32 - GAUSSIANSPLATTING_MAX_HIERACHY_CHILDREN_BITS)));
			check(ChildIndex < GAUSSIANSPLATTING_MAX_HIERACHY_CHILDREN);
			check(InClusterGroupPartStartIndex < (1 << (32 - GAUSSIANSPLATTING_MAX_CLUSTERS_PER_GROUP_BITS)));
			HierarchyNodeAndChildIndex = (NodeIndex << GAUSSIANSPLATTING_MAX_HIERACHY_CHILDREN_BITS) | ChildIndex;
			ClusterGroupPartStartIndex = InClusterGroupPartStartIndex;

			check(PageDependencyStart < GAUSSIANSPLATTING_MAX_RESOURCE_PAGES);
			check(PageDependencyNum <= GAUSSIANSPLATTING_MAX_GROUP_PARTS_MASK);
			PageDependencyStartAndNum = (PageDependencyStart << GAUSSIANSPLATTING_MAX_GROUP_PARTS_BITS) | PageDependencyNum; 
		}

		uint32 GetPageIndex()const { return PageIndex; }
		uint32 GetHierarchyNodeIndex() const {return HierarchyNodeAndChildIndex >> GAUSSIANSPLATTING_MAX_HIERACHY_CHILDREN_BITS; }
		uint32 GetChildIndex() const { return HierarchyNodeAndChildIndex & (GAUSSIANSPLATTING_MAX_HIERACHY_CHILDREN - 1); }
		uint32 GetClusterGroupPartStartIndex() const {return ClusterGroupPartStartIndex; }
		uint32 GetPageDependencyStart() const { return PageDependencyStartAndNum >> GAUSSIANSPLATTING_MAX_GROUP_PARTS_BITS; }
		uint32 GetPageDependencyNum() const { return PageDependencyStartAndNum & GAUSSIANSPLATTING_MAX_GROUP_PARTS_MASK; }

		uint32 PageIndex;
		uint32 HierarchyNodeAndChildIndex;
		uint32 ClusterGroupPartStartIndex;
		uint32 PageDependencyStartAndNum;
	};

	struct FFixupChunk
	{
	public:
		struct FHeader
		{
			uint32 NumGaussians = 0;
			uint16 Magic = 0;
			uint16 NumHierachyFixups = 0;
		}Header;

		uint8 Data[sizeof(FHierarchyFixup) * GAUSSIANSPLATTING_MAX_CLUSTERS_PER_PAGE];

		FHierarchyFixup& GetHierarchyFixup(uint32 Index) const { check(Index < Header.NumHierachyFixups); return ((FHierarchyFixup*)Data)[Index]; }
		uint32 GetSize() const { return sizeof(FHeader) + Header.NumHierachyFixups * sizeof(FHierarchyFixup); }
	};


	struct FResources
	{
		//Cluster+GaussianData(Pos, ColorSH, Opacity, Scale, Quat.)
		TArray<uint8> RootData;
		FByteBulkData StreamablePages;
		TArray<FPackedHierarchyNode> HierarchyNodes;
		TArray<uint32> HierarchyRootOffsets;
		TArray<FPageStreamingState> PageStreamingState;
		uint32 NumRootPages;
		uint32 NumInputGaussian;
		uint32 ResourceFlags;

		//Runtime State
		uint32 RuntimeResourceID = 0xFFFFFFFFu;
		uint32 HierarchyOffset = 0xFFFFFFFFu;
		int32 RootPageIndex = 0;
		uint32 NumHierarchyNodes = 0;
		uint32 NumResidentClusters = 0;
		uint32 PersistentHash = GAUSSIANSPLATTING_INVALID_PERSISTENT_HASH;

		ENGINE_API void InitResources(const UObject* Owner);
		ENGINE_API bool ReleaseResources();

		ENGINE_API void Serizlize(FArchive& Ar, UObject* Owner, bool bCooked);
		ENGINE_API bool HasStreamingData() const;

		ENGINE_API void GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize) const;
		ENGINE_API bool IsRootPage(uint32 PageIndex) const { return PageIndex < NumRootPages; }

	private:
		ENGINE_API void SerializeInternal(FArchive& Ar, UObject* Owner, bool bCooked);
	};


} // namespace GaussianSplatting
//
//ENGINE_API void ClearNaniteResources(TPimplPtr<Nanite::FResources>& InResources);
//ENGINE_API void InitNaniteResources(TPimplPtr<Nanite::FResources>& InResources, bool bRecreate = false);
//
//ENGINE_API uint64 GetNaniteResourcesSize(const TPimplPtr<Nanite::FResources>& InResources);
//ENGINE_API void GetNaniteResourcesSizeEx(const TPimplPtr<Nanite::FResources>& InResources, FResourceSizeEx& CumulativeResourceSize);
//
//ENGINE_API uint64 GetNaniteResourcesSize(const Nanite::FResources& InResources);
//ENGINE_API void GetNaniteResourcesSizeEx(const Nanite::FResources& InResources, FResourceSizeEx& CumulativeResourceSize);
