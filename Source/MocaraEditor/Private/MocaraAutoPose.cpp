#include "MocaraAutoPose.h"
#include "MocaraBoneMap.h"
#include "MocaraBvhImporter.h"
#include "MocaraPoseEditing.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "ReferenceSkeleton.h"
#include "TwoBoneIK.h"

float FMocaraAutoPose::EaseWeight(int32 Frame, int32 KeyFrame, int32 EaseFrames)
{
	FMocaraPoseKey Key;
	Key.Frame = KeyFrame;
	Key.EaseInFrames = EaseFrames;
	Key.EaseOutFrames = EaseFrames;
	return FMocaraPoseEditing::EaseWeight(Frame, Key);
}

void FMocaraAutoPose::SolveTwoBoneIK(FTransform& Root, FTransform& Joint, FTransform& Effector, const FVector& Target)
{
	const FVector Chain = Effector.GetLocation() - Root.GetLocation();
	const FVector Along = Root.GetLocation()
		+ Chain * FVector::DotProduct(Joint.GetLocation() - Root.GetLocation(), Chain)
			/ FMath::Max(Chain.SizeSquared(), UE_SMALL_NUMBER);
	FVector Bend = Joint.GetLocation() - Along;
	if (!Bend.Normalize())
	{
		Bend = FVector::CrossProduct(Chain, FVector::UpVector).GetSafeNormal();
	}
	AnimationCore::SolveTwoBoneIK(
		Root, Joint, Effector, Joint.GetLocation() + Bend * 100.f, Target, false, 1.0, 1.0);
}

FMocaraTwoHandGripPose FMocaraAutoPose::BuildTwoHandGripPose(
	const FTransform& FirstLeftHand,
	const FTransform& FirstRightHand,
	const FTransform& CurrentLeftHand,
	const FTransform& CurrentRightHand,
	float GripSpacingCm)
{
	FVector InitialAxis = FirstLeftHand.GetLocation() - FirstRightHand.GetLocation();
	if (!InitialAxis.Normalize())
	{
		InitialAxis = FVector::RightVector;
	}

	const FQuat DominantHandDelta = (
		CurrentRightHand.GetRotation() * FirstRightHand.GetRotation().Inverse()).GetNormalized();
	FVector GripAxis = DominantHandDelta.RotateVector(InitialAxis);
	if (!GripAxis.Normalize())
	{
		GripAxis = InitialAxis;
	}

	const FVector Midpoint =
		(CurrentLeftHand.GetLocation() + CurrentRightHand.GetLocation()) * 0.5f;
	const float SourceSpacing = FVector::Distance(
		FirstLeftHand.GetLocation(), FirstRightHand.GetLocation());
	const float ResolvedSpacing = GripSpacingCm > 0.f ? GripSpacingCm : SourceSpacing;
	const float HalfSpacing = FMath::Max(1.f, ResolvedSpacing) * 0.5f;

	FMocaraTwoHandGripPose Result;
	Result.RightHand = CurrentRightHand;
	Result.RightHand.SetLocation(Midpoint - GripAxis * HalfSpacing);
	Result.LeftHand = CurrentLeftHand;
	Result.LeftHand.SetLocation(Midpoint + GripAxis * HalfSpacing);
	Result.LeftHand.SetRotation(
		(DominantHandDelta * FirstLeftHand.GetRotation()).GetNormalized());
	return Result;
}

bool FMocaraAutoPose::ResolveKimodoConstraintBoneIndices(
	const TArray<FName>& UnrealBoneNames,
	TArray<int32>& OutBoneIndices,
	FString& OutError)
{
	OutBoneIndices.Reset();
	OutError.Reset();
	int32 FirstSomaBone = 0;
	if ((UnrealBoneNames.Num() == 31 || UnrealBoneNames.Num() == 78)
		&& UnrealBoneNames[0] == TEXT("Root")
		&& UnrealBoneNames[1] == TEXT("Hips"))
	{
		// Kimodo's standard-T-pose BVH exporter adds a zeroed transport root above
		// Hips. Unreal retains it in the imported skeleton, but Kimodo constraints
		// must contain only the underlying SOMA30/SOMA77 joints.
		FirstSomaBone = 1;
	}

	const int32 SomaBoneCount = UnrealBoneNames.Num() - FirstSomaBone;
	if ((SomaBoneCount != 30 && SomaBoneCount != 77)
		|| !UnrealBoneNames.IsValidIndex(FirstSomaBone)
		|| UnrealBoneNames[FirstSomaBone] != TEXT("Hips"))
	{
		OutError = FString::Printf(
			TEXT("Generated skeleton has %d bones; Kimodo constraints require SOMA30 or SOMA77, with only an optional Unreal Root above Hips."),
			UnrealBoneNames.Num());
		return false;
	}

	OutBoneIndices.Reserve(SomaBoneCount);
	for (int32 BoneIndex = FirstSomaBone; BoneIndex < UnrealBoneNames.Num(); ++BoneIndex)
	{
		OutBoneIndices.Add(BoneIndex);
	}
	return true;
}

bool FMocaraAutoPose::BuildTwoHandGripConstraintsJson(
	UAnimSequence* SomaSequence,
	TArray<TSharedPtr<FJsonValue>>& OutConstraints,
	FString& OutError,
	float GripSpacingCm,
	int32 SampleIntervalFrames)
{
	OutConstraints.Reset();
	if (!SomaSequence || !SomaSequence->GetDataModel() || !SomaSequence->GetSkeleton())
	{
		OutError = TEXT("Generate a SOMA clip before applying the two-hand grip preset.");
		return false;
	}

	const IAnimationDataModel* Model = SomaSequence->GetDataModel();
	const int32 NumKeys = Model->GetNumberOfKeys();
	if (NumKeys <= 0)
	{
		OutError = TEXT("The SOMA clip has no animation frames.");
		return false;
	}

	const FReferenceSkeleton& Ref = SomaSequence->GetSkeleton()->GetReferenceSkeleton();
	TArray<FName> RefBoneNames;
	RefBoneNames.Reserve(Ref.GetNum());
	for (int32 BoneIndex = 0; BoneIndex < Ref.GetNum(); ++BoneIndex)
	{
		RefBoneNames.Add(Ref.GetBoneName(BoneIndex));
	}
	TArray<int32> KimodoBoneIndices;
	if (!ResolveKimodoConstraintBoneIndices(RefBoneNames, KimodoBoneIndices, OutError))
	{
		return false;
	}
	const int32 LeftRootIndex = Ref.FindBoneIndex(TEXT("LeftArm"));
	const int32 LeftJointIndex = Ref.FindBoneIndex(TEXT("LeftForeArm"));
	const int32 LeftHandIndex = Ref.FindBoneIndex(TEXT("LeftHand"));
	const int32 RightRootIndex = Ref.FindBoneIndex(TEXT("RightArm"));
	const int32 RightJointIndex = Ref.FindBoneIndex(TEXT("RightForeArm"));
	const int32 RightHandIndex = Ref.FindBoneIndex(TEXT("RightHand"));
	int32 HipsIndex = Ref.FindBoneIndex(TEXT("Hips"));
	if (HipsIndex == INDEX_NONE)
	{
		HipsIndex = Ref.FindBoneIndex(TEXT("Root"));
	}
	if (LeftRootIndex == INDEX_NONE || LeftJointIndex == INDEX_NONE || LeftHandIndex == INDEX_NONE
		|| RightRootIndex == INDEX_NONE || RightJointIndex == INDEX_NONE || RightHandIndex == INDEX_NONE
		|| HipsIndex == INDEX_NONE)
	{
		OutError = TEXT("The generated SOMA skeleton is missing an arm, hand, or hips bone required by the grip preset.");
		return false;
	}
	if (Ref.GetParentIndex(LeftHandIndex) != LeftJointIndex
		|| Ref.GetParentIndex(LeftJointIndex) != LeftRootIndex
		|| Ref.GetParentIndex(RightHandIndex) != RightJointIndex
		|| Ref.GetParentIndex(RightJointIndex) != RightRootIndex)
	{
		OutError = TEXT("The SOMA arm hierarchy does not match the two-bone grip preset.");
		return false;
	}

	TArray<FName> TrackNames;
	Model->GetBoneTrackNames(TrackNames);
	TSet<FName> TrackSet;
	for (const FName BoneName : TrackNames)
	{
		TrackSet.Add(BoneName);
	}

	auto EvaluateFrame = [&Model, &Ref, &TrackSet](
		int32 Frame,
		TArray<FTransform>& OutLocalPose,
		TArray<FTransform>& OutComponentPose)
	{
		OutLocalPose = Ref.GetRefBonePose();
		OutComponentPose.SetNum(Ref.GetNum());
		for (int32 BoneIndex = 0; BoneIndex < Ref.GetNum(); ++BoneIndex)
		{
			const FName BoneName = Ref.GetBoneName(BoneIndex);
			if (TrackSet.Contains(BoneName))
			{
				OutLocalPose[BoneIndex] = Model->EvaluateBoneTrackTransform(
					BoneName, FFrameTime(Frame), EAnimInterpolationType::Step);
			}
			const int32 ParentIndex = Ref.GetParentIndex(BoneIndex);
			OutComponentPose[BoneIndex] = ParentIndex == INDEX_NONE
				? OutLocalPose[BoneIndex]
				: OutLocalPose[BoneIndex] * OutComponentPose[ParentIndex];
		}
	};

	TArray<FTransform> FirstLocalPose;
	TArray<FTransform> FirstComponentPose;
	EvaluateFrame(0, FirstLocalPose, FirstComponentPose);
	const FTransform FirstLeftHand = FirstComponentPose[LeftHandIndex];
	const FTransform FirstRightHand = FirstComponentPose[RightHandIndex];

	TArray<int32> Frames;
	const int32 Interval = FMath::Max(1, SampleIntervalFrames);
	for (int32 Frame = 0; Frame < NumKeys; Frame += Interval)
	{
		Frames.Add(Frame);
	}
	if (Frames.Last() != NumKeys - 1)
	{
		Frames.Add(NumKeys - 1);
	}

	TArray<TSharedPtr<FJsonValue>> FrameValues;
	TArray<TSharedPtr<FJsonValue>> RootPositions;
	TArray<TSharedPtr<FJsonValue>> LocalRotations;
	for (const int32 Frame : Frames)
	{
		TArray<FTransform> LocalPose;
		TArray<FTransform> ComponentPose;
		EvaluateFrame(Frame, LocalPose, ComponentPose);
		const FMocaraTwoHandGripPose Grip = BuildTwoHandGripPose(
			FirstLeftHand,
			FirstRightHand,
			ComponentPose[LeftHandIndex],
			ComponentPose[RightHandIndex],
			GripSpacingCm);

		auto SolveArm = [&Ref, &LocalPose, &ComponentPose](
			int32 RootIndex,
			int32 JointIndex,
			int32 HandIndex,
			const FTransform& HandTarget)
		{
			FTransform& Root = ComponentPose[RootIndex];
			FTransform& Joint = ComponentPose[JointIndex];
			FTransform& Hand = ComponentPose[HandIndex];
			FMocaraAutoPose::SolveTwoBoneIK(Root, Joint, Hand, HandTarget.GetLocation());
			Hand.SetRotation(HandTarget.GetRotation());

			const int32 RootParentIndex = Ref.GetParentIndex(RootIndex);
			LocalPose[RootIndex] = RootParentIndex == INDEX_NONE
				? Root : Root.GetRelativeTransform(ComponentPose[RootParentIndex]);
			LocalPose[JointIndex] = Joint.GetRelativeTransform(Root);
			LocalPose[HandIndex] = Hand.GetRelativeTransform(Joint);
		};

		SolveArm(RightRootIndex, RightJointIndex, RightHandIndex, Grip.RightHand);
		SolveArm(LeftRootIndex, LeftJointIndex, LeftHandIndex, Grip.LeftHand);

		FrameValues.Add(MakeShared<FJsonValueNumber>(Frame));
		const FVector RootYUpMeters =
			FMocaraBvhImporter::ZUpToYUp(ComponentPose[HipsIndex].GetLocation()) * 0.01f;
		TArray<TSharedPtr<FJsonValue>> RootTriple;
		RootTriple.Add(MakeShared<FJsonValueNumber>(RootYUpMeters.X));
		RootTriple.Add(MakeShared<FJsonValueNumber>(RootYUpMeters.Y));
		RootTriple.Add(MakeShared<FJsonValueNumber>(RootYUpMeters.Z));
		RootPositions.Add(MakeShared<FJsonValueArray>(RootTriple));

		TArray<TSharedPtr<FJsonValue>> JointRotations;
		for (const int32 BoneIndex : KimodoBoneIndices)
		{
			const FTransform& LocalTransform = LocalPose[BoneIndex];
			FVector Axis;
			float Angle;
			FMocaraBvhImporter::ConvertQuatZUpToYUp(LocalTransform.GetRotation())
				.ToAxisAndAngle(Axis, Angle);
			const FVector AxisAngle = Axis.GetSafeNormal() * Angle;
			TArray<TSharedPtr<FJsonValue>> Triple;
			Triple.Add(MakeShared<FJsonValueNumber>(AxisAngle.X));
			Triple.Add(MakeShared<FJsonValueNumber>(AxisAngle.Y));
			Triple.Add(MakeShared<FJsonValueNumber>(AxisAngle.Z));
			JointRotations.Add(MakeShared<FJsonValueArray>(Triple));
		}
		LocalRotations.Add(MakeShared<FJsonValueArray>(JointRotations));
	}

	TSharedRef<FJsonObject> Constraint = MakeShared<FJsonObject>();
	Constraint->SetStringField(TEXT("type"), TEXT("end-effector"));
	Constraint->SetArrayField(TEXT("frame_indices"), FrameValues);
	Constraint->SetArrayField(TEXT("root_positions"), RootPositions);
	Constraint->SetArrayField(TEXT("local_joints_rot"), LocalRotations);
	TArray<TSharedPtr<FJsonValue>> JointNames;
	JointNames.Add(MakeShared<FJsonValueString>(TEXT("LeftHand")));
	JointNames.Add(MakeShared<FJsonValueString>(TEXT("RightHand")));
	Constraint->SetArrayField(TEXT("joint_names"), JointNames);
	OutConstraints.Add(MakeShared<FJsonValueObject>(Constraint));
	return true;
}

bool FMocaraAutoPose::ApplyLocalKeys(UAnimSequence* Sequence, const TArray<FMocaraPoseKey>& Keys, bool bTwoBoneIK, FString& OutError)
{
	if (!Sequence || !Sequence->GetDataModel())
	{
		OutError = TEXT("No animation sequence to polish.");
		return false;
	}

	const IAnimationDataModel* Model = Sequence->GetDataModel();
	const int32 NumKeys = Model->GetNumberOfKeys();
	TArray<FName> BoneNames;
	Model->GetBoneTrackNames(BoneNames);
	USkeleton* Skeleton = Sequence->GetSkeleton();
	if (!Skeleton)
	{
		OutError = TEXT("Animation has no skeleton.");
		return false;
	}
	const FReferenceSkeleton& Ref = Skeleton->GetReferenceSkeleton();
	const TArray<FTransform>& RefPose = Ref.GetRefBonePose();
	TSet<FName> TrackSet;
	for (const FName BoneName : BoneNames)
	{
		TrackSet.Add(BoneName);
	}
	TMap<FName, TArray<FVector>> PositionKeys;
	TMap<FName, TArray<FQuat>> RotationKeys;
	TMap<FName, TArray<FVector>> ScaleKeys;
	for (const FName BoneName : BoneNames)
	{
		PositionKeys.Add(BoneName).Reserve(NumKeys);
		RotationKeys.Add(BoneName).Reserve(NumKeys);
		ScaleKeys.Add(BoneName).Reserve(NumKeys);
	}

	auto ResolveBoneIndex = [&Ref](FName BoneName) -> int32
	{
		int32 Index = Ref.FindBoneIndex(BoneName);
		if (Index != INDEX_NONE)
		{
			return Index;
		}
		for (const TPair<FName, FName>& Pair : FMocaraBoneMap::MannyToSoma())
		{
			if (Pair.Value == BoneName)
			{
				Index = Ref.FindBoneIndex(Pair.Key);
				if (Index != INDEX_NONE)
				{
					return Index;
				}
			}
			if (Pair.Key == BoneName)
			{
				Index = Ref.FindBoneIndex(Pair.Value);
				if (Index != INDEX_NONE)
				{
					return Index;
				}
			}
		}
		return INDEX_NONE;
	};

	IAnimationDataController& Controller = Sequence->GetController();
	Controller.OpenBracket(FText::FromString(TEXT("Mocara AutoPose")), false);
	TArray<TMap<FName, FMocaraPoseKey>> EvaluatedPoses;
	EvaluatedPoses.SetNum(NumKeys);
	for (int32 Frame = 0; Frame < NumKeys; ++Frame)
	{
		FMocaraPoseEditing::EvaluatePoseAtFrame(Keys, Frame, EvaluatedPoses[Frame]);
	}

	for (int32 Frame = 0; Frame < NumKeys; ++Frame)
	{
		TArray<FTransform> LocalPose = RefPose;
		for (int32 BoneIndex = 0; BoneIndex < Ref.GetNum(); ++BoneIndex)
		{
			const FName BoneName = Ref.GetBoneName(BoneIndex);
			if (TrackSet.Contains(BoneName))
			{
				LocalPose[BoneIndex] = Model->EvaluateBoneTrackTransform(
					BoneName, FFrameTime(Frame), EAnimInterpolationType::Step);
			}
		}

		for (const TPair<FName, FMocaraPoseKey>& Pair : EvaluatedPoses[Frame])
		{
			const FMocaraPoseKey& Key = Pair.Value;
			const int32 BoneIndex = ResolveBoneIndex(Pair.Key);
			if (!LocalPose.IsValidIndex(BoneIndex))
			{
				continue;
			}
			LocalPose[BoneIndex].SetRotation(Key.RotationOffset.Quaternion() * LocalPose[BoneIndex].GetRotation());
			const EMocaraPoseLane Lane = FMocaraPoseEditing::ClassifyBoneLane(Ref.GetBoneName(BoneIndex));
			const bool bLimbIk = bTwoBoneIK && Key.bUseTwoBoneIK
				&& Lane != EMocaraPoseLane::RootPath;
			if (!bLimbIk)
			{
				LocalPose[BoneIndex].AddToTranslation(Key.TranslationOffset);
			}
		}

		TArray<FTransform> ComponentPose;
		ComponentPose.SetNum(Ref.GetNum());
		for (int32 BoneIndex = 0; BoneIndex < Ref.GetNum(); ++BoneIndex)
		{
			const int32 ParentIndex = Ref.GetParentIndex(BoneIndex);
			ComponentPose[BoneIndex] = ParentIndex == INDEX_NONE
				? LocalPose[BoneIndex]
				: LocalPose[BoneIndex] * ComponentPose[ParentIndex];
		}

		if (bTwoBoneIK)
		{
			for (const TPair<FName, FMocaraPoseKey>& Pair : EvaluatedPoses[Frame])
			{
				const FMocaraPoseKey& Key = Pair.Value;
				const int32 EndIndex = ResolveBoneIndex(Pair.Key);
				const EMocaraPoseLane Lane = EndIndex == INDEX_NONE
					? EMocaraPoseLane::FullBody
					: FMocaraPoseEditing::ClassifyBoneLane(Ref.GetBoneName(EndIndex));
				if (!Key.bUseTwoBoneIK || Key.TranslationOffset.IsNearlyZero()
					|| (Lane != EMocaraPoseLane::LeftHand && Lane != EMocaraPoseLane::RightHand
						&& Lane != EMocaraPoseLane::LeftFoot && Lane != EMocaraPoseLane::RightFoot))
				{
					continue;
				}
				const int32 JointIndex = EndIndex == INDEX_NONE ? INDEX_NONE : Ref.GetParentIndex(EndIndex);
				const int32 RootIndex = JointIndex == INDEX_NONE ? INDEX_NONE : Ref.GetParentIndex(JointIndex);
				if (!ComponentPose.IsValidIndex(EndIndex) || !ComponentPose.IsValidIndex(JointIndex)
					|| !ComponentPose.IsValidIndex(RootIndex))
				{
					continue;
				}
				FTransform& RootTransform = ComponentPose[RootIndex];
				FTransform& JointTransform = ComponentPose[JointIndex];
				FTransform& EndTransform = ComponentPose[EndIndex];
				SolveTwoBoneIK(RootTransform, JointTransform, EndTransform,
					EndTransform.GetLocation() + Key.TranslationOffset);
				const int32 RootParent = Ref.GetParentIndex(RootIndex);
				LocalPose[RootIndex] = RootParent == INDEX_NONE
					? RootTransform : RootTransform.GetRelativeTransform(ComponentPose[RootParent]);
				LocalPose[JointIndex] = JointTransform.GetRelativeTransform(RootTransform);
				LocalPose[EndIndex] = EndTransform.GetRelativeTransform(JointTransform);
			}
		}

		for (const FName BoneName : BoneNames)
		{
			const int32 BoneIndex = Ref.FindBoneIndex(BoneName);
			if (!LocalPose.IsValidIndex(BoneIndex))
			{
				continue;
			}
			PositionKeys.FindChecked(BoneName).Add(LocalPose[BoneIndex].GetLocation());
			RotationKeys.FindChecked(BoneName).Add(LocalPose[BoneIndex].GetRotation());
			ScaleKeys.FindChecked(BoneName).Add(LocalPose[BoneIndex].GetScale3D());
		}
	}

	for (const FName BoneName : BoneNames)
	{
		Controller.SetBoneTrackKeys(
			BoneName,
			PositionKeys.FindChecked(BoneName),
			RotationKeys.FindChecked(BoneName),
			ScaleKeys.FindChecked(BoneName),
			false);
	}

	Controller.NotifyPopulated();
	Controller.CloseBracket(false);
	Sequence->PostEditChange();
	return true;
}

bool FMocaraAutoPose::BuildConstraintsJson(UAnimSequence* SomaSequence, const TArray<FMocaraPoseKey>& Keys, TArray<TSharedPtr<FJsonValue>>& OutConstraints, FString& OutError)
{
	OutConstraints.Reset();
	if (!SomaSequence || !SomaSequence->GetDataModel() || !SomaSequence->GetSkeleton())
	{
		OutError = TEXT("Need a generated SOMA clip before sending constraints.");
		return false;
	}

	const IAnimationDataModel* Model = SomaSequence->GetDataModel();
	const FReferenceSkeleton& Ref = SomaSequence->GetSkeleton()->GetReferenceSkeleton();
	TArray<FName> RefBoneNames;
	RefBoneNames.Reserve(Ref.GetNum());
	for (int32 BoneIndex = 0; BoneIndex < Ref.GetNum(); ++BoneIndex)
	{
		RefBoneNames.Add(Ref.GetBoneName(BoneIndex));
	}
	TArray<int32> KimodoBoneIndices;
	if (!ResolveKimodoConstraintBoneIndices(RefBoneNames, KimodoBoneIndices, OutError))
	{
		return false;
	}

	const int32 NumFrames = FMath::Max(
		SomaSequence->GetNumberOfSampledKeys(), Model->GetNumberOfFrames() + 1);
	TArray<int32> ActiveFrames;
	FMocaraPoseEditing::CollectActiveFrames(Keys, NumFrames, ActiveFrames);
	if (ActiveFrames.IsEmpty())
	{
		OutError = TEXT("No keyed pose interval overlaps the generated clip.");
		return false;
	}

	// Kimodo accepts parallel full-body arrays across multiple frame indices. One object
	// keeps long holds and eases from consuming the sidecar's constraint-count limit.
	TSharedRef<FJsonObject> Constraint = MakeShared<FJsonObject>();
	Constraint->SetStringField(TEXT("type"), TEXT("fullbody"));
	TArray<TSharedPtr<FJsonValue>> Frames;
	TArray<TSharedPtr<FJsonValue>> RootPositions;
	TArray<TSharedPtr<FJsonValue>> LocalRots;

	for (const int32 Frame : ActiveFrames)
	{
		TMap<FName, FMocaraPoseKey> EvaluatedPose;
		FMocaraPoseEditing::EvaluatePoseAtFrame(Keys, Frame, EvaluatedPose);
		if (EvaluatedPose.IsEmpty())
		{
			continue;
		}
		TMap<FName, FMocaraPoseKey> SomaPose;
		for (const TPair<FName, FMocaraPoseKey>& Pair : EvaluatedPose)
		{
			SomaPose.Add(FMocaraBoneMap::ToSoma(Pair.Key), Pair.Value);
		}

		Frames.Add(MakeShared<FJsonValueNumber>(Frame));
		TArray<TSharedPtr<FJsonValue>> JointRots;

		FVector RootYUpMeters = FVector::ZeroVector;
		for (const int32 BoneIndex : KimodoBoneIndices)
		{
			const FName BoneName = Ref.GetBoneName(BoneIndex);
			FTransform Xf = Model->EvaluateBoneTrackTransform(BoneName, FFrameTime(Frame), EAnimInterpolationType::Step);
			if (const FMocaraPoseKey* Key = SomaPose.Find(BoneName))
			{
				Xf.SetRotation(Key->RotationOffset.Quaternion() * Xf.GetRotation());
				Xf.AddToTranslation(Key->TranslationOffset);
			}

			if (BoneName == TEXT("Hips") || BoneName == TEXT("Root"))
			{
				const FVector ZUp = Xf.GetLocation();
				RootYUpMeters = FMocaraBvhImporter::ZUpToYUp(ZUp) * 0.01f;
			}

			FVector Axis;
			float Angle;
			FMocaraBvhImporter::ConvertQuatZUpToYUp(Xf.GetRotation()).ToAxisAndAngle(Axis, Angle);
			const FVector AA = Axis.GetSafeNormal() * Angle;
			TArray<TSharedPtr<FJsonValue>> Triple;
			Triple.Add(MakeShared<FJsonValueNumber>(AA.X));
			Triple.Add(MakeShared<FJsonValueNumber>(AA.Y));
			Triple.Add(MakeShared<FJsonValueNumber>(AA.Z));
			JointRots.Add(MakeShared<FJsonValueArray>(Triple));
		}

		TArray<TSharedPtr<FJsonValue>> RootTriple;
		RootTriple.Add(MakeShared<FJsonValueNumber>(RootYUpMeters.X));
		RootTriple.Add(MakeShared<FJsonValueNumber>(RootYUpMeters.Y));
		RootTriple.Add(MakeShared<FJsonValueNumber>(RootYUpMeters.Z));
		RootPositions.Add(MakeShared<FJsonValueArray>(RootTriple));
		LocalRots.Add(MakeShared<FJsonValueArray>(JointRots));
	}

	if (Frames.IsEmpty())
	{
		OutError = TEXT("The keyed pose interval did not produce any active constraint frames.");
		return false;
	}
	Constraint->SetArrayField(TEXT("frame_indices"), Frames);
	Constraint->SetArrayField(TEXT("root_positions"), RootPositions);
	Constraint->SetArrayField(TEXT("local_joints_rot"), LocalRots);
	OutConstraints.Add(MakeShared<FJsonValueObject>(Constraint));
	return true;
}
