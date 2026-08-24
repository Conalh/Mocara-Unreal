#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MocaraRetargeter.h"
#include "MocaraTargetProfile.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraMetaHumanTargetSelectionTest,
	"Mocara.Retarget.TargetSelection.MetaHumanBody",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraMetaHumanTargetSelectionTest::RunTest(const FString& Parameters)
{
	const FSoftObjectPath BodyPath(
		TEXT("/MetaHumanCharacter/Body/IdentityTemplate/SKM_Body.SKM_Body"));
	FMocaraTargetProfile Profile;
	FString Error;
	USkeletalMesh* BodyMesh = FMocaraRetargeter::ResolveTargetMesh(BodyPath, Profile, Error);
	TestNotNull(TEXT("An explicitly selected MetaHuman body mesh resolves"), BodyMesh);
	TestTrue(TEXT("A valid target does not report an error"), Error.IsEmpty());
	TestEqual(TEXT("Selection carries the MetaHuman-specific target profile"),
		Profile.ProfileName, FName(TEXT("MetaHumanBody")));

	const FName FirstAssetId = FMocaraRetargeter::MakeTargetAssetId(BodyMesh, Profile);
	const FName SecondAssetId = FMocaraRetargeter::MakeTargetAssetId(BodyMesh, Profile);
	TestTrue(TEXT("Generated retarget assets identify their MetaHuman target"),
		FirstAssetId.ToString().StartsWith(TEXT("MetaHumanBody_SKM_Body_")));
	TestEqual(TEXT("The same target path produces a stable retarget asset identifier"),
		FirstAssetId, SecondAssetId);

	FMocaraTargetProfile MissingProfile;
	FString MissingError;
	const FSoftObjectPath NonMeshPath(
		TEXT("/Engine/EngineMaterials/DefaultMaterial.DefaultMaterial"));
	USkeletalMesh* MissingMesh = FMocaraRetargeter::ResolveTargetMesh(
		NonMeshPath, MissingProfile, MissingError);
	TestNull(TEXT("A non-mesh explicit target does not silently fall back to Manny"), MissingMesh);
	TestTrue(TEXT("An invalid explicit target reports its path"),
		MissingError.Contains(NonMeshPath.ToString()));
	return true;
}

#endif
