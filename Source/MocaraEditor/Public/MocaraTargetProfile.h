#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;

/** One canonical source segment whose direction must be preserved on the target. */
struct FMocaraSourceDrivenSegment
{
	FName SourceBone = NAME_None;
	FName SourceChildBone = NAME_None;
	FName TargetBone = NAME_None;
	FName TargetChildBone = NAME_None;
};

/**
 * Target-family assumptions that must not leak into BVH import or generic retargeting.
 *
 * A profile owns the target's native motion basis, motion-root bone, source-driven
 * segment mappings, local-rotation inheritance boundaries, and minimum skeleton
 * contract. Future MetaHuman and custom-rig support can add profiles without
 * changing the canonical SOMA import path.
 */
struct FMocaraTargetProfile
{
	FName ProfileName = NAME_None;
	FName MotionRootBone = NAME_None;
	FQuat SourceMotionToTargetBasis = FQuat::Identity;
	TArray<FMocaraSourceDrivenSegment> SourceDrivenSegments;
	/** Roots whose existing local animation rotations should inherit the corrected parent pose. */
	TArray<FName> LocalRotationSubtreeRoots;
	TArray<FName> RequiredBones;

	static FMocaraTargetProfile Ue5Mannequin();
	static FMocaraTargetProfile MetaHumanBody();

	/** Resolve the most specific built-in profile supported by this target mesh. */
	static TOptional<FMocaraTargetProfile> ForMesh(const USkeletalMesh* Mesh);

	/** Re-express retargeted root motion around the target bone's local reference position. */
	FVector ReorientRootTranslation(const FVector& TargetReferenceLocalPosition, const FVector& RetargetedPosition) const;

	/** Re-express a component-space animation delta without rotating the target reference pose. */
	FQuat ReorientAnimationDelta(const FQuat& RetargetedDelta) const;

	/** True only when the target provides every bone required by this profile. */
	bool Matches(const USkeletalMesh* Mesh) const;
};
