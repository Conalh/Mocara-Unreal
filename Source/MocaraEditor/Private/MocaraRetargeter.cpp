#include "MocaraRetargeter.h"
#include "MocaraBoneMap.h"
#include "MocaraSettings.h"
#include "MocaraTargetProfile.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/SkeletalMesh.h"
#include "Modules/ModuleManager.h"
#include "RetargetEditor/IKRetargetBatchOperation.h"
#include "RetargetEditor/IKRetargeterController.h"
#include "RetargetEditor/IKRetargeterPoseGenerator.h"
#include "Retargeter/IKRetargeter.h"
#include "Retargeter/RetargetOps/FloorConstraintOp.h"
#include "Retargeter/RetargetOps/RunIKRigOp.h"
#include "Retargeter/RetargetOps/SpeedPlantingOp.h"
#include "Rig/IKRigDefinition.h"
#include "Rig/Solvers/IKRigLimbSolver.h"
#include "RigEditor/IKRigController.h"
#include "Retargeter/IKRetargetChainMapping.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "MocaraBvhImporter.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogMocaraRetarget, Log, All);

namespace
{
	TArray<FName> MeshBoneNames(const USkeletalMesh* Mesh)
	{
		TArray<FName> Names;
		if (!Mesh)
		{
			return Names;
		}
		const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
		for (int32 Index = 0; Index < Ref.GetNum(); ++Index)
		{
			Names.Add(Ref.GetBoneName(Index));
		}
		return Names;
	}

	UObject* CreateOrLoad(UClass* Class, const FString& PackagePath, const FName AssetName)
	{
		const FString LongName = PackagePath / AssetName.ToString();
		if (UObject* Existing = LoadObject<UObject>(nullptr, *LongName))
		{
			return Existing;
		}
		UPackage* Package = CreatePackage(*LongName);
		UObject* Asset = NewObject<UObject>(Package, Class, AssetName, RF_Public | RF_Standalone);
		FAssetRegistryModule::AssetCreated(Asset);
		Package->MarkPackageDirty();
		return Asset;
	}

	void AddChainIfPossible(UIKRigController* Controller, const TArray<FName>& Bones, const FName Chain, const TArray<FName>& StartCandidates, const TArray<FName>& EndCandidates)
	{
		const FName Start = FMocaraBoneMap::FindExisting(Bones, StartCandidates);
		const FName End = FMocaraBoneMap::FindExisting(Bones, EndCandidates);
		if (Start != NAME_None && End != NAME_None)
		{
			Controller->AddRetargetChain(Chain, Start, End, NAME_None);
		}
	}

	/** Component-space transforms for every bone of Mesh, posed by Sequence at Frame. */
	TArray<FTransform> ComponentPoseAt(const USkeletalMesh* Mesh, const UAnimSequence* Sequence, int32 Frame)
	{
		TArray<FTransform> Component;
		if (!Mesh)
		{
			return Component;
		}
		const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
		const IAnimationDataModel* Model = Sequence ? Sequence->GetDataModel() : nullptr;
		Component.SetNum(Ref.GetNum());
		for (int32 Index = 0; Index < Ref.GetNum(); ++Index)
		{
			const FName Bone = Ref.GetBoneName(Index);
			FTransform Local = Ref.GetRefBonePose()[Index];
			if (Model && Model->IsValidBoneTrackName(Bone))
			{
				Local = Model->EvaluateBoneTrackTransform(Bone, FFrameTime(Frame), EAnimInterpolationType::Step);
			}
			const int32 Parent = Ref.GetParentIndex(Index);
			Component[Index] = (Parent == INDEX_NONE) ? Local : Local * Component[Parent];
		}
		return Component;
	}

	bool BoneDirection(const USkeletalMesh* Mesh, const TArray<FTransform>& Pose, FName From, FName To, FVector& Out)
	{
		const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
		const int32 A = Ref.FindBoneIndex(From);
		const int32 B = Ref.FindBoneIndex(To);
		if (A == INDEX_NONE || B == INDEX_NONE || !Pose.IsValidIndex(A) || !Pose.IsValidIndex(B))
		{
			return false;
		}
		const FVector D = Pose[B].GetLocation() - Pose[A].GetLocation();
		if (D.IsNearlyZero())
		{
			return false;
		}
		Out = D.GetSafeNormal();
		return true;
	}

	bool GetReferenceLocalPosition(const USkeletalMesh* Mesh, const FName Bone, FVector& OutPosition)
	{
		if (!Mesh)
		{
			return false;
		}

		const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
		const int32 BoneIndex = Ref.FindBoneIndex(Bone);
		if (BoneIndex == INDEX_NONE)
		{
			return false;
		}

		// Animation data-model tracks are local to the parent bone, so the pivot must be
		// expressed in that same space. This also keeps future profiles correct when their
		// motion root is not directly under an identity root bone.
		OutPosition = Ref.GetRefBonePose()[BoneIndex].GetLocation();
		return true;
	}

	bool ApplyTargetMotionBasis(
		UAnimSequence* Sequence,
		const USkeletalMesh* TargetMesh,
		const UAnimSequence* SourceSequence,
		const USkeletalMesh* SourceMesh,
		const FMocaraTargetProfile& TargetProfile,
		FString& OutError)
	{
		if (!Sequence || !SourceSequence || !SourceMesh || !TargetProfile.Matches(TargetMesh))
		{
			OutError = FString::Printf(TEXT("Source data is missing or the target mesh does not satisfy the '%s' Mocara profile."),
				*TargetProfile.ProfileName.ToString());
			return false;
		}

		const IAnimationDataModel* Model = Sequence->GetDataModel();
		if (!Model || !Model->IsValidBoneTrackName(TargetProfile.MotionRootBone))
		{
			OutError = FString::Printf(TEXT("Retargeted animation has no '%s' motion-root track."),
				*TargetProfile.MotionRootBone.ToString());
			return false;
		}

		FVector ReferencePosition;
		if (!GetReferenceLocalPosition(TargetMesh, TargetProfile.MotionRootBone, ReferencePosition))
		{
			OutError = FString::Printf(TEXT("Target mesh has no '%s' reference bone."),
				*TargetProfile.MotionRootBone.ToString());
			return false;
		}

		TArray<FTransform> RootTransforms;
		Model->GetBoneTrackTransforms(TargetProfile.MotionRootBone, RootTransforms);
		if (RootTransforms.IsEmpty())
		{
			OutError = FString::Printf(TEXT("Retargeted animation's '%s' track has no keys."),
				*TargetProfile.MotionRootBone.ToString());
			return false;
		}

		const FReferenceSkeleton& Ref = TargetMesh->GetRefSkeleton();
		const TArray<FTransform> ReferenceComponentPose = ComponentPoseAt(TargetMesh, nullptr, 0);
		if (ReferenceComponentPose.Num() != Ref.GetNum())
		{
			OutError = TEXT("Could not build the target reference pose for motion-basis conversion.");
			return false;
		}

		const int32 NumKeys = RootTransforms.Num();
		TArray<TArray<FTransform>> TrackTransformsByBone;
		TArray<TArray<FQuat>> CorrectedLocalRotations;
		TrackTransformsByBone.SetNum(Ref.GetNum());
		CorrectedLocalRotations.SetNum(Ref.GetNum());
		for (int32 BoneIndex = 0; BoneIndex < Ref.GetNum(); ++BoneIndex)
		{
			const FName Bone = Ref.GetBoneName(BoneIndex);
			if (!Model->IsValidBoneTrackName(Bone))
			{
				continue;
			}
			Model->GetBoneTrackTransforms(Bone, TrackTransformsByBone[BoneIndex]);
			if (TrackTransformsByBone[BoneIndex].Num() != NumKeys)
			{
				OutError = FString::Printf(TEXT("Retargeted animation's '%s' track has %d keys; expected %d."),
					*Bone.ToString(), TrackTransformsByBone[BoneIndex].Num(), NumKeys);
				return false;
			}
			CorrectedLocalRotations[BoneIndex].SetNum(NumKeys);
		}

		struct FResolvedDirectionSegment
		{
			int32 SourceBoneIndex = INDEX_NONE;
			int32 SourceChildBoneIndex = INDEX_NONE;
			int32 TargetBoneIndex = INDEX_NONE;
			int32 TargetChildBoneIndex = INDEX_NONE;
		};
		const FReferenceSkeleton& SourceRef = SourceMesh->GetRefSkeleton();
		TMap<int32, FResolvedDirectionSegment> DirectionSegmentByTargetBone;
		for (const FMocaraSourceDrivenSegment& Segment : TargetProfile.SourceDrivenSegments)
		{
			FResolvedDirectionSegment Resolved;
			Resolved.SourceBoneIndex = SourceRef.FindBoneIndex(Segment.SourceBone);
			Resolved.SourceChildBoneIndex = SourceRef.FindBoneIndex(Segment.SourceChildBone);
			Resolved.TargetBoneIndex = Ref.FindBoneIndex(Segment.TargetBone);
			Resolved.TargetChildBoneIndex = Ref.FindBoneIndex(Segment.TargetChildBone);
			if (Resolved.SourceBoneIndex == INDEX_NONE || Resolved.SourceChildBoneIndex == INDEX_NONE ||
				Resolved.TargetBoneIndex == INDEX_NONE || Resolved.TargetChildBoneIndex == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Could not resolve source-driven segment '%s -> %s' onto '%s -> %s'."),
					*Segment.SourceBone.ToString(), *Segment.SourceChildBone.ToString(),
					*Segment.TargetBone.ToString(), *Segment.TargetChildBone.ToString());
				return false;
			}
			if (Ref.GetParentIndex(Resolved.TargetChildBoneIndex) != Resolved.TargetBoneIndex)
			{
				OutError = FString::Printf(TEXT("Target source-driven segment '%s -> %s' is not a direct bone segment."),
					*Segment.TargetBone.ToString(), *Segment.TargetChildBone.ToString());
				return false;
			}
			if (DirectionSegmentByTargetBone.Contains(Resolved.TargetBoneIndex))
			{
				OutError = FString::Printf(TEXT("Target bone '%s' has more than one source-driven segment."),
					*Segment.TargetBone.ToString());
				return false;
			}
			DirectionSegmentByTargetBone.Add(Resolved.TargetBoneIndex, Resolved);
		}

		TBitArray<> bInLocalRotationSubtree(false, Ref.GetNum());
		int32 NumLocalRotationInheritedBones = 0;
		for (const FName SubtreeRoot : TargetProfile.LocalRotationSubtreeRoots)
		{
			const int32 RootBoneIndex = Ref.FindBoneIndex(SubtreeRoot);
			if (RootBoneIndex == INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Could not resolve local-rotation subtree root '%s'."),
					*SubtreeRoot.ToString());
				return false;
			}

			for (int32 BoneIndex = 0; BoneIndex < Ref.GetNum(); ++BoneIndex)
			{
				for (int32 AncestorIndex = BoneIndex; AncestorIndex != INDEX_NONE;
					AncestorIndex = Ref.GetParentIndex(AncestorIndex))
				{
					if (AncestorIndex != RootBoneIndex)
					{
						continue;
					}
					if (!bInLocalRotationSubtree[BoneIndex])
					{
						bInLocalRotationSubtree[BoneIndex] = true;
						++NumLocalRotationInheritedBones;
					}
					break;
				}
			}
		}

		for (int32 Frame = 0; Frame < NumKeys; ++Frame)
		{
			const TArray<FTransform> CurrentComponentPose = ComponentPoseAt(TargetMesh, Sequence, Frame);
			const TArray<FTransform> SourceComponentPose = ComponentPoseAt(SourceMesh, SourceSequence, Frame);
			if (CurrentComponentPose.Num() != Ref.GetNum() || SourceComponentPose.Num() != SourceRef.GetNum())
			{
				OutError = FString::Printf(TEXT("Could not evaluate the source and target poses at frame %d."), Frame);
				return false;
			}

			TArray<FTransform> CurrentLocalPose;
			TArray<FTransform> CorrectedComponentPose;
			CurrentLocalPose.SetNum(Ref.GetNum());
			CorrectedComponentPose.SetNum(Ref.GetNum());
			for (int32 BoneIndex = 0; BoneIndex < Ref.GetNum(); ++BoneIndex)
			{
				const int32 ParentIndex = Ref.GetParentIndex(BoneIndex);
				CurrentLocalPose[BoneIndex] = ParentIndex == INDEX_NONE
					? CurrentComponentPose[BoneIndex]
					: CurrentComponentPose[BoneIndex].GetRelativeTransform(CurrentComponentPose[ParentIndex]);
			}

			for (int32 BoneIndex = 0; BoneIndex < Ref.GetNum(); ++BoneIndex)
			{
				const int32 ParentIndex = Ref.GetParentIndex(BoneIndex);
				FTransform CorrectedGlobal = ParentIndex == INDEX_NONE
					? CurrentLocalPose[BoneIndex]
					: CurrentLocalPose[BoneIndex] * CorrectedComponentPose[ParentIndex];
				const bool bShouldInheritLocalRotation = bInLocalRotationSubtree[BoneIndex];
				if (!bShouldInheritLocalRotation)
				{
					const FQuat ReferenceRotation = ReferenceComponentPose[BoneIndex].GetRotation();
					const FQuat CurrentRotation = CurrentComponentPose[BoneIndex].GetRotation();
					const FQuat AnimationDelta = CurrentRotation * ReferenceRotation.Inverse();
					CorrectedGlobal.SetRotation(
						TargetProfile.ReorientAnimationDelta(AnimationDelta) * ReferenceRotation);
				}

				if (const FResolvedDirectionSegment* Segment = DirectionSegmentByTargetBone.Find(BoneIndex))
				{
					const FVector SourceDirection =
						SourceComponentPose[Segment->SourceChildBoneIndex].GetLocation() -
						SourceComponentPose[Segment->SourceBoneIndex].GetLocation();
					const FVector DesiredDirection =
						TargetProfile.SourceMotionToTargetBasis.RotateVector(SourceDirection).GetSafeNormal();
					const FVector CurrentDirection = CorrectedGlobal.TransformVectorNoScale(
						CurrentLocalPose[Segment->TargetChildBoneIndex].GetLocation()).GetSafeNormal();
					if (DesiredDirection.IsNearlyZero() || CurrentDirection.IsNearlyZero())
					{
						OutError = FString::Printf(TEXT("Degenerate source-driven arm segment at frame %d."), Frame);
						return false;
					}
					const FQuat DirectionCorrection = FQuat::FindBetweenNormals(CurrentDirection, DesiredDirection);
					CorrectedGlobal.SetRotation(
						(DirectionCorrection * CorrectedGlobal.GetRotation()).GetNormalized());
				}

				const FTransform CorrectedLocal = ParentIndex == INDEX_NONE
					? CorrectedGlobal
					: CorrectedGlobal.GetRelativeTransform(CorrectedComponentPose[ParentIndex]);
				CorrectedComponentPose[BoneIndex] = CorrectedGlobal;
				if (!CorrectedLocalRotations[BoneIndex].IsEmpty())
				{
					CorrectedLocalRotations[BoneIndex][Frame] = CorrectedLocal.GetRotation();
				}
			}
		}

		IAnimationDataController& Controller = Sequence->GetController();
		Controller.OpenBracket(FText::FromString(TEXT("Apply Mocara target motion basis")), false);
		bool bSetAllTracks = true;
		int32 NumCorrectedTracks = 0;
		for (int32 BoneIndex = 0; BoneIndex < Ref.GetNum(); ++BoneIndex)
		{
			if (TrackTransformsByBone[BoneIndex].IsEmpty())
			{
				continue;
			}
			const FName Bone = Ref.GetBoneName(BoneIndex);
			TArray<FVector> Positions;
			TArray<FVector> Scales;
			Positions.Reserve(NumKeys);
			Scales.Reserve(NumKeys);
			for (const FTransform& Transform : TrackTransformsByBone[BoneIndex])
			{
				Positions.Add(Transform.GetLocation());
				Scales.Add(Transform.GetScale3D());
			}
			if (Bone == TargetProfile.MotionRootBone)
			{
				for (FVector& Position : Positions)
				{
					Position = TargetProfile.ReorientRootTranslation(ReferencePosition, Position);
				}
			}

			bSetAllTracks &= Controller.SetBoneTrackKeys(
				Bone, Positions, CorrectedLocalRotations[BoneIndex], Scales, false);
			++NumCorrectedTracks;
		}
		Controller.CloseBracket(false);
		if (!bSetAllTracks)
		{
			OutError = TEXT("Could not update every retargeted bone track during motion-basis conversion.");
			return false;
		}

		Sequence->PostEditChange();
		UE_LOG(LogMocaraRetarget, Display,
			TEXT("Applied target profile '%s' to %d animation tracks, including %d source-driven arm segments and %d local-rotation-inherited bones (%d '%s' translation keys)."),
			*TargetProfile.ProfileName.ToString(), NumCorrectedTracks, TargetProfile.SourceDrivenSegments.Num(),
			NumLocalRotationInheritedBones, NumKeys, *TargetProfile.MotionRootBone.ToString());
		return true;
	}

	/** Name of the first op of the given type in the stack, or None. */
	FName FindOpName(const UIKRetargeterController* Controller, const UScriptStruct* OpType)
	{
		for (int32 Index = 0; Index < Controller->GetNumRetargetOps(); ++Index)
		{
			if (const FIKRetargetOpBase* Op = Controller->GetRetargetOpByIndex(Index))
			{
				if (Op->GetType() == OpType)
				{
					return Controller->GetOpName(Index);
				}
			}
		}
		return NAME_None;
	}

	/** Find an op of the given type, or add one under ParentOpName. Idempotent across regenerations. */
	FIKRetargetOpBase* FindOrAddOp(UIKRetargeterController* Controller, const UScriptStruct* OpType, const FName ParentOpName)
	{
		for (int32 Index = 0; Index < Controller->GetNumRetargetOps(); ++Index)
		{
			if (FIKRetargetOpBase* Existing = Controller->GetRetargetOpByIndex(Index))
			{
				if (Existing->GetType() == OpType)
				{
					return Existing;
				}
			}
		}
		const int32 NewIndex = Controller->AddRetargetOp(OpType, ParentOpName);
		return (NewIndex != INDEX_NONE) ? Controller->GetRetargetOpByIndex(NewIndex) : nullptr;
	}

	/**
	 * Add the two ops that fix the artifacts measured on retargeted clips: the toe dipping
	 * ~2 cm below the floor, and the planted foot sliding ~1.35 cm/frame against the source's
	 * 0.36. AddDefaultOps() ships neither, and both must be children of the Run IK Rig op.
	 * Speed Planting reads the foot-speed curves the BVH importer writes onto the source clip.
	 */
	void AddFootPlantingOps(UIKRetargeterController* Controller)
	{
		const FName RunIkOpName = FindOpName(Controller, FIKRetargetRunIKRigOp::StaticStruct());
		if (RunIkOpName == NAME_None)
		{
			return;
		}
		FindOrAddOp(Controller, FIKRetargetFloorConstraintOp::StaticStruct(), RunIkOpName);
		FindOrAddOp(Controller, FIKRetargetSpeedPlantingOp::StaticStruct(), RunIkOpName);
	}

	void ConfigureFootPlanting(UIKRetargeterController* Controller, float FootPlantingStrength)
	{
		const FName RunIkOpName = FindOpName(Controller, FIKRetargetRunIKRigOp::StaticStruct());
		if (RunIkOpName == NAME_None)
		{
			return;
		}

		if (FIKRetargetOpBase* FloorOp = FindOrAddOp(Controller, FIKRetargetFloorConstraintOp::StaticStruct(), RunIkOpName))
		{
			if (FIKRetargetFloorConstraintOpSettings* Settings =
				static_cast<FIKRetargetFloorConstraintOpSettings*>(FloorOp->GetSettings()))
			{
				Settings->ChainsToAffect.Reset();
				for (const FName Chain : {FName(TEXT("LeftLeg")), FName(TEXT("RightLeg"))})
				{
					FFloorConstraintChainSettings ChainSettings;
					ChainSettings.TargetChainName = Chain;
					// Per-chain enable defaults to FALSE -- adding the chain is not enough.
					ChainSettings.EnableFloorConstraint = true;
					// Treat the foot as a heel/toe span rather than a point, otherwise the
					// toe still dips below the floor while the ankle sits on it.
					ChainSettings.bUseFoot = true;
					ChainSettings.Alpha = FootPlantingStrength;
					Settings->ChainsToAffect.Add(ChainSettings);
				}
			}
		}

		if (FIKRetargetOpBase* SpeedOp = FindOrAddOp(Controller, FIKRetargetSpeedPlantingOp::StaticStruct(), RunIkOpName))
		{
			if (FIKRetargetSpeedPlantingOpSettings* Settings =
				static_cast<FIKRetargetSpeedPlantingOpSettings*>(SpeedOp->GetSettings()))
			{
				Settings->ChainsToSpeedPlant.Reset();
				const TPair<FName, FName> ChainToCurve[] = {
					{FName(TEXT("LeftLeg")),  FName(TEXT("LeftFoot_speed"))},
					{FName(TEXT("RightLeg")), FName(TEXT("RightFoot_speed"))},
				};
				for (const TPair<FName, FName>& Pair : ChainToCurve)
				{
					FRetargetSpeedPlantingSettings ChainSettings;
					ChainSettings.TargetChainName = Pair.Key;
					ChainSettings.SpeedCurveName = Pair.Value;
					Settings->ChainsToSpeedPlant.Add(ChainSettings);
				}
			}
		}
	}

	/**
	 * Auto-characterization produces retarget chains but no IK at all -- both rigs come out
	 * with 0 solvers and 0 goals. That leaves the Run IK Rig op with nothing to solve, and
	 * the Floor Constraint / Speed Planting ops with no goals to constrain, so foot planting
	 * silently does nothing. Give the target's legs a 2-bone limb solver with a foot goal.
	 */
	void ConfigureLegIk(UIKRigController* Controller, const TArray<FName>& Bones)
	{
		struct FLegSetup
		{
			const TCHAR* Chain;
			const TCHAR* Thigh;
			const TCHAR* Foot;
			const TCHAR* Goal;
		};
		static const FLegSetup Legs[] = {
			{TEXT("LeftLeg"),  TEXT("thigh_l"), TEXT("foot_l"), TEXT("LeftFootGoal")},
			{TEXT("RightLeg"), TEXT("thigh_r"), TEXT("foot_r"), TEXT("RightFootGoal")},
		};

		for (const FLegSetup& Leg : Legs)
		{
			if (!Bones.Contains(FName(Leg.Thigh)) || !Bones.Contains(FName(Leg.Foot)))
			{
				continue;
			}
			// Create the goal and solver only once, but ALWAYS re-link the chain below:
			// ApplyAutoGeneratedRetargetDefinition() runs on every EnsureIkRig call and
			// rebuilds the retarget chains, which drops the chain->goal association. Skipping
			// the relink when the goal object happened to survive left the Floor Constraint op
			// unable to resolve a goal, and it ensured (GoalPtr) on every generate after the
			// first -- i.e. as soon as the rigs were being loaded from disk rather than built.
			FName GoalName = FName(Leg.Goal);
			if (Controller->GetGoal(GoalName) == nullptr)
			{
				GoalName = Controller->AddNewGoal(FName(Leg.Goal), FName(Leg.Foot));
				if (GoalName == NAME_None)
				{
					continue;
				}
				const int32 SolverIndex = Controller->AddSolver(FIKRigLimbSolver::StaticStruct());
				if (SolverIndex == INDEX_NONE)
				{
					continue;
				}
				Controller->SetStartBone(FName(Leg.Thigh), SolverIndex);
				Controller->ConnectGoalToSolver(GoalName, SolverIndex);
			}
			Controller->SetRetargetChainGoal(FName(Leg.Chain), GoalName);
		}
	}

	bool HasChain(const UIKRigController* Controller, const FName Chain)
	{
		for (const FBoneChain& Existing : Controller->GetRetargetChains())
		{
			if (Existing.ChainName == Chain)
			{
				return true;
			}
		}
		return false;
	}

	/**
	 * Make sure a chain exists and spans the intended bones. If auto-characterization already
	 * produced the chain we only widen its endpoints; otherwise we add it outright.
	 */
	void EnsureChainSpans(UIKRigController* Controller, const TArray<FName>& Bones, const FName Chain,
		const TArray<FName>& StartCandidates, const TArray<FName>& EndCandidates)
	{
		const FName Start = FMocaraBoneMap::FindExisting(Bones, StartCandidates);
		const FName End = FMocaraBoneMap::FindExisting(Bones, EndCandidates);
		if (Start == NAME_None || End == NAME_None)
		{
			return;
		}
		if (HasChain(Controller, Chain))
		{
			Controller->SetRetargetChainStartBone(Chain, Start);
			Controller->SetRetargetChainEndBone(Chain, End);
		}
		else
		{
			Controller->AddRetargetChain(Chain, Start, End, NAME_None);
		}
	}
}

USkeletalMesh* FMocaraRetargeter::ResolveTargetMesh(
	const FSoftObjectPath& TargetPath,
	FMocaraTargetProfile& OutProfile,
	FString& OutError)
{
	OutError.Empty();
	if (!TargetPath.IsValid())
	{
		OutError = TEXT("No Mocara target mesh was selected.");
		return nullptr;
	}

	USkeletalMesh* Mesh = Cast<USkeletalMesh>(TargetPath.TryLoad());
	if (!Mesh)
	{
		OutError = FString::Printf(TEXT("Could not load Mocara target mesh '%s'."), *TargetPath.ToString());
		return nullptr;
	}

	const TOptional<FMocaraTargetProfile> Profile = FMocaraTargetProfile::ForMesh(Mesh);
	if (!Profile.IsSet())
	{
		OutError = FString::Printf(
			TEXT("Target mesh '%s' is not supported by a built-in Mocara target profile."),
			*Mesh->GetPathName());
		return nullptr;
	}
	OutProfile = Profile.GetValue();
	return Mesh;
}

FName FMocaraRetargeter::MakeTargetAssetId(const USkeletalMesh* Mesh, const FMocaraTargetProfile& Profile)
{
	if (!Mesh || Profile.ProfileName.IsNone())
	{
		return NAME_None;
	}
	const uint32 PathHash = FCrc::StrCrc32(*Mesh->GetPathName());
	return FName(*FString::Printf(
		TEXT("%s_%s_%08X"),
		*Profile.ProfileName.ToString(),
		*Mesh->GetName(),
		PathHash));
}

USkeletalMesh* FMocaraRetargeter::FindTargetMesh(FMocaraTargetProfile& OutProfile, FString& OutError)
{
	OutError.Empty();
	TArray<FSoftObjectPath> Paths;
	if (const UMocaraSettings* Settings = GetDefault<UMocaraSettings>())
	{
		if (Settings->TargetMesh.IsValid())
		{
			return ResolveTargetMesh(Settings->TargetMesh, OutProfile, OutError);
		}
		Paths = Settings->ExtraMannySearchPaths;
		if (Settings->MannyMesh.IsValid())
		{
			Paths.Insert(Settings->MannyMesh, 0);
		}
	}
	Paths.Add(FSoftObjectPath(TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny.SKM_Manny")));
	Paths.Add(FSoftObjectPath(TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple.SKM_Manny_Simple")));
	Paths.Add(FSoftObjectPath(TEXT("/Game/ThirdPerson/Meshes/SKM_Manny.SKM_Manny")));

	for (const FSoftObjectPath& Path : Paths)
	{
		FMocaraTargetProfile CandidateProfile;
		FString CandidateError;
		if (USkeletalMesh* Mesh = ResolveTargetMesh(Path, CandidateProfile, CandidateError))
		{
			OutProfile = CandidateProfile;
			return Mesh;
		}
	}

	TArray<FAssetData> Assets;
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.GetAssetsByClass(USkeletalMesh::StaticClass()->GetClassPathName(), Assets);

	for (const FAssetData& Asset : Assets)
	{
		const FString Name = Asset.AssetName.ToString();
		if (Name.Contains(TEXT("Manny")) || Name.Contains(TEXT("Mannequin")))
		{
			if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset.GetAsset()))
			{
				if (const TOptional<FMocaraTargetProfile> Profile = FMocaraTargetProfile::ForMesh(Mesh))
				{
					OutProfile = Profile.GetValue();
					return Mesh;
				}
			}
		}
	}

	// Nothing matched by name. Fall back to a structural match so compatible targets are
	// still discovered without coupling support to a particular asset naming convention.
	for (const FAssetData& Asset : Assets)
	{
		USkeletalMesh* Mesh = Cast<USkeletalMesh>(Asset.GetAsset());
		if (const TOptional<FMocaraTargetProfile> Profile = FMocaraTargetProfile::ForMesh(Mesh))
		{
			UE_LOG(LogMocaraRetarget, Display,
				TEXT("Using structurally compatible retarget target '%s' with profile '%s'. ")
				TEXT("Set Project Settings > Plugins > Mocara > Target Mesh to choose explicitly."),
				*Mesh->GetPathName(), *Profile->ProfileName.ToString());
			OutProfile = Profile.GetValue();
			return Mesh;
		}
	}
	OutError = TEXT("No supported target Skeletal Mesh was found. Select one in Project Settings > Plugins > Mocara > Target Mesh.");
	return nullptr;
}

USkeletalMesh* FMocaraRetargeter::FindMannyMesh()
{
	FMocaraTargetProfile Profile;
	FString Error;
	return FindTargetMesh(Profile, Error);
}

UIKRigDefinition* FMocaraRetargeter::EnsureIkRig(USkeletalMesh* Mesh, const FString& PackagePath, const FName AssetName, bool bSoma)
{
	if (!Mesh)
	{
		return nullptr;
	}

	UIKRigDefinition* Rig = Cast<UIKRigDefinition>(CreateOrLoad(UIKRigDefinition::StaticClass(), PackagePath, AssetName));
	UIKRigController* Controller = UIKRigController::GetController(Rig);
	Controller->SetSkeletalMesh(Mesh);
	const bool bAuto = Controller->ApplyAutoGeneratedRetargetDefinition();
	const TArray<FName> Bones = MeshBoneNames(Mesh);

	if (!bAuto || Controller->GetRetargetChains().Num() == 0)
	{
		// Fallback for skeletons UE cannot characterize. Kept deliberately close to what
		// auto-characterization produces for these two rigs so the chain names line up.
		if (bSoma)
		{
			// Kimodo's own reader excludes "Root" and treats Hips as the motion root, so
			// prefer Hips here -- "Root" is a static bone at the origin and retargeting
			// from it would drop all pelvis translation.
			const FName Root = FMocaraBoneMap::FindExisting(Bones, {TEXT("Hips"), TEXT("Root")});
			if (Root != NAME_None)
			{
				Controller->SetRetargetRoot(Root);
			}
			AddChainIfPossible(Controller, Bones, TEXT("Spine"), {TEXT("Spine1")}, {TEXT("Chest"), TEXT("Spine2")});
			AddChainIfPossible(Controller, Bones, TEXT("Neck"), {TEXT("Neck1"), TEXT("Neck")}, {TEXT("Neck2"), TEXT("Neck1")});
			AddChainIfPossible(Controller, Bones, TEXT("Head"), {TEXT("Head")}, {TEXT("Head")});
			AddChainIfPossible(Controller, Bones, TEXT("LeftClavicle"), {TEXT("LeftShoulder")}, {TEXT("LeftShoulder")});
			AddChainIfPossible(Controller, Bones, TEXT("RightClavicle"), {TEXT("RightShoulder")}, {TEXT("RightShoulder")});
			AddChainIfPossible(Controller, Bones, TEXT("LeftArm"), {TEXT("LeftArm")}, {TEXT("LeftHand")});
			AddChainIfPossible(Controller, Bones, TEXT("RightArm"), {TEXT("RightArm")}, {TEXT("RightHand")});
			AddChainIfPossible(Controller, Bones, TEXT("LeftLeg"), {TEXT("LeftLeg"), TEXT("LeftUpLeg")}, {TEXT("LeftFoot")});
			AddChainIfPossible(Controller, Bones, TEXT("RightLeg"), {TEXT("RightLeg"), TEXT("RightUpLeg")}, {TEXT("RightFoot")});
			AddChainIfPossible(Controller, Bones, TEXT("LeftFoot"), {TEXT("LeftToeBase")}, {TEXT("LeftToeBase")});
			AddChainIfPossible(Controller, Bones, TEXT("RightFoot"), {TEXT("RightToeBase")}, {TEXT("RightToeBase")});
		}
		else
		{
			const FName Root = FMocaraBoneMap::FindExisting(Bones, {TEXT("pelvis"), TEXT("root")});
			if (Root != NAME_None)
			{
				Controller->SetRetargetRoot(Root);
			}
			AddChainIfPossible(Controller, Bones, TEXT("Spine"), {TEXT("spine_01")}, {TEXT("spine_05"), TEXT("spine_03")});
			AddChainIfPossible(Controller, Bones, TEXT("Neck"), {TEXT("neck_01")}, {TEXT("neck_02"), TEXT("neck_01")});
			AddChainIfPossible(Controller, Bones, TEXT("Head"), {TEXT("head")}, {TEXT("head")});
			AddChainIfPossible(Controller, Bones, TEXT("LeftClavicle"), {TEXT("clavicle_l")}, {TEXT("clavicle_l")});
			AddChainIfPossible(Controller, Bones, TEXT("RightClavicle"), {TEXT("clavicle_r")}, {TEXT("clavicle_r")});
			AddChainIfPossible(Controller, Bones, TEXT("LeftArm"), {TEXT("upperarm_l")}, {TEXT("hand_l")});
			AddChainIfPossible(Controller, Bones, TEXT("RightArm"), {TEXT("upperarm_r")}, {TEXT("hand_r")});
			AddChainIfPossible(Controller, Bones, TEXT("LeftLeg"), {TEXT("thigh_l")}, {TEXT("foot_l")});
			AddChainIfPossible(Controller, Bones, TEXT("RightLeg"), {TEXT("thigh_r")}, {TEXT("foot_r")});
			AddChainIfPossible(Controller, Bones, TEXT("LeftFoot"), {TEXT("ball_l")}, {TEXT("ball_l")});
			AddChainIfPossible(Controller, Bones, TEXT("RightFoot"), {TEXT("ball_r")}, {TEXT("ball_r")});
		}
	}

	if (!bSoma)
	{
		ConfigureLegIk(Controller, Bones);
	}

	if (bSoma)
	{
		// UE's auto-characterization of the SOMA skeleton stops the spine at Spine2 and
		// produces no neck chain at all. UE5 target chains are Spine spine_01..spine_05
		// and Neck neck_01..neck_02, so without these two repairs the Chest rotation is
		// dropped and the target "Neck" chain has no source to map to.
		EnsureChainSpans(Controller, Bones, TEXT("Spine"), {TEXT("Spine1")}, {TEXT("Chest"), TEXT("Spine2")});
		EnsureChainSpans(Controller, Bones, TEXT("Neck"), {TEXT("Neck1")}, {TEXT("Neck2"), TEXT("Neck1")});
	}

	return Rig;
}

/**
 * True for bones whose chain runs along the body's vertical axis (pelvis, spine, neck,
 * head, root). Aligning these chain-to-chain is degenerate -- their direction is the same
 * "up" in both skeletons, so the yaw about that axis carries no information and whatever
 * the solver picks is arbitrary. Everything here is left at the retarget pose's identity.
 */
static bool IsVerticallyDegenerateBone(const FName BoneName)
{
	const FString Name = BoneName.ToString().ToLower();
	static const TCHAR* Prefixes[] = { TEXT("root"), TEXT("pelvis"), TEXT("spine"), TEXT("neck"), TEXT("head") };
	for (const TCHAR* Prefix : Prefixes)
	{
		if (Name.StartsWith(Prefix))
		{
			return true;
		}
	}
	// SOMA spellings, in case this ever runs with the roles reversed.
	return Name == TEXT("hips") || Name == TEXT("chest");
}

/** Chain-to-chain align every bone except the vertically-degenerate ones. */
static void AutoAlignLimbsOnly(UIKRetargeterController* Controller, const UIKRigDefinition* TargetRig)
{
	const UIKRigController* RigController = TargetRig ? UIKRigController::GetController(const_cast<UIKRigDefinition*>(TargetRig)) : nullptr;
	const USkeletalMesh* TargetMesh = RigController ? RigController->GetSkeletalMesh() : nullptr;

	TArray<FName> BonesToAlign;
	for (const FName& Bone : MeshBoneNames(TargetMesh))
	{
		if (!IsVerticallyDegenerateBone(Bone))
		{
			BonesToAlign.Add(Bone);
		}
	}

	if (BonesToAlign.IsEmpty())
	{
		// Better a known-good torso than an unaligned everything.
		UE_LOG(LogMocaraRetarget, Warning, TEXT("No target bones resolved for alignment; skipping auto-align."));
		return;
	}

	// Clear the pose first. The retargeter asset persists between runs, so a torso yaw
	// written by a previous AutoAlignAllBones would survive -- we would simply stop
	// re-applying it while the stale value stayed baked in, and the fix would appear to
	// do nothing on any machine that had already generated a clip. Same shape of trap as
	// the leg IK goals that had to be re-linked on every EnsureIkRig.
	Controller->ResetRetargetPose(
		Controller->GetCurrentRetargetPoseName(ERetargetSourceOrTarget::Target),
		TArray<FName>(),                       // empty = every bone
		ERetargetSourceOrTarget::Target);

	Controller->AutoAlignBones(BonesToAlign, ERetargetAutoAlignMethod::ChainToChain, ERetargetSourceOrTarget::Target);
	UE_LOG(LogMocaraRetarget, Display, TEXT("Auto-aligned %d limb bones (torso left at identity)."), BonesToAlign.Num());
}

UIKRetargeter* FMocaraRetargeter::EnsureRetargeter(UIKRigDefinition* SourceRig, UIKRigDefinition* TargetRig, const FString& PackagePath, const FName AssetName)
{
	if (!SourceRig || !TargetRig)
	{
		return nullptr;
	}
	UIKRetargeter* Retargeter = Cast<UIKRetargeter>(CreateOrLoad(UIKRetargeter::StaticClass(), PackagePath, AssetName));
	UIKRetargeterController* Controller = UIKRetargeterController::GetController(Retargeter);
	Controller->SetIKRig(ERetargetSourceOrTarget::Source, SourceRig);
	Controller->SetIKRig(ERetargetSourceOrTarget::Target, TargetRig);
	if (Controller->GetNumRetargetOps() == 0)
	{
		Controller->AddDefaultOps();
	}
	// Add the foot-planting ops BEFORE assigning rigs and mapping chains: those two calls
	// initialise every op in the stack, so an op added afterwards is left with no IK rig
	// and no chain mapping, and silently does nothing.
	AddFootPlantingOps(Controller);

	Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Source, SourceRig);
	Controller->AssignIKRigToAllOps(ERetargetSourceOrTarget::Target, TargetRig);
	Controller->AutoMapChains(EAutoMapChainType::Exact, true);

	// Align the target retarget pose to the source's chain directions. SOMA is authored as
	// a T-pose while UE5 target reference poses are not; without this mismatch is baked into
	// every retargeted clip and shows up as tens of degrees of error in the arms
	// (measured: shoulder ~52-71 deg, elbow ~30 deg before alignment).
	//
	// Limbs ONLY. AutoAlignAllBones would also align the pelvis and spine, and those
	// chains point essentially straight up in both skeletons -- so "make the chain
	// directions match" is satisfied by ANY yaw about the vertical, leaving the rotation
	// about the chain unconstrained. It resolved to a 90 deg yaw: the whole torso faced
	// +Y while the feet kept swinging along +X, which reads as a side shuffle on a
	// forward run. Measured on the retargeted clip: pelvis forward (+0.008, +1.000) vs
	// foot swing (+0.999, +0.039) = 87.3 deg apart, where the source is 5.2 deg.
	//
	// Every existing metric was blind to it -- intrinsic joint angles do not change when
	// the whole body yaws, and the T-pose has no motion to disagree with.
	AutoAlignLimbsOnly(Controller, TargetRig);

	// Per-chain settings last, so nothing above resets them.
	float FootPlantingStrength = 1.0f;
	if (const UMocaraSettings* Settings = GetDefault<UMocaraSettings>())
	{
		FootPlantingStrength = FMath::Clamp(Settings->FootPlantingStrength, 0.f, 1.f);
	}
	ConfigureFootPlanting(Controller, FootPlantingStrength);

	return Retargeter;
}

UAnimSequence* FMocaraRetargeter::Retarget(UAnimSequence* SourceSeq, USkeletalMesh* SourceMesh, USkeletalMesh* TargetMesh, const FMocaraTargetProfile& TargetProfile, UIKRetargeter* Retargeter, const FString& DestinationPath, const FString& NewName, FString& OutError)
{
	if (!SourceSeq || !SourceMesh || !TargetMesh || !Retargeter)
	{
		OutError = TEXT("Retarget is missing a source clip, source mesh, target mesh, or IK retargeter.");
		return nullptr;
	}

	FIKRetargetBatchOperationInputs Inputs;
	Inputs.AssetsToRetarget.Add(FAssetData(SourceSeq));
	Inputs.SourceMesh = SourceMesh;
	Inputs.TargetMesh = TargetMesh;
	Inputs.IKRetargetAsset = Retargeter;
	Inputs.Prefix.Empty();
	Inputs.Suffix.Empty();
	Inputs.Search = SourceSeq->GetName();
	Inputs.Replace = NewName;
	Inputs.TargetPath = DestinationPath;
	Inputs.bOverwriteExistingFiles = true;
	Inputs.bIncludeReferencedAssets = false;

	const TArray<FAssetData> Results = UIKRetargetBatchOperation::RunBatchRetarget(Inputs);
	if (!Results.Num())
	{
		OutError = TEXT("IK retarget produced no animation. Check the source-to-target chain mapping.");
		return nullptr;
	}
	UAnimSequence* Result = Cast<UAnimSequence>(Results[0].GetAsset());
	if (!ApplyTargetMotionBasis(Result, TargetMesh, SourceSeq, SourceMesh, TargetProfile, OutError))
	{
		return nullptr;
	}
	return Result;
}

/**
 * Dump the retarget setup that Mocara builds for a given BVH, so the chain definitions
 * and source->target mapping can be inspected without opening the IK Retargeter editor.
 *   Mocara.VerifyRetarget <bvh-path>
 */
static void MocaraVerifyRetargetCommand(const TArray<FString>& Args)
{
	if (Args.Num() < 1)
	{
		UE_LOG(LogMocaraRetarget, Error, TEXT("Usage: Mocara.VerifyRetarget <bvh-path>"));
		return;
	}

	FMocaraImportedClip Clip;
	FString Error;
	if (!FMocaraBvhImporter::ImportFile(Args[0], TEXT("/Game/Mocara/Verify"), TEXT("AS_RtVerify"), false, Clip, Error, /*bSaveAssets=*/false))
	{
		UE_LOG(LogMocaraRetarget, Error, TEXT("Import failed: %s"), *Error);
		return;
	}

	// Optional second arg overrides the retarget target, so any character can be verified:
	//   Mocara.VerifyRetarget <bvh> /Game/MetaHumans/Kellan/.../m_med_nrw_body
	FMocaraTargetProfile TargetProfile;
	USkeletalMesh* Manny = nullptr;
	if (Args.IsValidIndex(1))
	{
		Manny = FMocaraRetargeter::ResolveTargetMesh(FSoftObjectPath(Args[1]), TargetProfile, Error);
		if (!Manny)
		{
			UE_LOG(LogMocaraRetarget, Error, TEXT("%s"), *Error);
			return;
		}
	}
	else
	{
		Manny = FMocaraRetargeter::FindTargetMesh(TargetProfile, Error);
	}
	UE_LOG(LogMocaraRetarget, Display, TEXT("target mesh: %s (%d bones)"),
		*GetNameSafe(Manny), Manny ? Manny->GetRefSkeleton().GetNum() : 0);
	if (!Manny)
	{
		return;
	}

	const FName TargetAssetId = FMocaraRetargeter::MakeTargetAssetId(Manny, TargetProfile);
	UIKRigDefinition* SomaRig = FMocaraRetargeter::EnsureIkRig(Clip.Mesh, TEXT("/Game/Mocara/Verify"), TEXT("IK_SOMA_V"), true);
	UIKRigDefinition* MannyRig = FMocaraRetargeter::EnsureIkRig(
		Manny, TEXT("/Game/Mocara/Verify"),
		FName(*FString::Printf(TEXT("IK_%s_V"), *TargetAssetId.ToString())), false);

	for (const TPair<const TCHAR*, UIKRigDefinition*> Pair : { TPair<const TCHAR*, UIKRigDefinition*>(TEXT("SOMA"), SomaRig),
	                                                           TPair<const TCHAR*, UIKRigDefinition*>(TEXT("Manny"), MannyRig) })
	{
		if (!Pair.Value)
		{
			continue;
		}
		const UIKRigController* Ctl = UIKRigController::GetController(Pair.Value);
		UE_LOG(LogMocaraRetarget, Display, TEXT("[%s] retarget root = '%s', %d chains, %d solvers, %d IK goals:"),
			Pair.Key, *Ctl->GetRetargetRoot().ToString(), Ctl->GetRetargetChains().Num(),
			Ctl->GetNumSolvers(), Ctl->GetAllGoals().Num());
		for (const UIKRigEffectorGoal* Goal : Ctl->GetAllGoals())
		{
			if (Goal)
			{
				UE_LOG(LogMocaraRetarget, Display, TEXT("    GOAL %s -> bone %s"),
					*Goal->GoalName.ToString(), *Goal->BoneName.ToString());
			}
		}
		for (const FBoneChain& Chain : Ctl->GetRetargetChains())
		{
			UE_LOG(LogMocaraRetarget, Display, TEXT("    %-14s %s -> %s"),
				*Chain.ChainName.ToString(), *Chain.StartBone.BoneName.ToString(), *Chain.EndBone.BoneName.ToString());
		}
	}

	UIKRetargeter* Fwd = FMocaraRetargeter::EnsureRetargeter(
		SomaRig, MannyRig, TEXT("/Game/Mocara/Verify"),
		FName(*FString::Printf(TEXT("RTG_%s_V"), *TargetAssetId.ToString())));
	if (!Fwd)
	{
		return;
	}
	const UIKRetargeterController* RtCtl = UIKRetargeterController::GetController(Fwd);
	const int32 NumOps = RtCtl->GetNumRetargetOps();
	UE_LOG(LogMocaraRetarget, Display, TEXT("retargeter has %d ops:"), NumOps);
	for (int32 OpIndex = 0; OpIndex < NumOps; ++OpIndex)
	{
		const FName OpName = RtCtl->GetOpName(OpIndex);
		const FRetargetChainMapping* Mapping = RtCtl->GetChainMapping(OpName);
		FIKRetargetOpBase* OpPtr = RtCtl->GetRetargetOpByIndex(OpIndex);
		FString Extra;
		if (OpPtr)
		{
			Extra = OpPtr->IsEnabled() ? TEXT(" enabled") : TEXT(" DISABLED");
			if (OpPtr->GetType() == FIKRetargetFloorConstraintOp::StaticStruct())
			{
				const auto* FS = static_cast<const FIKRetargetFloorConstraintOpSettings*>(OpPtr->GetSettingsConst());
				Extra += FString::Printf(TEXT(" chainsToAffect=%d alpha=%.2f"), FS ? FS->ChainsToAffect.Num() : -1, FS ? FS->Alpha : -1.0);
			}
			else if (OpPtr->GetType() == FIKRetargetSpeedPlantingOp::StaticStruct())
			{
				const auto* SS = static_cast<const FIKRetargetSpeedPlantingOpSettings*>(OpPtr->GetSettingsConst());
				Extra += FString::Printf(TEXT(" chainsToPlant=%d"), SS ? SS->ChainsToSpeedPlant.Num() : -1);
				if (SS)
				{
					for (const FRetargetSpeedPlantingSettings& C : SS->ChainsToSpeedPlant)
					{
						Extra += FString::Printf(TEXT(" [%s<-%s]"), *C.TargetChainName.ToString(), *C.SpeedCurveName.ToString());
					}
				}
			}
		}
		UE_LOG(LogMocaraRetarget, Display, TEXT("  op[%d] '%s' chainMapping=%s%s"),
			OpIndex, *OpName.ToString(), Mapping ? TEXT("yes") : TEXT("NONE"), *Extra);
		if (!Mapping)
		{
			continue;
		}
		for (const FName TargetChain : Mapping->GetChainNames(ERetargetSourceOrTarget::Target))
		{
			const FName Source = Mapping->GetChainMappedTo(TargetChain, ERetargetSourceOrTarget::Target);
			UE_LOG(LogMocaraRetarget, Display, TEXT("      %-22s <- %s"),
				*TargetChain.ToString(), Source.IsNone() ? TEXT("*** UNMAPPED ***") : *Source.ToString());
		}
	}

	UAnimSequence* Out = FMocaraRetargeter::Retarget(
		Clip.Sequence, Clip.Mesh, Manny, TargetProfile, Fwd, TEXT("/Game/Mocara/Verify"),
		FString::Printf(TEXT("AS_RtVerify_%s"), *TargetAssetId.ToString()), Error);
	UE_LOG(LogMocaraRetarget, Display, TEXT("retarget result: %s %s"), *GetNameSafe(Out), *Error);
	if (!Out)
	{
		return;
	}

	// End-to-end quality metric. Absolute limb directions (and even swing-from-rest) are
	// contaminated by the SOMA/Manny rest-pose difference, so compare INTRINSIC joint
	// angles instead -- the angle between two adjacent segments is independent of rest
	// pose and of global orientation, so it is a fair like-for-like pose comparison.
	struct FAngleProbe
	{
		const TCHAR* Label;
		FName SA1, SB1, SA2, SB2;   // SOMA: segment 1, segment 2
		FName MA1, MB1, MA2, MB2;   // Manny: segment 1, segment 2
	};
	static const TArray<FAngleProbe> Probes = {
		{TEXT("elbow L"),    TEXT("LeftArm"), TEXT("LeftForeArm"),  TEXT("LeftForeArm"), TEXT("LeftHand"),
		                     TEXT("upperarm_l"), TEXT("lowerarm_l"), TEXT("lowerarm_l"), TEXT("hand_l")},
		{TEXT("elbow R"),    TEXT("RightArm"), TEXT("RightForeArm"), TEXT("RightForeArm"), TEXT("RightHand"),
		                     TEXT("upperarm_r"), TEXT("lowerarm_r"), TEXT("lowerarm_r"), TEXT("hand_r")},
		{TEXT("knee L"),     TEXT("LeftLeg"), TEXT("LeftShin"),     TEXT("LeftShin"), TEXT("LeftFoot"),
		                     TEXT("thigh_l"), TEXT("calf_l"),       TEXT("calf_l"), TEXT("foot_l")},
		{TEXT("knee R"),     TEXT("RightLeg"), TEXT("RightShin"),   TEXT("RightShin"), TEXT("RightFoot"),
		                     TEXT("thigh_r"), TEXT("calf_r"),       TEXT("calf_r"), TEXT("foot_r")},
		{TEXT("shoulder L"), TEXT("Spine1"), TEXT("Chest"),         TEXT("LeftArm"), TEXT("LeftForeArm"),
		                     TEXT("spine_01"), TEXT("spine_05"),    TEXT("upperarm_l"), TEXT("lowerarm_l")},
		{TEXT("shoulder R"), TEXT("Spine1"), TEXT("Chest"),         TEXT("RightArm"), TEXT("RightForeArm"),
		                     TEXT("spine_01"), TEXT("spine_05"),    TEXT("upperarm_r"), TEXT("lowerarm_r")},
		{TEXT("hip L"),      TEXT("Spine1"), TEXT("Chest"),         TEXT("LeftLeg"), TEXT("LeftShin"),
		                     TEXT("spine_01"), TEXT("spine_05"),    TEXT("thigh_l"), TEXT("calf_l")},
		{TEXT("hip R"),      TEXT("Spine1"), TEXT("Chest"),         TEXT("RightLeg"), TEXT("RightShin"),
		                     TEXT("spine_01"), TEXT("spine_05"),    TEXT("thigh_r"), TEXT("calf_r")},
	};

	const IAnimationDataModel* OutModel = Out->GetDataModel();
	const int32 NumFrames = OutModel ? OutModel->GetNumberOfFrames() : 0;
	const int32 Step = FMath::Max(1, NumFrames / 30);

	auto AngleBetween = [](const FVector& A, const FVector& B)
	{
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp((float)FVector::DotProduct(A, B), -1.f, 1.f)));
	};

	TMap<FString, float> SumErr;
	TMap<FString, float> MaxErr;
	TMap<FString, int32> Count;
	for (int32 Frame = 0; Frame <= NumFrames; Frame += Step)
	{
		const TArray<FTransform> SomaPose = ComponentPoseAt(Clip.Mesh, Clip.Sequence, Frame);
		const TArray<FTransform> MannyPose = ComponentPoseAt(Manny, Out, Frame);
		for (const FAngleProbe& Probe : Probes)
		{
			FVector S1, S2, M1, M2;
			if (!BoneDirection(Clip.Mesh, SomaPose, Probe.SA1, Probe.SB1, S1)) { continue; }
			if (!BoneDirection(Clip.Mesh, SomaPose, Probe.SA2, Probe.SB2, S2)) { continue; }
			if (!BoneDirection(Manny, MannyPose, Probe.MA1, Probe.MB1, M1)) { continue; }
			if (!BoneDirection(Manny, MannyPose, Probe.MA2, Probe.MB2, M2)) { continue; }

			const float Deg = FMath::Abs(AngleBetween(S1, S2) - AngleBetween(M1, M2));
			const FString Key = Probe.Label;
			SumErr.FindOrAdd(Key) += Deg;
			MaxErr.FindOrAdd(Key) = FMath::Max(MaxErr.FindOrAdd(Key), Deg);
			Count.FindOrAdd(Key) += 1;
		}
	}

	UE_LOG(LogMocaraRetarget, Display, TEXT("intrinsic joint-angle error, SOMA vs retargeted target (degrees):"));
	for (const FAngleProbe& Probe : Probes)
	{
		const FString Key = Probe.Label;
		const int32* N = Count.Find(Key);
		if (!N || *N == 0) { continue; }
		const float Mean = SumErr[Key] / *N;
		UE_LOG(LogMocaraRetarget, Display, TEXT("    %-12s mean %6.2f   max %6.2f%s"),
			Probe.Label, Mean, MaxErr[Key], (Mean > 15.f) ? TEXT("   <-- SUSPECT") : TEXT(""));
	}
}

static FAutoConsoleCommand GMocaraVerifyRetargetCommand(
	TEXT("Mocara.VerifyRetarget"),
	TEXT("Dump Mocara's IK rig chains and retarget mapping: Mocara.VerifyRetarget <bvh> [targetMeshPath]"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&MocaraVerifyRetargetCommand));

/**
 * Export full component-space skeletons (source SOMA + retargeted Manny) to CSV so the
 * motion can be plotted and eyeballed outside the editor.
 *   Mocara.ExportPoseCsv <bvh-path> <out-csv>
 */
static void MocaraExportPoseCsvCommand(const TArray<FString>& Args)
{
	if (Args.Num() < 2)
	{
		UE_LOG(LogMocaraRetarget, Error, TEXT("Usage: Mocara.ExportPoseCsv <bvh-path> <out-csv>"));
		return;
	}

	FMocaraImportedClip Clip;
	FString Error;
	if (!FMocaraBvhImporter::ImportFile(Args[0], TEXT("/Game/Mocara/Verify"), TEXT("AS_CsvVerify"), false, Clip, Error, /*bSaveAssets=*/false))
	{
		UE_LOG(LogMocaraRetarget, Error, TEXT("Import failed: %s"), *Error);
		return;
	}
	FMocaraTargetProfile TargetProfile;
	USkeletalMesh* Manny = FMocaraRetargeter::FindTargetMesh(TargetProfile, Error);
	if (!Manny)
	{
		UE_LOG(LogMocaraRetarget, Error, TEXT("%s"), *Error);
		return;
	}

	const FName TargetAssetId = FMocaraRetargeter::MakeTargetAssetId(Manny, TargetProfile);
	UIKRigDefinition* SomaRig = FMocaraRetargeter::EnsureIkRig(Clip.Mesh, TEXT("/Game/Mocara/Verify"), TEXT("IK_SOMA_C"), true);
	UIKRigDefinition* MannyRig = FMocaraRetargeter::EnsureIkRig(
		Manny, TEXT("/Game/Mocara/Verify"),
		FName(*FString::Printf(TEXT("IK_%s_C"), *TargetAssetId.ToString())), false);
	UIKRetargeter* Fwd = FMocaraRetargeter::EnsureRetargeter(
		SomaRig, MannyRig, TEXT("/Game/Mocara/Verify"),
		FName(*FString::Printf(TEXT("RTG_%s_C"), *TargetAssetId.ToString())));
	UAnimSequence* Out = FMocaraRetargeter::Retarget(
		Clip.Sequence, Clip.Mesh, Manny, TargetProfile, Fwd, TEXT("/Game/Mocara/Verify"),
		FString::Printf(TEXT("AS_CsvVerify_%s"), *TargetAssetId.ToString()), Error);
	if (!Out)
	{
		UE_LOG(LogMocaraRetarget, Error, TEXT("Retarget failed: %s"), *Error);
		return;
	}

	FString Csv = TEXT("skeleton,frame,bone,parent,x,y,z\n");
	auto Dump = [&Csv](const TCHAR* Label, USkeletalMesh* Mesh, UAnimSequence* Seq, int32 NumFrames)
	{
		const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
		for (int32 Frame = 0; Frame <= NumFrames; ++Frame)
		{
			const TArray<FTransform> Pose = ComponentPoseAt(Mesh, Seq, Frame);
			for (int32 Index = 0; Index < Ref.GetNum(); ++Index)
			{
				const FVector P = Pose[Index].GetLocation();
				Csv += FString::Printf(TEXT("%s,%d,%s,%d,%.3f,%.3f,%.3f\n"),
					Label, Frame, *Ref.GetBoneName(Index).ToString(), Ref.GetParentIndex(Index), P.X, P.Y, P.Z);
			}
		}
	};

	const IAnimationDataModel* OutModel = Out->GetDataModel();
	const int32 NumFrames = OutModel ? OutModel->GetNumberOfFrames() : 0;
	Dump(TEXT("soma"), Clip.Mesh, Clip.Sequence, NumFrames);
	Dump(TEXT("manny"), Manny, Out, NumFrames);

	if (FFileHelper::SaveStringToFile(Csv, *Args[1]))
	{
		UE_LOG(LogMocaraRetarget, Display, TEXT("Wrote %d frames of both skeletons to %s"), NumFrames + 1, *Args[1]);
	}
	else
	{
		UE_LOG(LogMocaraRetarget, Error, TEXT("Could not write %s"), *Args[1]);
	}
}

static FAutoConsoleCommand GMocaraExportPoseCsvCommand(
	TEXT("Mocara.ExportPoseCsv"),
	TEXT("Export source + retargeted skeletons to CSV: Mocara.ExportPoseCsv <bvh> <out-csv>"),
	FConsoleCommandWithArgsDelegate::CreateStatic(&MocaraExportPoseCsvCommand));
