// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "NiagaraCommon.h"
#include "NiagaraShared.h"
#include "VectorVM.h"
#include "StaticMeshResources.h"
#include "Curves/RichCurve.h"
#include "NiagaraDataInterfaceCurveBase.h"
#include "NiagaraDataInterfaceColorCurve.generated.h"

class INiagaraCompiler;
class UCurveVector;
class UCurveLinearColor;
class UCurveFloat;
class FNiagaraSystemInstance;


/** Data Interface allowing sampling of color curves. */
UCLASS(EditInlineNew, Category="Curves", CollapseCategories, meta = (DisplayName = "Curve for Colors"), MinimalAPI)
class UNiagaraDataInterfaceColorCurve : public UNiagaraDataInterfaceCurveBase
{
	GENERATED_UCLASS_BODY()
public:
	UPROPERTY(EditAnywhere, Category = "Curve")
	FRichCurve RedCurve;

	UPROPERTY(EditAnywhere, Category = "Curve")
	FRichCurve GreenCurve;

	UPROPERTY(EditAnywhere, Category = "Curve")
	FRichCurve BlueCurve;

	UPROPERTY(EditAnywhere, Category = "Curve")
	FRichCurve AlphaCurve;


#if WITH_EDITORONLY_DATA
private:
	UPROPERTY()
	FRichCurve RedCurveCookedEditorCache;
	UPROPERTY()
	FRichCurve GreenCurveCookedEditorCache;
	UPROPERTY()
	FRichCurve BlueCurveCookedEditorCache;
	UPROPERTY()
	FRichCurve AlphaCurveCookedEditorCache;
public:
#endif

	//UObject Interface
	NIAGARA_API virtual void PostInitProperties() override;
	NIAGARA_API virtual void Serialize(FArchive& Ar) override;
	//UObject Interface End

	enum
	{
		CurveLUTNumElems = 4,
	};

	NIAGARA_API virtual void UpdateTimeRanges() override;
	NIAGARA_API virtual TArray<float> BuildLUT(int32 NumEntries) const override;

	NIAGARA_API virtual void GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, void* InstanceData, FVMExternalFunction &OutFunc) override;

	template<typename UseLUT>
	void SampleCurve(FVectorVMExternalFunctionContext& Context);

	NIAGARA_API virtual bool Equals(const UNiagaraDataInterface* Other) const override;

	//~ UNiagaraDataInterfaceCurveBase interface
	NIAGARA_API virtual void GetCurveData(TArray<FCurveData>& OutCurveData) override;

#if WITH_EDITORONLY_DATA
	NIAGARA_API virtual void SyncCurvesToAsset() override;
	NIAGARA_API virtual bool GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, const FNiagaraDataInterfaceGeneratedFunction& FunctionInfo, int FunctionInstanceIndex, FString& OutHLSL) override;
#endif

	virtual int32 GetCurveNumElems() const override { return CurveLUTNumElems; }
#if WITH_EDITORONLY_DATA
	virtual FName GetCurveSampleFunctionName() const override { return SampleCurveName; }
#endif

protected:
#if WITH_EDITORONLY_DATA
	virtual void GetFunctionsInternal(TArray<FNiagaraFunctionSignature>& OutFunctions) const override;
#endif

	NIAGARA_API virtual bool CopyToInternal(UNiagaraDataInterface* Destination) const override;

private:
	template<typename UseLUT>
	FORCEINLINE_DEBUGGABLE FLinearColor SampleCurveInternal(float X);

	static NIAGARA_API const FName SampleCurveName;
};
