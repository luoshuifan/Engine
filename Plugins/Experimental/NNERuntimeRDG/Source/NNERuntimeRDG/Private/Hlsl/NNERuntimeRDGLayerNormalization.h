// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "NNERuntimeRDGBase.h"
#include "NNERuntimeRDGHlslOp.h"

namespace UE::NNERuntimeRDG::Private::Hlsl
{
	bool RegisterLayerNormalizationOperator(FOperatorRegistryHlsl& Registry);
} // UE::NNERuntimeRDG::Private::Hlsl
