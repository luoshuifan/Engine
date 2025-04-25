// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplattingCullRaster.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "RHI.h"
#include "SceneUtils.h"
#include "ScenePrivate.h"
#include "SceneTextureParameters.h"
#include "GPUScene.h"
#include "RendererModule.h"
#include "SystemTextures.h"
#include "ComponentRecreateRenderStateContext.h"
#include "VirtualShadowMaps/VirtualShadowMapCacheManager.h"
#include "SceneTextureReductions.h"
#include "Engine/Engine.h"
#include "RenderGraphUtils.h"
#include "Engine/Engine.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "DynamicResolutionState.h"
#include "Lumen/Lumen.h"
#include "TessellationTable.h"
#include "SceneCulling/SceneCullingRenderer.h"
#include "PSOPrecacheValidation.h"
#include "UnrealEngine.h"
#include "Rendering/GaussianSplattingResources.h"

DECLARE_DWORD_COUNTER_STAT(TEXT("CullingContexts"), STAT_GaussianSplattingCullingContexts, STATGROUP_GaussianSplatting);

BEGIN_SHADER_PARAMETER_STRUCT(FCullingParameters, )
SHADER_PARAMETER(FVector2f, HZBSize)

SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray, HZBTextureArray)
SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HZBTexture)
SHADER_PARAMETER_SAMPLER(SamplerState, HZBSampler)
END_SHADER_PARAMETER_STRUCT()

class FGaussianCull_CS : public FGaussianSplattingGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGaussianCull_CS);
	SHADER_USE_PARAMETER_STRUCT(FGaussianCull_CS, FGaussianSplattingGlobalShader);

	using FPermutationDomain = TShaderPermutationDomain<>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGaussianSplattingGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);


	}

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FGaussianParameters, OutGaussianParameters)

		SHADER_PARAMETER_STRUCT_INCLUDE(FGaussianPrimitives, GaussianPrimitives)
		SHADER_PARAMETER(uint32, NumPrimitives)

		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneUniformParameters, Scene)
	END_SHADER_PARAMETER_STRUCT()

};



namespace GS
{
	class FRenderer : public FSceneRenderingAllocatorObject<FRenderer>, public IRenderer
	{
	public:
		FRenderer(
			FRDGBuilder& InGraphBuilder,
			const FScene& InScene,
			const FViewInfo& InSceneView,
			const TRDGUniformBufferRef<FSceneUniformParameters>& InSceneUniformBuffer,
			const FIntRect& InViewRect,
			const TRefCountPtr<IPooledRenderTarget>& InPrevHZB,
			FVirtualShadowMapArray* InVirtualShadowMapArray
		);
	private:
		FRDGBuilder& GraphBuilder;
		const FScene& Scene;
		const FViewInfo& SceneView;
		TRDGUniformBufferRef<FSceneUniformParameters> SceneUniformBuffer;
		FVirtualShadowMapArray* VirtualShadowMapArray;

		TRefCountPtr<IPooledRenderTarget> PrevHZB;
		FIntRect HZBBuildViewRect;

		FCullingParameters CullingParameters;


	private:
		void DrawGaussian();
		void ExtractResults();

	};

	TUniquePtr<IRenderer> IRenderer::Create(
		FRDGBuilder& GraphBuilder,
		const FScene& Scene,
		const FViewInfo& View,
		FSceneUniformBuffer& SceneUniformBuffer,
		const FIntRect& TextureRect,
		const TRefCountPtr<IPooledRenderTarget>& PreHZB,
		FVirtualShadowMapArray* VirtualShadowMapArray
	)
	{
		return MakeUnique<FRenderer>(
			GraphBuilder,
			Scene,
			View,
			SceneUniformBuffer.GetBuffer(GraphBuilder),
			TextureRect,
			PreHZB,
			VirtualShadowMapArray
		);
	}

	FRenderer::FRenderer(
		FRDGBuilder& InGraphBuilder,
		const FScene& InScene,
		const FViewInfo& InSceneView,
		const TRDGUniformBufferRef<FSceneUniformParameters>& InSceneUniformBuffer,
		const FIntRect& InViewRect,
		const TRefCountPtr<IPooledRenderTarget>& InPrevHZB,
		FVirtualShadowMapArray* InVirtualShadowMapArray
	)
	: GraphBuilder(InGraphBuilder)
	, Scene(InScene)
	, SceneView(InSceneView)
	, SceneUniformBuffer(InSceneUniformBuffer)
	, VirtualShadowMapArray(InVirtualShadowMapArray)
	, PrevHZB(InPrevHZB)
	, HZBBuildViewRect(InViewRect)
	{
		
	}

	void FRenderer::DrawGaussian()
	{
		LLM_SCOPE_BYTAG(GaussianSplatting);
		RDG_EVENT_SCOPE(GraphBuilder, "GaussianSplatting::DrawGaussian");

		{
			CullingParameters.HZBTexture = RegisterExternalTextureWithFallback(GraphBuilder, PrevHZB, GSystemTextures.BlackDummy);
			CullingParameters.HZBSize = PrevHZB ? PrevHZB->GetDesc().Extent : FVector2f(0, 0);
			CullingParameters.HZBSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		}



	}

	void FRenderer::ExtractResults()
	{

	}


	FRasterContext InitRasterContext(
		FRDGBuilder& GraphBuilder,
		const FViewFamilyInfo& ViewFamily,
		FIntPoint TextureSize,
		FIntRect TextureRect,
		bool bClearTarget,
		bool bAsyncCompute,
		FRDGBufferSRVRef RectMinMaxBufferSRV,
		uint32 NumRects,
		FRDGTextureRef ExternalDepthBuffer,
		bool bCustomPass,
		bool bVisualize,
		bool bVisualizeOverdraw
	)
	{
		check(ExternalDepthBuffer == nullptr || ExternalDepthBuffer->Desc.Extent == TextureSize);

		LLM_SCOPE_BYTAG(GaussianSplatting);
		RDG_EVENT_SCOPE(GraphBuilder, "GaussianSplatting::InitContext");

		FRasterContext RasterContext = {};

		RasterContext.bCustomPass = bCustomPass;
		RasterContext.VisualizeActive = bVisualize;
		RasterContext.VisualizeModeOverdraw = bVisualize && bVisualizeOverdraw;
		RasterContext.TextureSize = TextureSize;

		return RasterContext;
	}


} // namespace GS
