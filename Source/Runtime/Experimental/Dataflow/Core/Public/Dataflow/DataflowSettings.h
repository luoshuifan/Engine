// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Engine/DeveloperSettings.h"
#include "InputCoreTypes.h"

#include "DataflowSettings.generated.h"

typedef TMap<FName, FNodeColors> FNodeColorsMap;

USTRUCT()
struct FNodeColors
{
	GENERATED_USTRUCT_BODY()

	UPROPERTY(EditAnywhere, Category = Colors)
	FLinearColor NodeTitleColor = FLinearColor(0.f, 0.f, 0.f);

	UPROPERTY(EditAnywhere, Category = Colors)
	FLinearColor NodeBodyTintColor = FLinearColor(0.f, 0.f, 0.f);
};

UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "Dataflow"), MinimalAPI)
class UDataflowSettings : public UDeveloperSettings
{
	GENERATED_UCLASS_BODY()

	/** TArray<> pin type color. The other pin colors are defined in the general editor settings. */
	UPROPERTY(config, EditAnywhere, Category = PinColors)
	FLinearColor ArrayPinTypeColor;

	/** FManagedArrayCollection pin type color. The other pin colors are defined in the general editor settings. */
	UPROPERTY(config, EditAnywhere, Category = PinColors)
	FLinearColor ManagedArrayCollectionPinTypeColor;

	/** FBox pin type color. The other pin colors are defined in the general editor settings. */
	UPROPERTY(config, EditAnywhere, Category = PinColors)
	FLinearColor BoxPinTypeColor;

	/** FSphere pin type color. The other pin colors are defined in the general editor settings. */
	UPROPERTY(config, EditAnywhere, Category = PinColors)
	FLinearColor SpherePinTypeColor;

	/** FDataflowAnyType pin type color. The other pin colors are defined in the general editor settings. */
	UPROPERTY(config, EditAnywhere, Category = PinColors)
	FLinearColor DataflowAnyTypePinTypeColor;

	UPROPERTY(config, EditAnywhere, Category = NodeColors)
	TMap<FName, FNodeColors> NodeColorsMap;

	// Begin UDeveloperSettings Interface
	DATAFLOWCORE_API virtual FName GetCategoryName() const override;

	DATAFLOWCORE_API FNodeColors RegisterColors(const FName& Category, const FNodeColors& Colors);

	const TMap<FName, FNodeColors>& GetNodeColorsMap() { return NodeColorsMap; }

#if WITH_EDITOR
	DATAFLOWCORE_API virtual FText GetSectionText() const override;
#endif
	// END UDeveloperSettings Interface

#if WITH_EDITOR
	DATAFLOWCORE_API virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnDataflowSettingsChanged, const FNodeColorsMap&);

	/** Gets a multicast delegate which is called whenever one of the parameters in this settings object changes. */
	FOnDataflowSettingsChanged& GetOnDataflowSettingsChangedDelegate() { return OnDataflowSettingsChangedDelegate; }

protected:
	FOnDataflowSettingsChanged OnDataflowSettingsChangedDelegate;

};

