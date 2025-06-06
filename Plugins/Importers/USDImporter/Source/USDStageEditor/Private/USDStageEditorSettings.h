// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "USDStageEditorSettings.generated.h"

UCLASS(config = EditorPerProjectUserSettings)
class UUsdStageEditorSettings : public UObject
{
	GENERATED_BODY()

public:
	/** Whether our prim selection in SUSDStageTreeView is kept synchronized with the viewport selection */
	UPROPERTY(config, EditAnywhere, Category = USD)
	bool bSelectionSynced = true;

	/** Whether to automatically set a layer as edit target when isolating it */
	UPROPERTY(config, EditAnywhere, Category = USD)
	bool bIsolateLayerSyncedWithEditTarget = true;
};
