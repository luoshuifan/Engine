﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GameplayInteractionsTypes.h"
#include "PlayMontageStateTreeTask.generated.h"

enum class EStateTreeRunStatus : uint8;
struct FStateTreeTransitionResult;

class UAnimMontage;

USTRUCT()
struct FPlayMontageStateTreeTaskInstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category="Context")
	TObjectPtr<AActor> Actor = nullptr;
	
	UPROPERTY()
	float ComputedDuration = 0.0f;

	/** Accumulated time used to stop task if a montage is set */
	UPROPERTY()
	float Time = 0.f;
};


USTRUCT(meta = (DisplayName = "Play Anim Montage", Category="Gameplay Interactions"))
struct FPlayMontageStateTreeTask : public FGameplayInteractionStateTreeTask
{
	GENERATED_BODY()
	
	typedef FPlayMontageStateTreeTaskInstanceData FInstanceDataType;

protected:
	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }

	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	virtual EStateTreeRunStatus Tick(FStateTreeExecutionContext& Context, const float DeltaTime) const override;
#if WITH_EDITOR
	virtual FText GetDescription(const FGuid& ID, FStateTreeDataView InstanceDataView, const IStateTreeBindingLookup& BindingLookup, EStateTreeNodeFormatting Formatting = EStateTreeNodeFormatting::Text) const override;
	virtual FName GetIconName() const override
	{
		return FName("StateTreeEditorStyle|Node.Animation");
	}
	virtual FColor GetIconColor() const override
	{
		return UE::StateTree::Colors::Blue;
	}
#endif
	UPROPERTY(EditAnywhere, Category = Parameter)
	TObjectPtr<UAnimMontage> Montage = nullptr;
};
