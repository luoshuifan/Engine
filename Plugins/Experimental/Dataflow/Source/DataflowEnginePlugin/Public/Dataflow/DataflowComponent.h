// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "GeometryCollection/ManagedArrayCollection.h"
#include "Components/MeshComponent.h"
#include "Dataflow/DataflowComponentSelectionState.h"
#include "Dataflow/DataflowObject.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectGlobals.h"

#include "DataflowComponent.generated.h"

namespace UE::Dataflow { class IDataflowConstructionViewMode; }

/**
*	UDataflowComponent
*/
UCLASS(meta = (BlueprintSpawnableComponent))
class DATAFLOWENGINEPLUGIN_API UDataflowComponent : public UPrimitiveComponent
{
	GENERATED_UCLASS_BODY()

public:

	virtual ~UDataflowComponent();

	void Invalidate();
	void UpdateLocalBounds();


	//~ USceneComponent Interface.
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;

	//~ UPrimitiveComponent Interface.
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual UMaterialInterface* GetMaterial(int32 Index) const override;

	/* Rendering Support*/
	virtual FMaterialRelevance GetMaterialRelevance(ERHIFeatureLevel::Type InFeatureLevel) const;
	UMaterialInterface* GetDefaultMaterial() const;

	/** Render Targets*/
	void ResetRenderTargets(); 
	void AddRenderTarget(const UDataflowEdNode* InTarget); 
	const TArray<const UDataflowEdNode*>& GetRenderTargets() const {return RenderTargets;}

	/** Context */
	void SetContext(TSharedPtr<UE::Dataflow::FContext> InContext) { Context = InContext; }

	/** RenderCollection */
	void SetRenderingCollection(FManagedArrayCollection&& InCollection);
	const FManagedArrayCollection& GetRenderingCollection() const;
	      FManagedArrayCollection& ModifyRenderingCollection();

	/** Dataflow */
	void SetDataflow(const UDataflow* InDataflow) { Dataflow = InDataflow; }
	const UDataflow* GetDataflow() const { return Dataflow; }

	/* Selection */
	const FDataflowSelectionState& GetSelectionState() const { return SelectionState; }
	void SetSelectionState(const FDataflowSelectionState& InState) 
	{
		bUpdateSelection = true;
		SelectionState = InState; 
	}

	/* View mode */
	// NOTE: Currently UDataflowComponent is not used in the Dataflow Editor. Instead the FDataflowConstructionScene converts the FRenderingFacade to a UDynamicMeshComponent.
	// If we do start using UDataflowComponent we will need to update the current View Mode as it's changed using this function.
	void SetViewMode(const UE::Dataflow::IDataflowConstructionViewMode* InViewMode)
	{
		ViewMode = InViewMode;
	}

private:
	TSharedPtr<UE::Dataflow::FContext> Context;
	TArray<const UDataflowEdNode*> RenderTargets;
	TObjectPtr< const UDataflow> Dataflow;
	FManagedArrayCollection RenderCollection;

	bool bUpdateRender = true;
	bool bUpdateSelection = true;
	bool bBoundsNeedsUpdate = true;
	FBoxSphereBounds BoundingBox = FBoxSphereBounds(ForceInitToZero);
	FDataflowSelectionState SelectionState = FDataflowSelectionState(FDataflowSelectionState::EMode::DSS_Dataflow_None);
	const UE::Dataflow::IDataflowConstructionViewMode* ViewMode;
};

