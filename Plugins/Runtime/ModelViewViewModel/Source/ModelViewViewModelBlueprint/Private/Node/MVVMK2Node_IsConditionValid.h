// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "K2Node.h"
#include "View/MVVMViewTypes.h"

#include "MVVMK2Node_IsConditionValid.generated.h"

class UEdGraphPin;
class FKismetCompilerContext;

UCLASS(MinimalAPI, Hidden, HideDropDown)
class UMVVMK2Node_IsConditionValid : public UK2Node
{
	GENERATED_BODY()

public:
	//~ Begin K2Node Interface.
	virtual void ExpandNode(FKismetCompilerContext& CompilerContext, UEdGraph* SourceGraph) override;
	virtual void AllocateDefaultPins() override;
	//~ End K2Node Interface.

	/** Get the then output pin */
	UEdGraphPin* GetThenPin() const;
	/** Get the else output pin */
	UEdGraphPin* GetElsePin() const;

	UEdGraphPin* GetOperationPin() const;
	UEdGraphPin* GetValuePin() const;
	UEdGraphPin* GetCompareValuePin() const;
	UEdGraphPin* GetCompareMaxValuePin() const;

	UPROPERTY()
	FMVVMViewClass_ConditionKey ConditionKey;
};

