// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Converters/GLTFConverter.h"
#include "Converters/GLTFMeshAttributesArray.h"
#include "Containers/StaticArray.h"

struct FMeshDescription;

class GLTFEXPORTER_API FGLTFUVDegenerateChecker : public TGLTFConverter<float, const FMeshDescription*, FGLTFIndexArray, int32>
{
protected:

	virtual float Convert(const FMeshDescription* Description, FGLTFIndexArray SectionIndices, int32 TexCoord) override;

private:

	static bool IsDegenerateTriangle(const TStaticArray<FVector2f, 3>& Points);
	static bool IsDegenerateTriangle(const TStaticArray<FVector3f, 3>& Points);
};
