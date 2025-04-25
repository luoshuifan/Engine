// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ShaderParameterMacros.h"
#include "GlobalShader.h"
#include "UnifiedBuffer.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "Rendering/GaussianSplattingResources.h"
#include "MaterialDomain.h"
#include "MaterialShaderType.h"
#include "MaterialShader.h"
#include "Misc/ScopeRWLock.h"
#include "Experimental/Containers/RobinHoodHashTable.h"
#include "LightMapRendering.h" // TODO: Remove with later refactor (moving Nanite shading into its own files)
#include "RenderUtils.h"
#include "PrimitiveViewRelevance.h"

DECLARE_LOG_CATEGORY_EXTERN(LogGaussianSplatting, Warning, All);

DECLARE_GPU_STAT_NAMED_EXTERN(GaussianSplattingDebug, TEXT("GaussianSplatting Debug"));

struct FSceneTextures;
struct FDBufferTextures;

namespace GS
{


} // namespace GS

BEGIN_SHADER_PARAMETER_STRUCT(FGaussianPrimitives, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, Positions)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector3f>, ColorSH)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, Opacity)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector2f>, Scale)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FVector4f>, Quat)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FGaussianParameters, )
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int>, Radii)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector2f>, PointsXYImage)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, Depths)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, TransMats)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, RGB)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, NormalOpacity)
END_SHADER_PARAMETER_STRUCT()


class FGaussianSplattingGlobalShader : public FGlobalShader
{
public:
	FGaussianSplattingGlobalShader() = default;
	FGaussianSplattingGlobalShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
	: FGlobalShader(Initializer)
	{
	}
	
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportNanite(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		// Force shader model 6.0+
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
		OutEnvironment.CompilerFlags.Add(CFLAG_HLSL2021);
		OutEnvironment.CompilerFlags.Add(CFLAG_WarningsAsErrors);
	}
};

class FGaussianSplattingMaterialShader : public FMaterialShader
{
public:
	FGaussianSplattingMaterialShader() = default;
	FGaussianSplattingMaterialShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
	: FMaterialShader(Initializer)
	{
	}

	static bool ShouldCompileProgrammablePermutation(
		const FMaterialShaderParameters& MaterialParameters,
		bool bPermutationVertexProgrammable,
		bool bPermutationPixelProgrammable,
		bool bHWRasterShader)
	{
		if (MaterialParameters.bIsDefaultMaterial)
		{
			return true;
		}
		return	true;
	}


	static bool ShouldCompilePixelPermutation(const FMaterialShaderPermutationParameters& Parameters)
	{
	
		return true;
	}
	
	static bool ShouldCompileVertexPermutation(const FMaterialShaderPermutationParameters& Parameters)
	{

		return true;
	}
	
	static bool ShouldCompileComputePermutation(const FMaterialShaderPermutationParameters& Parameters)
	{
		return true;
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);

		// Force shader model 6.0+
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
		OutEnvironment.CompilerFlags.Add(CFLAG_HLSL2021);
		OutEnvironment.CompilerFlags.Add(CFLAG_ShaderBundle);
		OutEnvironment.CompilerFlags.Add(CFLAG_RootConstants);
	}
};
