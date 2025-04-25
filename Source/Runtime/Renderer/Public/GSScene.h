// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CommonRenderResources.h"
#include "Rendering/GaussianSplattingResources.h"


namespace GS
{
	struct FSceneCard
	{
		FVector4f Position;


	};

	struct FGSPrimitive
	{
		TArray<FVector3f> Position;

		//SH0 * Num + (SHR + SHG + SHB) * Num
		TArray<FVector3f> ColorSH;

		TArray<float> Opacity;

		TArray<FVector2f> Scale;

		TArray<FVector4f> Quat;

		int64 GetSize() const
		{
			return Position.GetTypeSize() * Position.Num()
				+ ColorSH.GetTypeSize() * ColorSH.Num()
				+ Opacity.GetTypeSize() * Opacity.Num()
				+ Scale.GetTypeSize() * Scale.Num()
				+ Quat.GetTypeSize() * Quat.Num();
		}
	};

	struct FGaussianProperty
	{
		int32 NumGaussian;

		int32 MaxSHDegree;
	};

	class FGSScene
	{
	public:
		void LoadPrimitives();

		void PrimitivesToResources();

		const TArray<FGSPrimitive>& GetPrimitives() const  {return Primitives; }
		
		TArray<FGSPrimitive>& GetPrimitives() { return Primitives; }

		int32 PrimitiveNumber() const { return Primitives.Num(); }

	private:
		TArray<FGSPrimitive> Primitives;

		TArray<FGaussianProperty> Gaussians;

		static bool bInitGaussianResources;

	};

}