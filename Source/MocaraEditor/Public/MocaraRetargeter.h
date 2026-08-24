#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;
class UAnimSequence;
class UIKRigDefinition;
class UIKRetargeter;
struct FMocaraTargetProfile;

class FMocaraRetargeter
{
public:
	/** Resolve and validate one explicitly selected target mesh without falling back. */
	static USkeletalMesh* ResolveTargetMesh(const FSoftObjectPath& TargetPath, FMocaraTargetProfile& OutProfile, FString& OutError);
	/** Resolve the configured target, then the legacy/default mannequin candidates. */
	static USkeletalMesh* FindTargetMesh(FMocaraTargetProfile& OutProfile, FString& OutError);
	/** Stable, path-qualified identifier used to isolate generated rigs and animations per target. */
	static FName MakeTargetAssetId(const USkeletalMesh* Mesh, const FMocaraTargetProfile& Profile);
	/** Backward-compatible alias used by existing verification commands. */
	static USkeletalMesh* FindMannyMesh();
	static UIKRigDefinition* EnsureIkRig(USkeletalMesh* Mesh, const FString& PackagePath, const FName AssetName, bool bSoma);
	static UIKRetargeter* EnsureRetargeter(UIKRigDefinition* SourceRig, UIKRigDefinition* TargetRig, const FString& PackagePath, const FName AssetName);
	static UAnimSequence* Retarget(UAnimSequence* SourceSeq, USkeletalMesh* SourceMesh, USkeletalMesh* TargetMesh, const FMocaraTargetProfile& TargetProfile, UIKRetargeter* Retargeter, const FString& DestinationPath, const FString& NewName, FString& OutError);
};
