#include "MocaraTargetProfile.h"

#include "Engine/SkeletalMesh.h"

FMocaraTargetProfile FMocaraTargetProfile::Ue5Mannequin()
{
	FMocaraTargetProfile Profile;
	Profile.ProfileName = TEXT("UE5Mannequin");
	Profile.MotionRootBone = TEXT("pelvis");
	// SOMA's imported native travel is +X, while UE5 mannequin animation is authored
	// facing +Y. UE's pelvis retarget op scales translation component-wise and does not
	// rotate it into the target basis, so the profile supplies that missing basis change.
	Profile.SourceMotionToTargetBasis = FQuat(FVector::UpVector, PI * 0.5f);
	// Preserve the source arm bend planes explicitly. A rotation-delta basis conversion
	// alone cannot do this across SOMA's T-pose and Manny's A-pose reference geometry.
	Profile.SourceDrivenSegments = {
		{TEXT("LeftArm"), TEXT("LeftForeArm"), TEXT("upperarm_l"), TEXT("lowerarm_l")},
		{TEXT("LeftForeArm"), TEXT("LeftHand"), TEXT("lowerarm_l"), TEXT("hand_l")},
		{TEXT("RightArm"), TEXT("RightForeArm"), TEXT("upperarm_r"), TEXT("lowerarm_r")},
		{TEXT("RightForeArm"), TEXT("RightHand"), TEXT("lowerarm_r"), TEXT("hand_r")}
	};
	// Twist bones, hands, and fingers already have meaningful local rotations from IK retargeting.
	// Re-expressing each one's component-space delta independently makes those children
	// fight the corrected upper/lower-arm parents and corkscrews the skinned mesh.
	Profile.LocalRotationSubtreeRoots = {
		TEXT("upperarm_twist_01_l"), TEXT("upperarm_twist_02_l"),
		TEXT("lowerarm_twist_01_l"), TEXT("lowerarm_twist_02_l"), TEXT("hand_l"),
		TEXT("upperarm_twist_01_r"), TEXT("upperarm_twist_02_r"),
		TEXT("lowerarm_twist_01_r"), TEXT("lowerarm_twist_02_r"), TEXT("hand_r")
	};
	Profile.RequiredBones = {
		TEXT("pelvis"), TEXT("spine_01"), TEXT("thigh_l"), TEXT("thigh_r"),
		TEXT("foot_l"), TEXT("foot_r"), TEXT("upperarm_l"), TEXT("lowerarm_l"), TEXT("hand_l"),
		TEXT("upperarm_twist_01_l"), TEXT("upperarm_twist_02_l"),
		TEXT("lowerarm_twist_01_l"), TEXT("lowerarm_twist_02_l"),
		TEXT("upperarm_r"), TEXT("lowerarm_r"), TEXT("hand_r"),
		TEXT("upperarm_twist_01_r"), TEXT("upperarm_twist_02_r"),
		TEXT("lowerarm_twist_01_r"), TEXT("lowerarm_twist_02_r"), TEXT("head")
	};
	return Profile;
}

FMocaraTargetProfile FMocaraTargetProfile::MetaHumanBody()
{
	// MetaHuman bodies use the same base deformation skeleton and motion basis as the
	// UE5 mannequin, then add corrective branches evaluated by the body post-process rig.
	// Preserve those branches in local space so Mocara's basis conversion moves them with
	// their corrected parent rather than independently rotating the corrective hierarchy.
	FMocaraTargetProfile Profile = Ue5Mannequin();
	Profile.ProfileName = TEXT("MetaHumanBody");
	const TArray<FName> CorrectiveRoots = {
		TEXT("upperarm_correctiveRoot_l"), TEXT("upperarm_correctiveRoot_r"),
		TEXT("lowerarm_correctiveRoot_l"), TEXT("lowerarm_correctiveRoot_r"),
		TEXT("thigh_correctiveRoot_l"), TEXT("thigh_correctiveRoot_r"),
		TEXT("calf_correctiveRoot_l"), TEXT("calf_correctiveRoot_r")
	};
	Profile.LocalRotationSubtreeRoots.Append(CorrectiveRoots);
	Profile.RequiredBones.Append(CorrectiveRoots);
	return Profile;
}

TOptional<FMocaraTargetProfile> FMocaraTargetProfile::ForMesh(const USkeletalMesh* Mesh)
{
	const FMocaraTargetProfile MetaHuman = MetaHumanBody();
	if (MetaHuman.Matches(Mesh))
	{
		return MetaHuman;
	}

	const FMocaraTargetProfile Mannequin = Ue5Mannequin();
	if (Mannequin.Matches(Mesh))
	{
		return Mannequin;
	}
	return {};
}

FVector FMocaraTargetProfile::ReorientRootTranslation(
	const FVector& TargetReferenceLocalPosition,
	const FVector& RetargetedPosition) const
{
	return TargetReferenceLocalPosition + SourceMotionToTargetBasis.RotateVector(RetargetedPosition - TargetReferenceLocalPosition);
}

FQuat FMocaraTargetProfile::ReorientAnimationDelta(const FQuat& RetargetedDelta) const
{
	return (SourceMotionToTargetBasis * RetargetedDelta * SourceMotionToTargetBasis.Inverse()).GetNormalized();
}

bool FMocaraTargetProfile::Matches(const USkeletalMesh* Mesh) const
{
	if (!Mesh || MotionRootBone.IsNone())
	{
		return false;
	}

	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	for (const FName Bone : RequiredBones)
	{
		if (Ref.FindBoneIndex(Bone) == INDEX_NONE)
		{
			return false;
		}
	}
	return true;
}
