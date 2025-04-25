// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "PSOPrecacheMaterial.h"

class FVirtualShadowMapArray;
class FViewFamilyInfo;
class FSceneInstanceCullingQuery;

namespace GS
{
	struct FRasterContext
	{
		FVector2f			RcpViewSize;
		FIntPoint			TextureSize;

		FRDGTextureRef		DepthBuffer;
		FRDGTextureRef		DbgBuffer64;
		FRDGTextureRef		DbgBuffer32;

		bool				VisualizeActive;
		bool				VisualizeModeOverdraw;

		bool				bCustomPass;
	};

	class IRenderer
	{
	public:
		static TUniquePtr<IRenderer> Create(
			FRDGBuilder& GraphBuilder,
			const FScene& Scene,
			const FViewInfo& View,
			FSceneUniformBuffer& SceneUniformBuffer,
			const FIntRect& TextureRect,
			const TRefCountPtr<IPooledRenderTarget>& PreHZB,
			FVirtualShadowMapArray* VirtualShadowMapArray = nullptr
		);

		IRenderer() = default;
		virtual ~IRenderer() = default;

		virtual void DrawGaussian() = 0;
		virtual void ExtractResults() = 0;
	};

	FRasterContext InitRasterContext(
		FRDGBuilder& GraphBuilder,
		const FViewFamilyInfo& ViewFamily,
		FIntPoint TextureSize,
		FIntRect TextureRect,
		bool bClearTarget = true,
		bool bAsyncCompute = true,
		FRDGBufferSRVRef RectMinMaxBufferSRV = nullptr,
		uint32 NumRects = 0,
		FRDGTextureRef ExternalDepthBuffer = nullptr,
		bool bCustomPass = false,
		bool bVisualize = false,
		bool bVisualizeOverdraw = false
	);

} // namespace GS
