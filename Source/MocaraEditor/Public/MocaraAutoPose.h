#pragma once

#include "CoreMinimal.h"
#include "MocaraTypes.h"
#include "Dom/JsonValue.h"

class UAnimSequence;
class USkeletalMesh;

/** Two wrist targets expressed in the animation's component space. */
struct MOCARAEDITOR_API FMocaraTwoHandGripPose
{
	FTransform LeftHand;
	FTransform RightHand;
};

class FMocaraAutoPose
{
public:
	static float EaseWeight(int32 Frame, int32 KeyFrame, int32 EaseFrames);
	static bool ApplyLocalKeys(UAnimSequence* Sequence, const TArray<FMocaraPoseKey>& Keys, bool bTwoBoneIK, FString& OutError);
	static bool BuildConstraintsJson(UAnimSequence* SomaSequence, const TArray<FMocaraPoseKey>& Keys, TArray<TSharedPtr<FJsonValue>>& OutConstraints, FString& OutError);
	static FMocaraTwoHandGripPose BuildTwoHandGripPose(
		const FTransform& FirstLeftHand,
		const FTransform& FirstRightHand,
		const FTransform& CurrentLeftHand,
		const FTransform& CurrentRightHand,
		float GripSpacingCm);
	/** Map an imported SOMA skeleton to the 30/77-joint layout accepted by Kimodo constraints. */
	static bool ResolveKimodoConstraintBoneIndices(
		const TArray<FName>& UnrealBoneNames,
		TArray<int32>& OutBoneIndices,
		FString& OutError);
	static bool BuildTwoHandGripConstraintsJson(
		UAnimSequence* SomaSequence,
		TArray<TSharedPtr<FJsonValue>>& OutConstraints,
		FString& OutError,
		float GripSpacingCm = 0.f,
		int32 SampleIntervalFrames = 4);
	static void SolveTwoBoneIK(FTransform& Root, FTransform& Joint, FTransform& Effector, const FVector& Target);
};
