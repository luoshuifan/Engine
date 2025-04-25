// Copyright Epic Games, Inc. All Rights Reserved.

#include "GaussianSplattingShared.h"
#include "RHI.h"
#include "SceneUtils.h"
#include "ScenePrivate.h"
#include "SceneRelativeViewMatrices.h"
#include "UnrealEngine.h"

DEFINE_LOG_CATEGORY(LogGaussianSplatting);
DEFINE_GPU_STAT(GaussianSplattingDebug);