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
	TestFalse(TEXT("Legacy request stays a single prompt"), Payload->HasField(TEXT("segments")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraMultiPromptContractTest,
	"Mocara.Generation.Contract.MultiPromptTimeline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraMultiPromptContractTest::RunTest(const FString& Parameters)
{
	FMocaraGenerateRequest Request;
	Request.Prompt = TEXT("runs forward.");
	Request.TransitionFrames = 7;
	Request.PromptSegments = {
		{TEXT("runs forward."), 1.5f},
		{TEXT("vaults a rail, landing low."), 2.0f},
		{TEXT("recovers into a sprint."), 1.0f},
	};

	const TSharedRef<FJsonObject> Payload = FMocaraKimodoClient::BuildGeneratePayload(Request, nullptr);
	const TArray<TSharedPtr<FJsonValue>>& Segments = Payload->GetArrayField(TEXT("segments"));
	TestEqual(TEXT("Every prompt segment is serialized"), Segments.Num(), 3);
	TestEqual(TEXT("First prompt remains the compatibility prompt"),
		Payload->GetStringField(TEXT("prompt")), Request.Prompt);
	TestEqual(TEXT("Total duration is derived from the timeline"),
		static_cast<float>(Payload->GetNumberField(TEXT("duration"))), 4.5f);
	TestEqual(TEXT("Transition overlap is serialized"),
		static_cast<int32>(Payload->GetNumberField(TEXT("transition_frames"))), 7);
	TestEqual(TEXT("Segment punctuation is preserved"),
		Segments[1]->AsObject()->GetStringField(TEXT("prompt")), Request.PromptSegments[1].Prompt);
	TestEqual(TEXT("Per-segment duration is preserved"),
		static_cast<float>(Segments[2]->AsObject()->GetNumberField(TEXT("duration"))), 1.0f);
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
			"text_encoder_precision":"float32",
			"backend":"nvidia-kimodo-python",
			"segments":[
				{"prompt":"A person swings a two-handed sword.","duration":1.5},
				{"prompt":"The person recovers to guard.","duration":1.5}
			],
			"transition_frames":7,
			"model_bundle":{"status":"verified","bundle_sha256":"c8765f326f2a7fcb087b48704d82c94d0e0cbaff6a264e90f6a73b102db7ba13"}
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
	TestEqual(TEXT("Backend provenance is retained"), State.Backend, FString(TEXT("nvidia-kimodo-python")));
	TestEqual(TEXT("Verified bundle hash is retained"), State.ModelBundleSha256,
		FString(TEXT("c8765f326f2a7fcb087b48704d82c94d0e0cbaff6a264e90f6a73b102db7ba13")));
	TestEqual(TEXT("Prompt segments are retained"), State.PromptSegments.Num(), 2);
	TestEqual(TEXT("Segment punctuation is retained"), State.PromptSegments[1].Prompt,
		FString(TEXT("The person recovers to guard.")));
	TestEqual(TEXT("Transition overlap is retained"), State.TransitionFrames, 7);
	TestEqual(TEXT("Provenance path is retained"), State.ProvenancePath, FString(TEXT("C:/Saved/mocara_job.json")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FMocaraHistoryContractTest,
	"Mocara.Generation.Contract.PersistentHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMocaraHistoryContractTest::RunTest(const FString& Parameters)
{
	const FString Body = TEXT(R"({
		"jobs":[{
			"job_id":"222222222222",
			"status":"done",
			"created_at":"2026-08-30T11:00:00Z",
			"windows_bvh_path":"C:/Saved/mocara_222222222222_c01.bvh",
			"windows_npz_path":"C:/Saved/mocara_222222222222_c01.npz",
			"fps":30.0,
			"num_frames":60,
			"completed_candidates":1,
			"artifacts":[{
				"candidate_index":0,
				"seed":20,
				"bvh_path":"/mnt/c/Saved/mocara_222222222222_c01.bvh",
				"npz_path":"/mnt/c/Saved/mocara_222222222222_c01.npz",
				"windows_bvh_path":"C:/Saved/mocara_222222222222_c01.bvh",
				"windows_npz_path":"C:/Saved/mocara_222222222222_c01.npz"
			}],
			"provenance":{
				"prompt":"vault a rail",
				"duration":2.0,
				"seed":20,
				"model":"kimodo-soma-rp-v1.1",
				"diffusion_steps":100,
				"text_guidance":2.0,
				"constraint_guidance":2.0,
				"candidate_count":1,
				"constraint_count":0,
				"text_encoder_precision":"float32",
				"backend":"nvidia-kimodo-python",
				"in_place":false,
				"bvh_standard_tpose":true
			}
		}]
	})");

	TArray<FMocaraJobState> Jobs;
	FString Error;
	TestTrue(TEXT("History response parses"), FMocaraKimodoClient::ParseHistoryResponse(Body, Jobs, Error));
	TestEqual(TEXT("One history job is returned"), Jobs.Num(), 1);
	TestEqual(TEXT("History job id is retained"), Jobs[0].JobId, FString(TEXT("222222222222")));
	TestEqual(TEXT("History creation time is retained"), Jobs[0].CreatedAt, FString(TEXT("2026-08-30T11:00:00Z")));
	TestEqual(TEXT("History duration is retained"), Jobs[0].DurationSeconds, 2.0f);
	TestEqual(TEXT("History prompt is retained"), Jobs[0].Prompt, FString(TEXT("vault a rail")));
	TestEqual(TEXT("History artifact can be imported"), Jobs[0].BvhPath,
		FString(TEXT("C:/Saved/mocara_222222222222_c01.bvh")));
	return true;
}

#endif
