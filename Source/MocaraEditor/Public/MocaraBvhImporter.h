#pragma once

#include "CoreMinimal.h"

class USkeleton;
class USkeletalMesh;
class UAnimSequence;

struct FMocaraBvhJoint
{
	FName Name;
	int32 ParentIndex = INDEX_NONE;
	FVector OffsetYUp = FVector::ZeroVector;
	TArray<FString> Channels;
	int32 ChannelOffset = 0;
};

struct FMocaraBvhFile
{
	TArray<FMocaraBvhJoint> Joints;
	int32 NumFrames = 0;
	float FrameTime = 1.0f / 30.0f;
	TArray<TArray<float>> FrameValues;

	bool Load(const FString& Filename, FString& OutError);
};

struct FMocaraImportedClip
{
	USkeleton* Skeleton = nullptr;
	USkeletalMesh* Mesh = nullptr;
	UAnimSequence* Sequence = nullptr;
	/** True unless an enabled auto-save operation failed for any generated asset. */
	bool bAllAssetsSaved = true;
};

class FMocaraBvhImporter
{
public:
	static bool ImportFile(const FString& Filename, const FString& DestinationPath, const FString& AssetBaseName, bool bInPlace, FMocaraImportedClip& OutClip, FString& OutError, bool bSaveAssets = true);

	/** SOMA/Mixamo Y-up: X=left, Y=up, Z=forward. Unreal: X=forward, Y=right, Z=up. */
	static FVector YUpToZUp(const FVector& V) { return FVector(V.Z, -V.X, V.Y); }
	static FVector ZUpToYUp(const FVector& V) { return FVector(-V.Y, V.Z, V.X); }
	static FQuat ConvertQuatYUpToZUp(const FQuat& Q);

	/**
	 * Write an asset's package to disk. Generated clips are only marked dirty otherwise,
	 * so closing the editor without Save All loses them.
	 */
	static bool SaveGeneratedAsset(UObject* Asset);
	static FQuat ConvertQuatZUpToYUp(const FQuat& Q);
};
