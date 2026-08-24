#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MocaraViewportMath.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraViewportFollowDiscontinuityTest,
	"Mocara.Viewport.Camera.FollowDiscontinuity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraViewportFollowDiscontinuityTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Ordinary per-frame travel keeps following"),
		UE::Mocara::Viewport::ShouldReanchorFollowCamera(FVector(12.f, 4.f, 0.f)));
	TestFalse(TEXT("The threshold itself remains a continuous camera step"),
		UE::Mocara::Viewport::ShouldReanchorFollowCamera(FVector(60.f, 0.f, 0.f)));
	TestTrue(TEXT("A loop or scrub jump reanchors instead of moving the camera"),
		UE::Mocara::Viewport::ShouldReanchorFollowCamera(FVector(60.01f, 0.f, 0.f)));
	return true;
}

#endif
