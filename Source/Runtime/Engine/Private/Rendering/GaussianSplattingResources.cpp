// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/GaussianSplattingResources.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/Engine.h"
#include "EngineLogs.h"
#include "EngineModule.h"
#include "HAL/LowLevelMemStats.h"
#include "Rendering/GaussianSplattingStreamingManager.h"
#include "Rendering/RayTracingGeometryManager.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "EngineUtils.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/InstancedStaticMesh.h"
#include "Engine/SkinnedAssetCommon.h"
#include "SkeletalRenderPublic.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "CommonRenderResources.h"
#include "DistanceFieldAtlas.h"
#include "Elements/SMInstance/SMInstanceElementData.h" // For SMInstanceElementDataUtil::SMInstanceElementsEnabled
#include "MaterialCachedData.h"
#include "MaterialDomain.h"
#include "MeshMaterialShader.h"
#include "PrimitiveSceneInfo.h"
#include "SceneInterface.h"
#include "StaticMeshComponentLODInfo.h"
#include "Stats/StatsTrace.h"
#include "SkinningDefinitions.h"

#include "ComponentRecreateRenderStateContext.h"
#include "StaticMeshSceneProxyDesc.h"
#include "InstancedStaticMeshSceneProxyDesc.h"
#include "GPUSkinCacheVisualizationData.h"
#include "VT/MeshPaintVirtualTexture.h"

#include "AnimationRuntime.h"

#if WITH_EDITOR
#include "DerivedDataCache.h"
#include "DerivedDataRequestOwner.h"
#include "Rendering/StaticLightingSystemInterface.h"
#endif

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
#include "SkeletalDebugRendering.h"
#endif

#if WITH_EDITORONLY_DATA
#include "UObject/Package.h"
#endif

#if GAUSSIANSPLATTING_ENABLE_DEBUG_RENDERING
#include "AI/Navigation/NavCollisionBase.h"
#include "PhysicsEngine/BodySetup.h"
#endif

#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"

DEFINE_GPU_STAT(GaussianSplattingStreaming);
DEFINE_GPU_STAT(GaussianSplattingReadback);

DECLARE_LLM_MEMORY_STAT(TEXT("GaussianSplatting"), STAT_GaussianSplattingLLM, STATGROUP_LLMFULL);
DECLARE_LLM_MEMORY_STAT(TEXT("GaussianSplatting"), STAT_GaussianSplattingSummaryLLM, STATGROUP_LLM);
LLM_DEFINE_TAG(GaussianSplatting, NAME_None, NAME_None, GET_STATFNAME(STAT_GaussianSplattingLLM), GET_STATFNAME(STAT_GaussianSplattingSummaryLLM));

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

extern TAutoConsoleVariable<int32> CVarDebugDrawSimpleBones;
extern TAutoConsoleVariable<int32> CVarDebugDrawBoneAxes;

#endif

namespace GS
{

	void FResources::InitResources(const UObject* Owner)
	{
		if (PageStreamingState.Num() == 0)
		{
			return;
		}

		check(RootData.Num() > 0)
		PersistentHash = FMath::Max(FCrc::StrCrc32<TCHAR>(TEXT("GaussianTest")),1u);

		ENQUEUE_RENDER_COMMAND(InitGaussianSplattingResource)(
			[this](FRHICommandListImmediate& RHICmdList)
			{
				GStreamingManager.Add(this);
			}
		);
	}

} // namespace GS

