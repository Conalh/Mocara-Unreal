#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MocaraTargetProfile.h"
#include "Engine/SkeletalMesh.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraUe5MannequinMotionBasisTest,
	"Mocara.Retarget.TargetProfile.UE5MannequinMotionBasis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraUe5MannequinMotionBasisTest::RunTest(const FString& Parameters)
{
	const FMocaraTargetProfile Profile = FMocaraTargetProfile::Ue5Mannequin();
	TestEqual(TEXT("The built-in profile has a stable identifier"), Profile.ProfileName, FName(TEXT("UE5Mannequin")));
	TestEqual(TEXT("Manny motion is carried by the pelvis track"), Profile.MotionRootBone, FName(TEXT("pelvis")));
	TestEqual(TEXT("Both upper-arm and forearm directions are source-driven"), Profile.SourceDrivenSegments.Num(), 4);
	TestEqual(TEXT("Left upper-arm source starts at LeftArm"),
		Profile.SourceDrivenSegments[0].SourceBone, FName(TEXT("LeftArm")));
	TestEqual(TEXT("Left upper-arm target ends at lowerarm_l"),
		Profile.SourceDrivenSegments[0].TargetChildBone, FName(TEXT("lowerarm_l")));
	TestEqual(TEXT("Right forearm target ends at hand_r"),
		Profile.SourceDrivenSegments[3].TargetChildBone, FName(TEXT("hand_r")));
	TestEqual(TEXT("Both arm twist sets and hand subtrees preserve local rotation"),
		Profile.LocalRotationSubtreeRoots.Num(), 10);
	TestTrue(TEXT("Left upper-arm twist inherits the corrected upper arm"),
		Profile.LocalRotationSubtreeRoots.Contains(FName(TEXT("upperarm_twist_01_l"))));
	TestTrue(TEXT("Right lower-arm twist inherits the corrected lower arm"),
		Profile.LocalRotationSubtreeRoots.Contains(FName(TEXT("lowerarm_twist_02_r"))));
	TestTrue(TEXT("Finger rotations inherit through the left hand subtree"),
		Profile.LocalRotationSubtreeRoots.Contains(FName(TEXT("hand_l"))));
	TestFalse(TEXT("The source-driven left upper arm still receives motion-basis correction"),
		Profile.LocalRotationSubtreeRoots.Contains(FName(TEXT("upperarm_l"))));
	TestFalse(TEXT("The source-driven right forearm still receives motion-basis correction"),
		Profile.LocalRotationSubtreeRoots.Contains(FName(TEXT("lowerarm_r"))));

	const FVector ReferencePosition(0.f, 0.f, 95.f);
	const FVector SourceBasisMotion(100.f, 0.f, 90.f);
	const FVector TargetBasisMotion = Profile.ReorientRootTranslation(ReferencePosition, SourceBasisMotion);

	TestTrue(TEXT("Canonical +X travel becomes UE5 mannequin-native +Y travel"),
		TargetBasisMotion.Equals(FVector(0.f, 100.f, 90.f), 0.01f));
	TestTrue(TEXT("Changing motion basis preserves distance from the reference pelvis"),
		FMath::IsNearlyEqual(
			FVector::Distance(ReferencePosition, SourceBasisMotion),
			FVector::Distance(ReferencePosition, TargetBasisMotion),
			0.01f));

	const FVector RestLimb = -FVector::UpVector;
	const FQuat SourceForwardSwing(-FVector::RightVector, FMath::DegreesToRadians(30.f));
	const FVector SourceSwungLimb = SourceForwardSwing.RotateVector(RestLimb);
	const FVector ExpectedTargetSwungLimb = Profile.SourceMotionToTargetBasis.RotateVector(SourceSwungLimb);
	const FVector ActualTargetSwungLimb = Profile.ReorientAnimationDelta(SourceForwardSwing).RotateVector(RestLimb);
	TestTrue(TEXT("Animation deltas rotate limb swing into the target motion basis without rotating the rest limb"),
		ActualTargetSwungLimb.Equals(ExpectedTargetSwungLimb, 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraMetaHumanBodyProfileTest,
	"Mocara.Retarget.TargetProfile.MetaHumanBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraMetaHumanBodyProfileTest::RunTest(const FString& Parameters)
{
	USkeletalMesh* BodyMesh = LoadObject<USkeletalMesh>(
		nullptr,
		TEXT("/MetaHumanCharacter/Body/IdentityTemplate/SKM_Body.SKM_Body"));
	TestNotNull(TEXT("UE 5.8 MetaHuman Character provides its canonical body test mesh"), BodyMesh);
	if (!BodyMesh)
	{
		return false;
	}

	const FMocaraTargetProfile Profile = FMocaraTargetProfile::MetaHumanBody();
	TestEqual(TEXT("The MetaHuman body profile has a stable identifier"),
		Profile.ProfileName, FName(TEXT("MetaHumanBody")));
	TestTrue(TEXT("The profile matches Epic's canonical MetaHuman body mesh"), Profile.Matches(BodyMesh));
	TestTrue(TEXT("MetaHuman upper-arm correctives inherit the corrected base-arm pose"),
		Profile.LocalRotationSubtreeRoots.Contains(FName(TEXT("upperarm_correctiveRoot_l"))));
	TestTrue(TEXT("MetaHuman lower-leg correctives inherit the corrected base-leg pose"),
		Profile.LocalRotationSubtreeRoots.Contains(FName(TEXT("calf_correctiveRoot_r"))));

	const TOptional<FMocaraTargetProfile> Resolved = FMocaraTargetProfile::ForMesh(BodyMesh);
	TestTrue(TEXT("The canonical MetaHuman body resolves to a supported target profile"), Resolved.IsSet());
	if (Resolved.IsSet())
	{
		TestEqual(TEXT("Profile resolution prefers MetaHuman over the shared UE5 base bone set"),
			Resolved->ProfileName, FName(TEXT("MetaHumanBody")));
	}
	return true;
}

#endif
