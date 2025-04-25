// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	PSOPrecache.h
=============================================================================*/

#pragma once

#include "PSOPrecache.h"

struct FSceneTexturesConfig;

/**
* Predeclared global PSOCollector function
*/
typedef void (*GlobalPSOCollectorFunction)(const FSceneTexturesConfig& SceneTexturesConfig, int32 GlobalPSOCollectorIndex, TArray<FPSOPrecacheData>& PSOInitializers);

/**
* Manages all collect functions of the globally declared PSOCollectorCreateFunction
*/
class FGlobalPSOCollectorManager
{
public:
	constexpr static uint32 MaxPSOCollectorCount = 4;

	static int32 GetPSOCollectorCount() { return PSOCollectorCount; }
	static GlobalPSOCollectorFunction GetCollectFunction(int32 Index)
	{
		check(Index < MaxPSOCollectorCount);
		return PSOCollectors[Index].CollectFunction;
	}
	static const TCHAR* GetName(int32 Index)
	{
		if (Index == INDEX_NONE)
		{
			return TEXT("Unknown");
		}
		check(Index < MaxPSOCollectorCount);
		return PSOCollectors[Index].Name;
	}
	static ENGINE_API int32 GetIndex(const TCHAR* Name);

private:

	// Have to used fixed size array instead of TArray because of order of initialization of static member variables
	static inline int32 PSOCollectorCount = 0;

	struct FPSOCollectorData
	{
		GlobalPSOCollectorFunction CollectFunction;
		const TCHAR* Name = nullptr;
	};
	static ENGINE_API FPSOCollectorData PSOCollectors[MaxPSOCollectorCount];

	friend class FRegisterGlobalPSOCollectorFunction;
};

/**
* Helper class used to register/unregister the GlobalPSOCollectorFunction to the manager at static startup time
*/
class FRegisterGlobalPSOCollectorFunction
{
public:
	FRegisterGlobalPSOCollectorFunction(GlobalPSOCollectorFunction InCollectFunction, const TCHAR* InName)
	{
		Index = FGlobalPSOCollectorManager::PSOCollectorCount;
		check(Index < FGlobalPSOCollectorManager::MaxPSOCollectorCount);

		FGlobalPSOCollectorManager::PSOCollectors[Index].CollectFunction = InCollectFunction;
		FGlobalPSOCollectorManager::PSOCollectors[Index].Name = InName;
		FGlobalPSOCollectorManager::PSOCollectorCount++;
	}

	~FRegisterGlobalPSOCollectorFunction()
	{
		FGlobalPSOCollectorManager::PSOCollectors[Index].CollectFunction = nullptr;
		FGlobalPSOCollectorManager::PSOCollectors[Index].Name = nullptr;
	}

	int32 GetIndex() const { return Index; }

private:
	uint32 Index;
};

/**
 * Start the actual async PSO precache request from the given list of initializers
 */
extern ENGINE_API FPSOPrecacheRequestResultArray RequestPrecachePSOs(const FPSOPrecacheDataArray& PSOInitializers);
