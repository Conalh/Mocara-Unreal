#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "MocaraKimodoClient.h"
#include "Dom/JsonObject.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraGenerateContractTest,
	"Mocara.Generation.Contract.ReproducibleControls",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraGenerateContractTest::RunTest(const FString& Parameters)
{
	FMocaraGenerateRequest Request;
	Request.Prompt = TEXT("A person swings a two-handed sword.");
	Request.DurationSeconds = 0.1f;
	Request.Seed = 48271;
	Request.bUseSeed = true;
	Request.TextGuidance = 2.5f;
	Request.ConstraintGuidance = 3.f;
	Request.CandidateCount = 3;
	Request.ConstraintPreset = TEXT("two-handed-grip");

	const TSharedRef<FJsonObject> Payload = FMocaraKimodoClient::BuildGeneratePayload(Request, nullptr);
	TestEqual(TEXT("Prompt is sent exactly"), Payload->GetStringField(TEXT("prompt")), Request.Prompt);
	TestEqual(TEXT("Duration is clamped to Kimodo's stable minimum"),
		static_cast<float>(Payload->GetNumberField(TEXT("duration"))), 0.5f);
	TestEqual(TEXT("Seed is sent when enabled"),
		static_cast<int32>(Payload->GetNumberField(TEXT("seed"))), Request.Seed);
	TestEqual(TEXT("Text guidance is sent"),
		static_cast<float>(Payload->GetNumberField(TEXT("text_guidance"))), Request.TextGuidance);
	TestEqual(TEXT("Constraint guidance is sent"),
		static_cast<float>(Payload->GetNumberField(TEXT("constraint_guidance"))), Request.ConstraintGuidance);
	TestEqual(TEXT("Candidate count is sent"),
		static_cast<int32>(Payload->GetNumberField(TEXT("candidate_count"))), Request.CandidateCount);
	TestEqual(TEXT("Constraint preset is identified for provenance"),
		Payload->GetStringField(TEXT("constraint_preset")), Request.ConstraintPreset);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraJobContractTest,
	"Mocara.Generation.Contract.CandidatesAndProvenance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraJobContractTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT(R"({
		"status":"done",
		"windows_bvh_path":"C:/Saved/mocara_job_c01.bvh",
		"windows_npz_path":"C:/Saved/mocara_job_c01.npz",
		"windows_provenance_path":"C:/Saved/mocara_job.json",
		"fps":30.0,
		"num_frames":90,
		"completed_candidates":2,
		"artifacts":[
			{"candidate_index":0,"seed":99,"bvh_path":"/mnt/c/Saved/c01.bvh","npz_path":"/mnt/c/Saved/c01.npz","windows_bvh_path":"C:/Saved/c01.bvh","windows_npz_path":"C:/Saved/c01.npz"},
			{"candidate_index":1,"seed":100,"bvh_path":"/mnt/c/Saved/c02.bvh","npz_path":"/mnt/c/Saved/c02.npz","windows_bvh_path":"C:/Saved/c02.bvh","windows_npz_path":"C:/Saved/c02.npz"}
		],
		"provenance":{
			"prompt":"A person swings a two-handed sword.",
			"duration":3.0,
			"seed":99,
			"model":"kimodo-soma-rp-v1.1",
			"diffusion_steps":100,
			"cfg_type":"separated",
			"text_guidance":2.5,
			"constraint_guidance":3.0,
			"candidate_count":2,
			"constraint_preset":"two-handed-grip",
			"constraint_count":1,
			"text_encoder_precision":"float32"
		}
	})");

	FMocaraJobState State;
	FString Error;
	TestTrue(TEXT("Job response parses"), FMocaraKimodoClient::ParseJobState(Body, TEXT("job"), State, Error));
	TestEqual(TEXT("All candidates are retained"), State.Artifacts.Num(), 2);
	TestEqual(TEXT("Candidate progress is retained"), State.CompletedCandidates, 2);
	TestEqual(TEXT("First candidate keeps its Windows path"), State.Artifacts[0].BvhPath, FString(TEXT("C:/Saved/c01.bvh")));
	TestEqual(TEXT("Second candidate keeps its index"), State.Artifacts[1].CandidateIndex, 1);
	TestEqual(TEXT("Each candidate keeps its reproducible seed"), State.Artifacts[1].Seed, 100);
	TestEqual(TEXT("Prompt provenance is retained"), State.Prompt, FString(TEXT("A person swings a two-handed sword.")));
	TestTrue(TEXT("Seed provenance is retained"), State.bHasSeed);
	TestEqual(TEXT("Seed provenance is exact"), State.Seed, 99);
	TestEqual(TEXT("FP32 provenance is visible"), State.TextEncoderPrecision, FString(TEXT("float32")));
	TestEqual(TEXT("Provenance path is retained"), State.ProvenancePath, FString(TEXT("C:/Saved/mocara_job.json")));
	return true;
}

#endif
