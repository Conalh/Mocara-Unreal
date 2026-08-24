#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MocaraAutoPose.h"
#include "MocaraPoseEditing.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraPoseEditingUpsertTest,
	"Mocara.Viewport.PoseEditing.UpsertRotationKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraPoseEditingUpsertTest::RunTest(const FString& Parameters)
{
	TArray<FMocaraPoseKey> Keys;
	const FName Bone = TEXT("LeftArm");

	const int32 AddedIndex = FMocaraPoseEditing::UpsertRotationKey(
		Keys, 12, Bone, FRotator(10.f, 20.f, 30.f), 6, 3, 4, false);
	TestEqual(TEXT("A missing frame/bone key is added"), AddedIndex, 0);
	TestEqual(TEXT("Only one key exists"), Keys.Num(), 1);
	TestEqual(TEXT("The key frame is retained"), Keys[0].Frame, 12);
	TestEqual(TEXT("The selected bone is retained"), Keys[0].BoneName, Bone);

	Keys[0].TranslationOffset = FVector(1.f, 2.f, 3.f);
	const int32 UpdatedIndex = FMocaraPoseEditing::UpsertRotationKey(
		Keys, 12, Bone, FRotator(-5.f, 15.f, 25.f), 9, 5, 7, true);
	TestEqual(TEXT("The matching key is updated in place"), UpdatedIndex, AddedIndex);
	TestEqual(TEXT("Upsert does not duplicate a key"), Keys.Num(), 1);
	TestTrue(TEXT("Unrelated translation data is preserved"), Keys[0].TranslationOffset.Equals(FVector(1.f, 2.f, 3.f)));
	TestTrue(TEXT("Rotation is updated"), Keys[0].RotationOffset.Equals(FRotator(-5.f, 15.f, 25.f)));
	TestEqual(TEXT("Ease in is updated"), Keys[0].EaseInFrames, 9);
	TestEqual(TEXT("Hold is updated"), Keys[0].HoldFrames, 5);
	TestEqual(TEXT("Ease out is updated"), Keys[0].EaseOutFrames, 7);
	TestTrue(TEXT("IK intent is updated"), Keys[0].bUseTwoBoneIK);

	const int32 TransformIndex = FMocaraPoseEditing::UpsertTransformKey(
		Keys, 12, Bone, FRotator(1.f, 2.f, 3.f), FVector(4.f, 5.f, 6.f), 5, 2, 6, true);
	TestEqual(TEXT("Transform upsert updates the same key"), TransformIndex, AddedIndex);
	TestTrue(TEXT("Transform upsert writes rotation"), Keys[0].RotationOffset.Equals(FRotator(1.f, 2.f, 3.f)));
	TestTrue(TEXT("Transform upsert writes translation"), Keys[0].TranslationOffset.Equals(FVector(4.f, 5.f, 6.f)));
	TestEqual(TEXT("Transform upsert writes ease in"), Keys[0].EaseInFrames, 5);
	TestEqual(TEXT("Transform upsert writes hold"), Keys[0].HoldFrames, 2);
	TestEqual(TEXT("Transform upsert writes ease out"), Keys[0].EaseOutFrames, 6);
	FMocaraPoseEditing::UpsertTransformKey(
		Keys, 12, Bone, FRotator::ZeroRotator, FVector::ZeroVector, -2, 0, -4, false);
	TestEqual(TEXT("Ease in cannot be negative"), Keys[0].EaseInFrames, 0);
	TestEqual(TEXT("A key always holds for at least one frame"), Keys[0].HoldFrames, 1);
	TestEqual(TEXT("Ease out cannot be negative"), Keys[0].EaseOutFrames, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraPoseEditingKeyIntervalTest,
	"Mocara.Viewport.PoseEditing.KeyIntervals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraPoseEditingKeyIntervalTest::RunTest(const FString& Parameters)
{
	FMocaraPoseKey Key;
	Key.Frame = 10;
	Key.EaseInFrames = 4;
	Key.HoldFrames = 3;
	Key.EaseOutFrames = 6;

	TestEqual(TEXT("The active interval begins at the start of ease in"),
		FMocaraPoseEditing::IntervalStartFrame(Key), 6);
	TestEqual(TEXT("The active interval ends after ease out"),
		FMocaraPoseEditing::IntervalEndFrame(Key), 18);
	TestTrue(TEXT("Ease in uses a smooth half-weight midpoint"),
		FMath::IsNearlyEqual(FMocaraPoseEditing::EaseWeight(8, Key), 0.5f));
	TestTrue(TEXT("The first hold frame has full strength"),
		FMath::IsNearlyEqual(FMocaraPoseEditing::EaseWeight(10, Key), 1.f));
	TestTrue(TEXT("The final hold frame has full strength"),
		FMath::IsNearlyEqual(FMocaraPoseEditing::EaseWeight(12, Key), 1.f));
	TestTrue(TEXT("Ease out uses a smooth half-weight midpoint"),
		FMath::IsNearlyEqual(FMocaraPoseEditing::EaseWeight(15, Key), 0.5f));
	TestTrue(TEXT("The ease-in boundary is inactive"),
		FMath::IsNearlyZero(FMocaraPoseEditing::EaseWeight(6, Key)));
	TestTrue(TEXT("The ease-out boundary is inactive"),
		FMath::IsNearlyZero(FMocaraPoseEditing::EaseWeight(18, Key)));
	TArray<FMocaraPoseKey> Keys = {Key};
	TArray<int32> ActiveFrames;
	FMocaraPoseEditing::CollectActiveFrames(Keys, 16, ActiveFrames);
	const TArray<int32> ExpectedFrames = {7, 8, 9, 10, 11, 12, 13, 14, 15};
	TestTrue(TEXT("Constraint frame collection clips the interval to the animation"),
		ActiveFrames == ExpectedFrames);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraPoseEditingResizeIntervalTest,
	"Mocara.Viewport.PoseEditing.ResizeKeyIntervals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraPoseEditingResizeIntervalTest::RunTest(const FString& Parameters)
{
	FMocaraPoseKey Key;
	Key.Frame = 10;
	Key.EaseInFrames = 4;
	Key.HoldFrames = 3;
	Key.EaseOutFrames = 6;

	FMocaraPoseEditing::ResizeInterval(Key, EMocaraPoseIntervalHandle::EaseInStart, 3);
	TestEqual(TEXT("Dragging the left handle changes ease in"), Key.EaseInFrames, 7);
	FMocaraPoseEditing::ResizeInterval(Key, EMocaraPoseIntervalHandle::HoldEnd, 17);
	TestEqual(TEXT("Dragging the bright region's right handle changes hold"), Key.HoldFrames, 7);
	FMocaraPoseEditing::ResizeInterval(Key, EMocaraPoseIntervalHandle::EaseOutEnd, 25);
	TestEqual(TEXT("Dragging the outer right handle changes ease out"), Key.EaseOutFrames, 8);

	FMocaraPoseEditing::ResizeInterval(Key, EMocaraPoseIntervalHandle::EaseInStart, 20);
	TestEqual(TEXT("Ease in clamps at zero when dragged past the key"), Key.EaseInFrames, 0);
	FMocaraPoseEditing::ResizeInterval(Key, EMocaraPoseIntervalHandle::HoldEnd, 4);
	TestEqual(TEXT("Hold cannot be collapsed below one frame"), Key.HoldFrames, 1);
	FMocaraPoseEditing::ResizeInterval(Key, EMocaraPoseIntervalHandle::EaseOutEnd, 4);
	TestEqual(TEXT("Ease out clamps at zero before the hold boundary"), Key.EaseOutFrames, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraPoseEditingFriendlyLabelsTest,
	"Mocara.Viewport.PoseEditing.FriendlyLabels",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraPoseEditingFriendlyLabelsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("SOMA arm receives a friendly label"),
		FMocaraPoseEditing::DisplayLabel(TEXT("LeftArm")), FString(TEXT("Left Upper Arm")));
	TestEqual(TEXT("Manny arm receives the same friendly label"),
		FMocaraPoseEditing::DisplayLabel(TEXT("upperarm_l")), FString(TEXT("Left Upper Arm")));
	TestEqual(TEXT("SOMA toe base is presented in plain language"),
		FMocaraPoseEditing::DisplayLabel(TEXT("LeftToeBase")), FString(TEXT("Left Toes")));
	TestEqual(TEXT("Manny ball bone receives the same friendly label"),
		FMocaraPoseEditing::DisplayLabel(TEXT("ball_l")), FString(TEXT("Left Toes")));
	TestEqual(TEXT("An unknown bone retains its exact identifier"),
		FMocaraPoseEditing::DisplayLabel(TEXT("custom_driver")), FString(TEXT("custom_driver")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraPoseEditingSelectionTest,
	"Mocara.Viewport.PoseEditing.Selection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraPoseEditingSelectionTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("A deform bone can be selected"), FMocaraPoseEditing::IsSelectableBone(TEXT("upperarm_l")));
	TestFalse(TEXT("The static root is excluded"), FMocaraPoseEditing::IsSelectableBone(TEXT("root")));
	TestFalse(TEXT("IK helpers are excluded"), FMocaraPoseEditing::IsSelectableBone(TEXT("ik_hand_l")));
	TestFalse(TEXT("SOMA interaction helper is excluded"), FMocaraPoseEditing::IsSelectableBone(TEXT("interaction")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraPoseEditingRotationTest,
	"Mocara.Viewport.PoseEditing.RotationAccumulation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraPoseEditingRotationTest::RunTest(const FString& Parameters)
{
	const FRotator Current(20.f, 35.f, 5.f);
	const FRotator Delta(0.f, 25.f, 0.f);
	const FQuat LocalExpected = Current.Quaternion() * Delta.Quaternion();
	const FQuat WorldExpected = Delta.Quaternion() * Current.Quaternion();

	TestTrue(TEXT("Local deltas post-multiply the existing offset"),
		FMocaraPoseEditing::AccumulateRotation(Current, Delta, true).Quaternion().Equals(LocalExpected));
	TestTrue(TEXT("World deltas pre-multiply the existing offset"),
		FMocaraPoseEditing::AccumulateRotation(Current, Delta, false).Quaternion().Equals(WorldExpected));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraPoseEditingEvaluationTest,
	"Mocara.Viewport.PoseEditing.EvaluateFullPose",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraPoseEditingEvaluationTest::RunTest(const FString& Parameters)
{
	TArray<FMocaraPoseKey> Keys;
	FMocaraPoseKey Head;
	Head.Frame = 10;
	Head.BoneName = TEXT("head");
	Head.RotationOffset = FRotator(0.f, 90.f, 0.f);
	Head.EaseInFrames = 4;
	Head.EaseOutFrames = 4;
	Keys.Add(Head);

	FMocaraPoseKey Hand;
	Hand.Frame = 8;
	Hand.BoneName = TEXT("hand_l");
	Hand.TranslationOffset = FVector(10.f, 0.f, 0.f);
	Hand.EaseInFrames = 4;
	Hand.EaseOutFrames = 4;
	Hand.bUseTwoBoneIK = true;
	Keys.Add(Hand);

	TMap<FName, FMocaraPoseKey> Pose;
	FMocaraPoseEditing::EvaluatePoseAtFrame(Keys, 8, Pose);

	TestEqual(TEXT("Every active bone contributes to the evaluated pose"), Pose.Num(), 2);
	TestTrue(TEXT("Rotation uses the smooth ease weight"),
		Pose.FindChecked(TEXT("head")).RotationOffset.Equals(FRotator(0.f, 45.f, 0.f), 0.01f));
	TestTrue(TEXT("Translation is retained at its keyed frame"),
		Pose.FindChecked(TEXT("hand_l")).TranslationOffset.Equals(FVector(10.f, 0.f, 0.f)));
	TestTrue(TEXT("IK intent survives evaluation"), Pose.FindChecked(TEXT("hand_l")).bUseTwoBoneIK);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraPoseEditingOverlapTest,
	"Mocara.Viewport.PoseEditing.NormalizedOverlap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraPoseEditingOverlapTest::RunTest(const FString& Parameters)
{
	TArray<FMocaraPoseKey> Keys;
	FMocaraPoseKey First;
	First.Frame = 0;
	First.BoneName = TEXT("hand_l");
	First.TranslationOffset = FVector(0.f, 0.f, 25.f);
	First.RotationOffset = FRotator(0.f, 30.f, 0.f);
	First.EaseInFrames = 8;
	First.EaseOutFrames = 8;
	Keys.Add(First);

	FMocaraPoseKey Duplicate = First;
	Duplicate.Frame = 2;
	Keys.Add(Duplicate);

	TMap<FName, FMocaraPoseKey> Pose;
	FMocaraPoseEditing::EvaluatePoseAtFrame(Keys, 1, Pose);
	FMocaraPoseKey Evaluated = Pose.FindChecked(TEXT("hand_l"));
	TestTrue(TEXT("Overlapping identical translations do not amplify"),
		Evaluated.TranslationOffset.Equals(First.TranslationOffset, 0.01f));
	TestTrue(TEXT("Overlapping identical rotations do not amplify"),
		Evaluated.RotationOffset.Equals(First.RotationOffset, 0.01f));

	Keys[1].TranslationOffset = FVector(0.f, 0.f, 50.f);
	Keys[1].RotationOffset = FRotator(0.f, 60.f, 0.f);
	FMocaraPoseEditing::EvaluatePoseAtFrame(Keys, 2, Pose);
	Evaluated = Pose.FindChecked(TEXT("hand_l"));
	TestTrue(TEXT("An exact key is not diluted by a neighbouring key"),
		Evaluated.TranslationOffset.Equals(Keys[1].TranslationOffset, 0.01f));
	TestTrue(TEXT("An exact keyed rotation is preserved"),
		Evaluated.RotationOffset.Equals(Keys[1].RotationOffset, 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraPoseEditingLaneTest,
	"Mocara.Viewport.PoseEditing.SemanticLanes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraPoseEditingLaneTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("Manny pelvis uses the root lane"),
		FMocaraPoseEditing::ClassifyBoneLane(TEXT("pelvis")), EMocaraPoseLane::RootPath);
	TestEqual(TEXT("SOMA hips uses the root lane"),
		FMocaraPoseEditing::ClassifyBoneLane(TEXT("Hips")), EMocaraPoseLane::RootPath);
	TestEqual(TEXT("Manny left hand uses the left-hand lane"),
		FMocaraPoseEditing::ClassifyBoneLane(TEXT("hand_l")), EMocaraPoseLane::LeftHand);
	TestEqual(TEXT("SOMA left hand uses the left-hand lane"),
		FMocaraPoseEditing::ClassifyBoneLane(TEXT("LeftHand")), EMocaraPoseLane::LeftHand);
	TestEqual(TEXT("Manny right foot uses the right-foot lane"),
		FMocaraPoseEditing::ClassifyBoneLane(TEXT("foot_r")), EMocaraPoseLane::RightFoot);
	TestEqual(TEXT("SOMA right shin uses the right-foot lane"),
		FMocaraPoseEditing::ClassifyBoneLane(TEXT("RightShin")), EMocaraPoseLane::RightFoot);
	TestEqual(TEXT("Head remains a full-body correction"),
		FMocaraPoseEditing::ClassifyBoneLane(TEXT("head")), EMocaraPoseLane::FullBody);

	TestTrue(TEXT("Hands expose translation manipulation"), FMocaraPoseEditing::CanTranslateBone(TEXT("hand_l")));
	TestTrue(TEXT("Feet expose translation manipulation"), FMocaraPoseEditing::CanTranslateBone(TEXT("LeftFoot")));
	TestTrue(TEXT("Hips expose translation manipulation"), FMocaraPoseEditing::CanTranslateBone(TEXT("pelvis")));
	TestFalse(TEXT("Head remains rotation-only"), FMocaraPoseEditing::CanTranslateBone(TEXT("head")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraPoseEditingMoveKeyTest,
	"Mocara.Viewport.PoseEditing.MoveAndDuplicateKeys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraPoseEditingMoveKeyTest::RunTest(const FString& Parameters)
{
	TArray<FMocaraPoseKey> Keys;
	FMocaraPoseKey Source;
	Source.Frame = 3;
	Source.BoneName = TEXT("hand_l");
	Source.TranslationOffset = FVector(8.f, 0.f, 0.f);
	Keys.Add(Source);

	const int32 Duplicate = FMocaraPoseEditing::DuplicateKey(Keys, 0, 7);
	TestEqual(TEXT("Duplicate adds a key"), Keys.Num(), 2);
	TestEqual(TEXT("Duplicate lands at the requested frame"), Keys[Duplicate].Frame, 7);
	TestTrue(TEXT("Duplicate retains transform data"), Keys[Duplicate].TranslationOffset.Equals(Source.TranslationOffset));

	const int32 Moved = FMocaraPoseEditing::MoveKey(Keys, Duplicate, 3);
	TestEqual(TEXT("Moving onto the same bone and frame replaces the older key"), Keys.Num(), 1);
	TestEqual(TEXT("Moved key remains selected"), Moved, 0);
	TestEqual(TEXT("Moved key reaches its target"), Keys[Moved].Frame, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraPoseEditingNearestKeyTest,
	"Mocara.Viewport.PoseEditing.NearestKey",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraPoseEditingNearestKeyTest::RunTest(const FString& Parameters)
{
	TArray<FMocaraPoseKey> Keys;
	for (const TPair<int32, FName>& Entry : {
		TPair<int32, FName>(12, TEXT("hand_l")),
		TPair<int32, FName>(8, TEXT("hand_l")),
		TPair<int32, FName>(10, TEXT("hand_r"))})
	{
		FMocaraPoseKey& Key = Keys.AddDefaulted_GetRef();
		Key.Frame = Entry.Key;
		Key.BoneName = Entry.Value;
	}

	TestEqual(TEXT("The closest key for the requested bone is returned"),
		FMocaraPoseEditing::FindNearestKey(Keys, 11, TEXT("hand_l")), 0);
	TestEqual(TEXT("An equal-distance tie prefers the earlier key"),
		FMocaraPoseEditing::FindNearestKey(Keys, 10, TEXT("hand_l")), 1);
	TestEqual(TEXT("Other bones are ignored"),
		FMocaraPoseEditing::FindNearestKey(Keys, 10, TEXT("head")), INDEX_NONE);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraPoseEditingIkTest,
	"Mocara.Viewport.PoseEditing.TwoBoneIK",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraPoseEditingIkTest::RunTest(const FString& Parameters)
{
	FTransform Root(FQuat::Identity, FVector(0.f, 0.f, 0.f));
	FTransform Joint(FQuat::Identity, FVector(45.f, 20.f, 0.f));
	FTransform Effector(FQuat::Identity, FVector(90.f, 0.f, 0.f));
	const float UpperLength = FVector::Distance(Root.GetLocation(), Joint.GetLocation());
	const float LowerLength = FVector::Distance(Joint.GetLocation(), Effector.GetLocation());
	const FVector Target(70.f, 35.f, 0.f);

	FMocaraAutoPose::SolveTwoBoneIK(Root, Joint, Effector, Target);
	TestTrue(TEXT("IK reaches a target inside the chain reach"), Effector.GetLocation().Equals(Target, 0.01f));
	TestTrue(TEXT("IK preserves the upper-limb length"),
		FMath::IsNearlyEqual(FVector::Distance(Root.GetLocation(), Joint.GetLocation()), UpperLength, 0.01f));
	TestTrue(TEXT("IK preserves the lower-limb length"),
		FMath::IsNearlyEqual(FVector::Distance(Joint.GetLocation(), Effector.GetLocation()), LowerLength, 0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraTwoHandGripTargetTest,
	"Mocara.Viewport.PoseEditing.TwoHandGripTarget",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraTwoHandGripTargetTest::RunTest(const FString& Parameters)
{
	const FTransform FirstRight(FQuat::Identity, FVector(-40.f, 0.f, 100.f));
	const FTransform FirstLeft(FQuat(FVector::UpVector, FMath::DegreesToRadians(20.f)), FVector(40.f, 0.f, 100.f));
	const FQuat Swing = FQuat(FVector::UpVector, FMath::DegreesToRadians(90.f));
	const FTransform CurrentRight(Swing, FVector(10.f, -30.f, 125.f));
	const FTransform CurrentLeft(FQuat::Identity, FVector(70.f, 10.f, 135.f));

	const FMocaraTwoHandGripPose Grip = FMocaraAutoPose::BuildTwoHandGripPose(
		FirstLeft, FirstRight, CurrentLeft, CurrentRight, 14.f);

	TestTrue(TEXT("Grip keeps the original two-hand midpoint"),
		((Grip.LeftHand.GetLocation() + Grip.RightHand.GetLocation()) * 0.5f)
			.Equals((CurrentLeft.GetLocation() + CurrentRight.GetLocation()) * 0.5f, 0.01f));
	TestTrue(TEXT("Grip enforces the requested hand spacing"),
		FMath::IsNearlyEqual(
			FVector::Distance(Grip.LeftHand.GetLocation(), Grip.RightHand.GetLocation()), 14.f, 0.01f));
	TestTrue(TEXT("Left wrist follows the dominant-hand swing frame"),
		Grip.LeftHand.GetRotation().Equals(Swing * FirstLeft.GetRotation(), 0.001f));

	const FMocaraTwoHandGripPose AutoSpacingGrip = FMocaraAutoPose::BuildTwoHandGripPose(
		FirstLeft, FirstRight, CurrentLeft, CurrentRight, 0.f);
	TestTrue(TEXT("Automatic spacing preserves the source item's two-hand distance"),
		FMath::IsNearlyEqual(
			FVector::Distance(
				AutoSpacingGrip.LeftHand.GetLocation(), AutoSpacingGrip.RightHand.GetLocation()),
			FVector::Distance(FirstLeft.GetLocation(), FirstRight.GetLocation()),
			0.01f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraKimodoConstraintJointOrderTest,
	"Mocara.Viewport.PoseEditing.KimodoConstraintJointOrder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraKimodoConstraintJointOrderTest::RunTest(const FString& Parameters)
{
	TArray<FName> UnrealBoneNames;
	UnrealBoneNames.Add(TEXT("Root"));
	UnrealBoneNames.Add(TEXT("Hips"));
	for (int32 Index = 2; Index < 78; ++Index)
	{
		UnrealBoneNames.Add(*FString::Printf(TEXT("SomaBone%d"), Index));
	}

	TArray<int32> KimodoBoneIndices;
	FString Error;
	if (!TestTrue(TEXT("The Unreal-only root is accepted"),
		FMocaraAutoPose::ResolveKimodoConstraintBoneIndices(
			UnrealBoneNames, KimodoBoneIndices, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("Kimodo receives exactly SOMA77 joints"), KimodoBoneIndices.Num(), 77);
	TestEqual(TEXT("Hips is the first serialized joint"), KimodoBoneIndices[0], 1);
	TestEqual(TEXT("The last SOMA joint is retained"), KimodoBoneIndices.Last(), 77);
	return true;
}

#endif
