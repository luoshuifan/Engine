// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "RHI.h"

class FWmfMediaHardwareVideoDecodingTextureSample;

class FWmfMediaHardwareVideoDecodingParameters
{
public:
	WMFMEDIA_API static bool ConvertTextureFormat_RenderThread(FRHICommandListImmediate& RHICmdList, FWmfMediaHardwareVideoDecodingTextureSample* InSample, FTextureRHIRef InDstTexture);
};
