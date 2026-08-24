#pragma once

#include "CoreMinimal.h"
#include "MocaraTypes.h"

enum class EMocaraPoseLane : uint8
{
	RootPath,
	FullBody,
	LeftHand,
	RightHand,
	LeftFoot,
	RightFoot
};

/** Pure editing operations shared by the Slate controls and viewport manipulator. */
struct MOCARAEDITOR_API FMocaraPoseEditing
{
	static bool IsSelectableBone(FName BoneName);
	static bool CanTranslateBone(FName BoneName);
	static EMocaraPoseLane ClassifyBoneLane(FName BoneName);
	/** User-facing label only. Bone identifiers remain unchanged for editing and serialization. */
	static FString DisplayLabel(FName BoneName);
	static int32 IntervalStartFrame(const FMocaraPoseKey& Key);
	static int32 IntervalEndFrame(const FMocaraPoseKey& Key);
	static float EaseWeight(int32 Frame, const FMocaraPoseKey& Key);
	static void CollectActiveFrames(
		const TArray<FMocaraPoseKey>& Keys,
		int32 NumFrames,
		TArray<int32>& OutFrames);
	static void ResizeInterval(
		FMocaraPoseKey& Key,
		EMocaraPoseIntervalHandle Handle,
		int32 DraggedFrame);
	static void EvaluatePoseAtFrame(
		const TArray<FMocaraPoseKey>& Keys,
		int32 Frame,
		TMap<FName, FMocaraPoseKey>& OutPose);
	static int32 FindKey(const TArray<FMocaraPoseKey>& Keys, int32 Frame, FName BoneName);
	/** Finds the closest key for a bone, preferring the earlier frame on a tie. */
	static int32 FindNearestKey(const TArray<FMocaraPoseKey>& Keys, int32 Frame, FName BoneName);
	static int32 UpsertRotationKey(
		TArray<FMocaraPoseKey>& Keys,
		int32 Frame,
		FName BoneName,
		const FRotator& RotationOffset,
		int32 EaseInFrames,
		int32 HoldFrames,
		int32 EaseOutFrames,
		bool bUseTwoBoneIK);
	static int32 UpsertTransformKey(
		TArray<FMocaraPoseKey>& Keys,
		int32 Frame,
		FName BoneName,
		const FRotator& RotationOffset,
		const FVector& TranslationOffset,
		int32 EaseInFrames,
		int32 HoldFrames,
		int32 EaseOutFrames,
		bool bUseTwoBoneIK);
	static int32 MoveKey(TArray<FMocaraPoseKey>& Keys, int32 KeyIndex, int32 NewFrame);
	static int32 DuplicateKey(TArray<FMocaraPoseKey>& Keys, int32 KeyIndex, int32 NewFrame);
	static FRotator AccumulateRotation(const FRotator& Current, const FRotator& Delta, bool bLocalSpace);
};
