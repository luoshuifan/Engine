#include "GSScene.h"
#include "Rendering/GaussianSplattingResources.h"

namespace GS
{
	bool FGSScene::bInitGaussianResources = false;
	
	static FMatrix44f QuatToRotmat(const FVector4f& Quat)
	{
		float s = FMath::InvSqrt(Quat.W * Quat.W + Quat.X * Quat.X + Quat.Y * Quat.Y + Quat.Z * Quat.Z);
		float w = Quat.X * s;
		float x = Quat.Y * s;
		float y = Quat.Z * s;
		float z = Quat.W * s;

		return FMatrix44f(
			FVector3f(1.0f- 2.0f * (y * y + z * z), 2.0f * (x * y + w * z),          2.0f * (x * z - w * y)),
			FVector3f(2.0f * (x * y - w * z),         1.0f - 2.0f * (x * x + z * z),  2.0f * (y * z + w * x)),
			FVector3f(2.0f * (x * z + w * y),        2.0f * (y * z - w * x),           1.0f - 2.0f * (x * x + y * y)),
			FVector3f(0,                                      0,                                       0));
	}

	static FMatrix44f ScaleToMat(const FVector2f& Scale)
	{
		return FMatrix44f(
			FVector3f(Scale.X, 0,         0),
			FVector3f(0,         Scale.Y, 0),
			FVector3f(0,          0,         1),
			FVector3f(0,          0,         0));
	}

	
	void FGSScene::LoadPrimitives()
	{
		static const FString PointCloudPath = TEXT("G:\\APP\\2d-gaussian-splatting\\output\\point_cloud\\iteration_30000\\point_cloud.bin");

		if(Gaussians.Num() > 0)
			return;

		TArray<uint8> BinaryData;
		if(!FFileHelper::LoadFileToArray(BinaryData, *PointCloudPath))
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to load point cloud data from %s"), *PointCloudPath);
			return;
		}

		TArray<float> PlyData;
		float PlyDataNum = BinaryData.Num() / sizeof(float);
		PlyData.SetNum(PlyDataNum);
		FMemory::Memcpy(PlyData.GetData(), BinaryData.GetData(), BinaryData.Num());

		int32 PrimitiveCount = PlyData[0];
		int32 MaxSHDegree = PlyData[1];

		int32 TmpOffset = (PlyData.Num() - 2) / PrimitiveCount;

		Primitives.AddDefaulted(1);
		Gaussians.Add({PrimitiveCount, MaxSHDegree});

		FGSPrimitive& Primitive = Primitives[0];

		uint32 PrimitiveOffset = 2;
		Primitive.Position.AddUninitialized(PrimitiveCount);
		FMemory::Memcpy(Primitive.Position.GetData(), &PlyData[PrimitiveOffset], sizeof(FVector3f) * PrimitiveCount);
		PrimitiveOffset += sizeof(FVector3f) / sizeof(float) * PrimitiveCount;

		Primitive.ColorSH.AddUninitialized(PrimitiveCount * 16);
		FMemory::Memcpy(Primitive.ColorSH.GetData(), &PlyData[PrimitiveOffset], sizeof(FVector3f) * PrimitiveCount * 16);
		PrimitiveOffset += sizeof(FVector3f) / sizeof(float) * PrimitiveCount * 16;

		Primitive.Opacity.AddUninitialized(PrimitiveCount);
		FMemory::Memcpy(Primitive.Opacity.GetData(), &PlyData[PrimitiveOffset], sizeof(float) * PrimitiveCount);
		PrimitiveOffset += sizeof(float) / sizeof(float) * PrimitiveCount;

		Primitive.Scale.AddUninitialized(PrimitiveCount);
		FMemory::Memcpy(Primitive.Scale.GetData(), &PlyData[PrimitiveOffset], sizeof(FVector2f) * PrimitiveCount);
		PrimitiveOffset += sizeof(FVector2f) / sizeof(float)* PrimitiveCount;

		Primitive.Quat.AddUninitialized(PrimitiveCount);
		FMemory::Memcpy(Primitive.Quat.GetData(), &PlyData[PrimitiveOffset], sizeof(FVector4f) * PrimitiveCount);
		PrimitiveOffset += sizeof(FVector4f) / sizeof(float) * PrimitiveCount;
	}

	void FGSScene::PrimitivesToResources()
	{
		if(bInitGaussianResources)
			return;

		for (int32 Index = 0; Index < Gaussians.Num(); ++Index)
		{
			FGaussianProperty& Property = Gaussians[Index];
			FGSPrimitive& Primitive = Primitives[Index];
			TArray<FPackedLeafNode> LeafNodes;
			FResources Resources;

			LeafNodes.AddUninitialized(Property.NumGaussian);
			Resources.RootData.AddUninitialized(Primitive.GetSize());
			Resources.HierarchyNodes.AddUninitialized(Property.NumGaussian);
			Resources.NumRootPages = 1;

			for (int32 SubIndex = 0; SubIndex < Property.NumGaussian; ++SubIndex)
			{
				FVector3f Position = Primitive.Position[SubIndex];
				FMatrix44f R = QuatToRotmat(Primitive.Quat[SubIndex]);
				FMatrix44f S = ScaleToMat(Primitive.Scale[SubIndex]);
				FMatrix44f H = R * S;

				FVector2f Bound {GAUSSIANSPLATTING_CUTOFF * GAUSSIANSPLATTING_CUTOFF, GAUSSIANSPLATTING_CUTOFF * GAUSSIANSPLATTING_CUTOFF};
				float Radii = (Bound.X * H.GetColumn(0) + Bound.Y * H.GetColumn(1)).Length();

				FPackedHierarchyNode& HierarchyNode = Resources.HierarchyNodes[SubIndex];
				HierarchyNode.LODBounds[0] = FVector4f(Position, Radii);

				FPackedLeafNode& LeafNode = LeafNodes[SubIndex];
				LeafNode.Position = Primitive.Position[SubIndex];
				LeafNode.Opacity = Primitive.Opacity[SubIndex];
				LeafNode.Scale = Primitive.Scale[SubIndex];
				LeafNode.Quat = Primitive.Quat[SubIndex];
				LeafNode.ColorSH[0] = Primitive.ColorSH[SubIndex];
				//LeafNode.ColorSH
				FMemory::Memcpy(&(LeafNode.ColorSH[1]), &(Primitive.ColorSH[Property.NumGaussian + SubIndex * 15]), Primitive.ColorSH.GetTypeSize() * 15);

				int32 a = 0;
			}

			uint32 RootDataSize = LeafNodes.GetTypeSize() * LeafNodes.Num();

			FMemory::Memcpy(Resources.RootData.GetData(), LeafNodes.GetData(), RootDataSize);

			Resources.PageStreamingState.Add({0,  RootDataSize, RootDataSize, 0});

			Resources.InitResources(nullptr);
		}

		bInitGaussianResources = true;
	}

}