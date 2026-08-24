#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MocaraPreviewCharacterResolver.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "PreviewScene.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraMetaHumanPreviewCharacterTest,
	"Mocara.Viewport.MetaHuman.AssembledCharacter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraMetaHumanPreviewCharacterTest::RunTest(const FString& Parameters)
{
	USkeletalMesh* BodyMesh = LoadObject<USkeletalMesh>(
		nullptr,
		TEXT("/Game/MetaHumans/Common/Common/Mocap/m_med_nrw_body_mocap.m_med_nrw_body_mocap"));
	if (!BodyMesh)
	{
		AddWarning(TEXT("MetaHuman project content is not installed; skipping assembled-character assertions."));
		return true;
	}

	FName BodyComponentName;
	UClass* PreviewClass = FMocaraPreviewCharacterResolver::FindCompatibleClass(
		BodyMesh, nullptr, BodyComponentName);
	if (!TestNotNull(TEXT("A compatible assembled MetaHuman Blueprint is discovered"), PreviewClass))
	{
		return false;
	}
	TestTrue(TEXT("The discovered example is Kellan"),
		PreviewClass->GetPathName().Contains(TEXT("BP_Kellan")));
	TestTrue(TEXT("The assembled Blueprint exposes its body component"),
		BodyComponentName.ToString().Contains(TEXT("Body")));

	FPreviewScene PreviewScene{FPreviewScene::ConstructionValues()};
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* PreviewActor = PreviewScene.GetWorld()->SpawnActor<AActor>(
		PreviewClass, FTransform::Identity, SpawnParameters);
	if (!TestNotNull(TEXT("The assembled MetaHuman Blueprint can be spawned in a preview world"), PreviewActor))
	{
		return false;
	}
	TestNotNull(
		TEXT("The spawned example exposes a compatible live Body component"),
		FMocaraPreviewCharacterResolver::FindCompatibleBodyComponent(
			PreviewActor, BodyMesh, BodyComponentName));
	PreviewActor->Destroy();
	return true;
}

#endif
