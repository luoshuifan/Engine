// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "USDGeomMeshTranslator.h"

#if USE_USD_SDK

class USDSCHEMAS_API UE_DEPRECATED(5.4, "Use the UsdSkelSkeletonTranslator for skeletal data") FUsdSkelRootTranslator
	: public FUsdGeomXformableTranslator
{
	using Super = FUsdGeomXformableTranslator;

public:
	using FUsdGeomXformableTranslator::FUsdGeomXformableTranslator;

	virtual void CreateAssets() override;
	virtual USceneComponent* CreateComponents() override;
	virtual void UpdateComponents(USceneComponent* SceneComponent) override;

	virtual bool CollapsesChildren(ECollapsingType CollapsingType) const override;
	virtual bool CanBeCollapsed(ECollapsingType CollapsingType) const override;

	virtual TSet<UE::FSdfPath> CollectAuxiliaryPrims() const override;
};

#endif	  // #if USE_USD_SDK
