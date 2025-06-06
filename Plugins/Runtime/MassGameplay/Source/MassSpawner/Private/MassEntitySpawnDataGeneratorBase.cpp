﻿// Copyright Epic Games, Inc. All Rights Reserved.

#include "MassEntitySpawnDataGeneratorBase.h"
#include "VisualLogger/VisualLogger.h"
#include "MassSpawnerTypes.h"

void UMassEntitySpawnDataGeneratorBase::BuildResultsFromEntityTypes(const int32 SpawnCount, TConstArrayView<FMassSpawnedEntityType> EntityTypes, TArray<FMassEntitySpawnDataGeneratorResult>& OutResults) const
{
	float TotalProportion = 0.0f;
	for (const FMassSpawnedEntityType& EntityType : EntityTypes)
	{
		TotalProportion += EntityType.Proportion;
	}

	if (TotalProportion <= 0)
	{
		UE_VLOG_UELOG(this, LogMassSpawner, Error, TEXT("The total combined proportion of all the entity types needs to be greater than 0."));
		return;
	}

	for (int32 i = 0; i < EntityTypes.Num(); i++)
	{
		const FMassSpawnedEntityType& EntityType = EntityTypes[i];
		const int32 EntityCount = static_cast<int32>(static_cast<float>(SpawnCount) * EntityType.Proportion / TotalProportion);
		if (EntityCount > 0 && EntityType.GetEntityConfig() != nullptr)
		{
			FMassEntitySpawnDataGeneratorResult& Res = OutResults.AddDefaulted_GetRef();
			Res.NumEntities = EntityCount;
			Res.EntityConfigIndex = i;
		}
	}
}