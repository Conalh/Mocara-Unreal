#include "MocaraPoseEditing.h"

namespace
{
	bool IsAnyOf(const FString& Name, std::initializer_list<const TCHAR*> Values)
	{
		for (const TCHAR* Value : Values)
		{
			if (Name == Value)
			{
				return true;
			}
		}
		return false;
	}

	bool IsMannyFinger(const FString& Name, const TCHAR* SideSuffix)
	{
		return Name.EndsWith(SideSuffix)
			&& (Name.StartsWith(TEXT("index_"))
				|| Name.StartsWith(TEXT("middle_"))
				|| Name.StartsWith(TEXT("ring_"))
				|| Name.StartsWith(TEXT("pinky_"))
				|| Name.StartsWith(TEXT("thumb_")));
	}
}

bool FMocaraPoseEditing::IsSelectableBone(FName BoneName)
{
	const FString Name = BoneName.ToString().ToLower();
	return !BoneName.IsNone()
		&& !Name.StartsWith(TEXT("ik_"))
		&& Name != TEXT("root")
		&& Name != TEXT("interaction")
		&& Name != TEXT("center_of_mass");
}

EMocaraPoseLane FMocaraPoseEditing::ClassifyBoneLane(FName BoneName)
{
	const FString Name = BoneName.ToString().ToLower();
	if (IsAnyOf(Name, {TEXT("root"), TEXT("hips"), TEXT("pelvis")}))
	{
		return EMocaraPoseLane::RootPath;
	}
	if (Name.StartsWith(TEXT("lefthand")) || Name == TEXT("hand_l") || IsMannyFinger(Name, TEXT("_l")))
	{
		return EMocaraPoseLane::LeftHand;
	}
	if (Name.StartsWith(TEXT("righthand")) || Name == TEXT("hand_r") || IsMannyFinger(Name, TEXT("_r")))
	{
		return EMocaraPoseLane::RightHand;
	}
	if (IsAnyOf(Name, {TEXT("leftfoot"), TEXT("lefttoebase"), TEXT("leftshin"), TEXT("foot_l"), TEXT("ball_l"), TEXT("calf_l")}))
	{
		return EMocaraPoseLane::LeftFoot;
	}
	if (IsAnyOf(Name, {TEXT("rightfoot"), TEXT("righttoebase"), TEXT("rightshin"), TEXT("foot_r"), TEXT("ball_r"), TEXT("calf_r")}))
	{
		return EMocaraPoseLane::RightFoot;
	}
	return EMocaraPoseLane::FullBody;
}

bool FMocaraPoseEditing::CanTranslateBone(FName BoneName)
{
	const FString Name = BoneName.ToString().ToLower();
	return IsAnyOf(Name,
		{TEXT("root"), TEXT("hips"), TEXT("pelvis"), TEXT("lefthand"), TEXT("righthand"),
			TEXT("leftfoot"), TEXT("rightfoot"), TEXT("hand_l"), TEXT("hand_r"), TEXT("foot_l"), TEXT("foot_r")});
}

FString FMocaraPoseEditing::DisplayLabel(FName BoneName)
{
	const FString Name = BoneName.ToString().ToLower();
	if (IsAnyOf(Name, {TEXT("hips"), TEXT("pelvis")})) return TEXT("Hips");
	if (IsAnyOf(Name, {TEXT("spine1"), TEXT("spine_01")})) return TEXT("Lower Spine");
	if (IsAnyOf(Name, {TEXT("spine2"), TEXT("spine_02")})) return TEXT("Mid Spine");
	if (IsAnyOf(Name, {TEXT("chest"), TEXT("spine3"), TEXT("spine_03")})) return TEXT("Chest");
	if (IsAnyOf(Name, {TEXT("neck1"), TEXT("neck_01")})) return TEXT("Lower Neck");
	if (IsAnyOf(Name, {TEXT("neck2"), TEXT("neck_02")})) return TEXT("Upper Neck");
	if (Name == TEXT("head")) return TEXT("Head");
	if (Name == TEXT("jaw")) return TEXT("Jaw");
	if (IsAnyOf(Name, {TEXT("leftshoulder"), TEXT("clavicle_l")})) return TEXT("Left Shoulder");
	if (IsAnyOf(Name, {TEXT("leftarm"), TEXT("upperarm_l")})) return TEXT("Left Upper Arm");
	if (IsAnyOf(Name, {TEXT("leftforearm"), TEXT("lowerarm_l")})) return TEXT("Left Forearm");
	if (IsAnyOf(Name, {TEXT("lefthand"), TEXT("hand_l")})) return TEXT("Left Hand");
	if (IsAnyOf(Name, {TEXT("rightshoulder"), TEXT("clavicle_r")})) return TEXT("Right Shoulder");
	if (IsAnyOf(Name, {TEXT("rightarm"), TEXT("upperarm_r")})) return TEXT("Right Upper Arm");
	if (IsAnyOf(Name, {TEXT("rightforearm"), TEXT("lowerarm_r")})) return TEXT("Right Forearm");
	if (IsAnyOf(Name, {TEXT("righthand"), TEXT("hand_r")})) return TEXT("Right Hand");
	if (IsAnyOf(Name, {TEXT("leftleg"), TEXT("thigh_l")})) return TEXT("Left Thigh");
	if (IsAnyOf(Name, {TEXT("leftshin"), TEXT("calf_l")})) return TEXT("Left Shin");
	if (IsAnyOf(Name, {TEXT("leftfoot"), TEXT("foot_l")})) return TEXT("Left Foot");
	if (IsAnyOf(Name, {TEXT("lefttoebase"), TEXT("ball_l")})) return TEXT("Left Toes");
	if (IsAnyOf(Name, {TEXT("rightleg"), TEXT("thigh_r")})) return TEXT("Right Thigh");
	if (IsAnyOf(Name, {TEXT("rightshin"), TEXT("calf_r")})) return TEXT("Right Shin");
	if (IsAnyOf(Name, {TEXT("rightfoot"), TEXT("foot_r")})) return TEXT("Right Foot");
	if (IsAnyOf(Name, {TEXT("righttoebase"), TEXT("ball_r")})) return TEXT("Right Toes");
	return BoneName.ToString();
}

int32 FMocaraPoseEditing::IntervalStartFrame(const FMocaraPoseKey& Key)
{
	return Key.Frame - FMath::Max(0, Key.EaseInFrames);
}

int32 FMocaraPoseEditing::IntervalEndFrame(const FMocaraPoseKey& Key)
{
	return Key.Frame + FMath::Max(1, Key.HoldFrames) - 1 + FMath::Max(0, Key.EaseOutFrames);
}

float FMocaraPoseEditing::EaseWeight(int32 Frame, const FMocaraPoseKey& Key)
{
	const int32 HoldEnd = Key.Frame + FMath::Max(1, Key.HoldFrames) - 1;
	if (Frame >= Key.Frame && Frame <= HoldEnd)
	{
		return 1.f;
	}

	const int32 EaseFrames = Frame < Key.Frame
		? FMath::Max(0, Key.EaseInFrames)
		: FMath::Max(0, Key.EaseOutFrames);
	if (EaseFrames <= 0)
	{
		return 0.f;
	}
	const float Distance = Frame < Key.Frame
		? static_cast<float>(Key.Frame - Frame)
		: static_cast<float>(Frame - HoldEnd);
	if (Distance >= EaseFrames)
	{
		return 0.f;
	}
	const float Alpha = 1.f - Distance / static_cast<float>(EaseFrames);
	return Alpha * Alpha * (3.f - 2.f * Alpha);
}

void FMocaraPoseEditing::CollectActiveFrames(
	const TArray<FMocaraPoseKey>& Keys,
	int32 NumFrames,
	TArray<int32>& OutFrames)
{
	OutFrames.Reset();
	if (Keys.IsEmpty() || NumFrames <= 0)
	{
		return;
	}

	TSet<int32> ActiveFrames;
	for (const FMocaraPoseKey& Key : Keys)
	{
		const int32 FirstFrame = FMath::Clamp(IntervalStartFrame(Key), 0, NumFrames - 1);
		const int32 LastFrame = FMath::Clamp(IntervalEndFrame(Key), 0, NumFrames - 1);
		for (int32 Frame = FirstFrame; Frame <= LastFrame; ++Frame)
		{
			if (EaseWeight(Frame, Key) > 0.f)
			{
				ActiveFrames.Add(Frame);
			}
		}
	}
	OutFrames.Reserve(ActiveFrames.Num());
	for (const int32 Frame : ActiveFrames)
	{
		OutFrames.Add(Frame);
	}
	OutFrames.Sort();
}

void FMocaraPoseEditing::ResizeInterval(
	FMocaraPoseKey& Key,
	EMocaraPoseIntervalHandle Handle,
	int32 DraggedFrame)
{
	switch (Handle)
	{
	case EMocaraPoseIntervalHandle::EaseInStart:
		Key.EaseInFrames = FMath::Max(0, Key.Frame - DraggedFrame);
		break;
	case EMocaraPoseIntervalHandle::HoldEnd:
		Key.HoldFrames = FMath::Max(1, DraggedFrame - Key.Frame);
		break;
	case EMocaraPoseIntervalHandle::EaseOutEnd:
		Key.EaseOutFrames = FMath::Max(0, DraggedFrame - (Key.Frame + FMath::Max(1, Key.HoldFrames)));
		break;
	default:
		break;
	}
}

void FMocaraPoseEditing::EvaluatePoseAtFrame(
	const TArray<FMocaraPoseKey>& Keys,
	int32 Frame,
	TMap<FName, FMocaraPoseKey>& OutPose)
{
	struct FAccumulatedPose
	{
		FQuat WeightedRotation = FQuat(0.f, 0.f, 0.f, 0.f);
		FVector WeightedTranslation = FVector::ZeroVector;
		float TotalWeight = 0.f;
		int32 EaseInFrames = 0;
		int32 HoldFrames = 1;
		int32 EaseOutFrames = 0;
		bool bUseTwoBoneIK = false;
		const FMocaraPoseKey* FullStrengthKey = nullptr;
	};

	OutPose.Reset();
	TMap<FName, FAccumulatedPose> AccumulatedPoses;
	for (const FMocaraPoseKey& Key : Keys)
	{
		const float Weight = EaseWeight(Frame, Key);
		if (Weight <= 0.f)
		{
			continue;
		}

		FAccumulatedPose& Accumulated = AccumulatedPoses.FindOrAdd(Key.BoneName);
		if (FMath::IsNearlyEqual(Weight, 1.f)
			&& (!Accumulated.FullStrengthKey || Key.Frame >= Accumulated.FullStrengthKey->Frame))
		{
			Accumulated.FullStrengthKey = &Key;
		}
		FQuat KeyRotation = Key.RotationOffset.Quaternion();
		const double Dot = Accumulated.WeightedRotation.X * KeyRotation.X
			+ Accumulated.WeightedRotation.Y * KeyRotation.Y
			+ Accumulated.WeightedRotation.Z * KeyRotation.Z
			+ Accumulated.WeightedRotation.W * KeyRotation.W;
		if (Accumulated.TotalWeight > 0.f && Dot < 0.0)
		{
			KeyRotation *= -1.0;
		}
		Accumulated.WeightedRotation.X += KeyRotation.X * Weight;
		Accumulated.WeightedRotation.Y += KeyRotation.Y * Weight;
		Accumulated.WeightedRotation.Z += KeyRotation.Z * Weight;
		Accumulated.WeightedRotation.W += KeyRotation.W * Weight;
		Accumulated.WeightedTranslation += Key.TranslationOffset * Weight;
		Accumulated.TotalWeight += Weight;
		Accumulated.EaseInFrames = FMath::Max(Accumulated.EaseInFrames, Key.EaseInFrames);
		Accumulated.HoldFrames = FMath::Max(Accumulated.HoldFrames, Key.HoldFrames);
		Accumulated.EaseOutFrames = FMath::Max(Accumulated.EaseOutFrames, Key.EaseOutFrames);
		Accumulated.bUseTwoBoneIK |= Key.bUseTwoBoneIK;
	}

	for (const TPair<FName, FAccumulatedPose>& Pair : AccumulatedPoses)
	{
		const FAccumulatedPose& Accumulated = Pair.Value;
		if (Accumulated.FullStrengthKey)
		{
			FMocaraPoseKey& Evaluated = OutPose.Add(Pair.Key, *Accumulated.FullStrengthKey);
			Evaluated.Frame = Frame;
			continue;
		}
		const float Influence = FMath::Min(Accumulated.TotalWeight, 1.f);
		FQuat MeanRotation = Accumulated.WeightedRotation;
		MeanRotation.Normalize();

		FMocaraPoseKey& Evaluated = OutPose.Add(Pair.Key);
		Evaluated.BoneName = Pair.Key;
		Evaluated.Frame = Frame;
		Evaluated.RotationOffset = FQuat::Slerp(FQuat::Identity, MeanRotation, Influence).Rotator().GetNormalized();
		Evaluated.TranslationOffset = Accumulated.WeightedTranslation / Accumulated.TotalWeight * Influence;
		Evaluated.EaseInFrames = Accumulated.EaseInFrames;
		Evaluated.HoldFrames = Accumulated.HoldFrames;
		Evaluated.EaseOutFrames = Accumulated.EaseOutFrames;
		Evaluated.bUseTwoBoneIK = Accumulated.bUseTwoBoneIK;
	}
}

int32 FMocaraPoseEditing::FindKey(const TArray<FMocaraPoseKey>& Keys, int32 Frame, FName BoneName)
{
	return Keys.IndexOfByPredicate([Frame, BoneName](const FMocaraPoseKey& Key)
	{
		return Key.Frame == Frame && Key.BoneName == BoneName;
	});
}

int32 FMocaraPoseEditing::FindNearestKey(
	const TArray<FMocaraPoseKey>& Keys,
	int32 Frame,
	FName BoneName)
{
	int32 BestIndex = INDEX_NONE;
	int32 BestDistance = MAX_int32;
	int32 BestFrame = MAX_int32;
	for (int32 Index = 0; Index < Keys.Num(); ++Index)
	{
		const FMocaraPoseKey& Key = Keys[Index];
		if (Key.BoneName != BoneName)
		{
			continue;
		}
		const int32 Distance = FMath::Abs(Key.Frame - Frame);
		if (Distance < BestDistance || (Distance == BestDistance && Key.Frame < BestFrame))
		{
			BestIndex = Index;
			BestDistance = Distance;
			BestFrame = Key.Frame;
		}
	}
	return BestIndex;
}

int32 FMocaraPoseEditing::UpsertRotationKey(
	TArray<FMocaraPoseKey>& Keys,
	int32 Frame,
	FName BoneName,
	const FRotator& RotationOffset,
	int32 EaseInFrames,
	int32 HoldFrames,
	int32 EaseOutFrames,
	bool bUseTwoBoneIK)
{
	const int32 ExistingIndex = FindKey(Keys, Frame, BoneName);
	const FVector TranslationOffset = ExistingIndex == INDEX_NONE
		? FVector::ZeroVector
		: Keys[ExistingIndex].TranslationOffset;
	return UpsertTransformKey(Keys, Frame, BoneName, RotationOffset, TranslationOffset,
		EaseInFrames, HoldFrames, EaseOutFrames, bUseTwoBoneIK);
}

int32 FMocaraPoseEditing::UpsertTransformKey(
	TArray<FMocaraPoseKey>& Keys,
	int32 Frame,
	FName BoneName,
	const FRotator& RotationOffset,
	const FVector& TranslationOffset,
	int32 EaseInFrames,
	int32 HoldFrames,
	int32 EaseOutFrames,
	bool bUseTwoBoneIK)
{
	int32 Index = FindKey(Keys, Frame, BoneName);
	if (Index == INDEX_NONE)
	{
		FMocaraPoseKey Key;
		Key.Frame = Frame;
		Key.BoneName = BoneName;
		Index = Keys.Add(Key);
	}

	FMocaraPoseKey& Key = Keys[Index];
	Key.RotationOffset = RotationOffset.GetNormalized();
	Key.TranslationOffset = TranslationOffset;
	Key.EaseInFrames = FMath::Max(0, EaseInFrames);
	Key.HoldFrames = FMath::Max(1, HoldFrames);
	Key.EaseOutFrames = FMath::Max(0, EaseOutFrames);
	Key.bUseTwoBoneIK = bUseTwoBoneIK;
	return Index;
}

int32 FMocaraPoseEditing::MoveKey(TArray<FMocaraPoseKey>& Keys, int32 KeyIndex, int32 NewFrame)
{
	if (!Keys.IsValidIndex(KeyIndex))
	{
		return INDEX_NONE;
	}
	const FName BoneName = Keys[KeyIndex].BoneName;
	const int32 Collision = Keys.IndexOfByPredicate([KeyIndex, NewFrame, BoneName, &Keys](const FMocaraPoseKey& Key)
	{
		return &Key != &Keys[KeyIndex] && Key.Frame == NewFrame && Key.BoneName == BoneName;
	});
	if (Collision != INDEX_NONE)
	{
		Keys.RemoveAt(Collision);
		if (Collision < KeyIndex)
		{
			--KeyIndex;
		}
	}
	Keys[KeyIndex].Frame = NewFrame;
	return KeyIndex;
}

int32 FMocaraPoseEditing::DuplicateKey(TArray<FMocaraPoseKey>& Keys, int32 KeyIndex, int32 NewFrame)
{
	if (!Keys.IsValidIndex(KeyIndex))
	{
		return INDEX_NONE;
	}
	const FMocaraPoseKey Copy = Keys[KeyIndex];
	const int32 Collision = FindKey(Keys, NewFrame, Copy.BoneName);
	if (Collision != INDEX_NONE)
	{
		Keys[Collision] = Copy;
		Keys[Collision].Frame = NewFrame;
		return Collision;
	}
	FMocaraPoseKey& Added = Keys.Add_GetRef(Copy);
	Added.Frame = NewFrame;
	return Keys.Num() - 1;
}

FRotator FMocaraPoseEditing::AccumulateRotation(const FRotator& Current, const FRotator& Delta, bool bLocalSpace)
{
	const FQuat CurrentQuat = Current.Quaternion();
	const FQuat DeltaQuat = Delta.Quaternion();
	FQuat Result = bLocalSpace ? CurrentQuat * DeltaQuat : DeltaQuat * CurrentQuat;
	Result.Normalize();
	return Result.Rotator().GetNormalized();
}
