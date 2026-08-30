#include "MocaraKimodoClient.h"
#include "MocaraSettings.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "HttpManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"

namespace
{
	/**
	 * Completion state for a blocking request. Held by TSharedRef so the HTTP
	 * callback stays valid even if WaitForRequest returns first -- capturing the
	 * caller's stack by reference here was corrupting memory whenever a request
	 * outlived the call (failed flush, timeout, editor shutdown).
	 */
	struct FMocaraHttpResult
	{
		bool bDone = false;
		bool bFailed = false;
		int32 Code = 0;
		FString Body;
		FString Error;
	};

	/** Default ceiling so a hung sidecar cannot block the game thread forever in Flush(). */
	constexpr float MocaraDefaultTimeoutSeconds = 10.0f;
}

static bool WaitForRequest(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Request, FString& OutBody, int32& OutCode, FString& OutError)
{
	if (!FMocaraKimodoClient::IsHttpAvailable())
	{
		OutError = TEXT("HTTP request failed. HTTP module is unavailable.");
		return false;
	}

	const TSharedRef<FMocaraHttpResult> Result = MakeShared<FMocaraHttpResult>();
	Request->OnProcessRequestComplete().BindLambda([Result](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
	{
		Result->bDone = true;
		if (!bSucceeded || !Response.IsValid())
		{
			Result->bFailed = true;
			Result->Error = TEXT("HTTP request failed. Is the Mocara sidecar running?");
			Result->Code = 0;
			return;
		}
		Result->Code = Response->GetResponseCode();
		Result->Body = Response->GetContentAsString();
	});

	if (!Request->GetTimeout().IsSet())
	{
		Request->SetTimeout(MocaraDefaultTimeoutSeconds);
	}

	if (!Request->ProcessRequest())
	{
		OutError = TEXT("Unable to start HTTP request.");
		return false;
	}

	FHttpModule::Get().GetHttpManager().Flush(EHttpFlushReason::FullFlush);

	if (!Result->bDone)
	{
		// Never leave a request in flight behind us.
		Request->CancelRequest();
		OutError = TEXT("HTTP request timed out.");
		return false;
	}

	OutBody = Result->Body;
	OutCode = Result->Code;
	OutError = Result->Error;
	return !Result->bFailed;
}

namespace
{
	/** Last known-good sidecar root. Read on the shutdown path when UObjects are gone. */
	FString GCachedSidecarUrl = TEXT("http://127.0.0.1:8765");
}

bool FMocaraKimodoClient::IsUObjectAccessSafe()
{
	return UObjectInitialized() && !GExitPurge && !IsEngineExitRequested();
}

bool FMocaraKimodoClient::IsHttpAvailable()
{
	return FModuleManager::Get().IsModuleLoaded(TEXT("HTTP"));
}

void FMocaraKimodoClient::RefreshCachedSettings()
{
	if (!IsUObjectAccessSafe())
	{
		return;
	}
	if (const UMocaraSettings* Settings = GetDefault<UMocaraSettings>())
	{
		if (!Settings->SidecarUrl.IsEmpty())
		{
			GCachedSidecarUrl = Settings->SidecarUrl;
		}
	}
}

FString FMocaraKimodoClient::MakeUrl(const FString& Path) const
{
	// Refreshes while the editor is alive; a no-op during teardown. Dereferencing
	// GetDefault<UMocaraSettings>() unconditionally here was the ShutdownModule crash.
	RefreshCachedSettings();

	FString Root = GCachedSidecarUrl;
	while (Root.EndsWith(TEXT("/")))
	{
		Root.LeftChopInline(1);
	}
	return Root + Path;
}

TSharedRef<IHttpRequest, ESPMode::ThreadSafe> FMocaraKimodoClient::MakeRequest(const TCHAR* Verb, const FString& Path) const
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetVerb(Verb);
	Request->SetURL(MakeUrl(Path));
	// The sidecar rejects requests without this. The value is not a secret; requiring a
	// custom header is what stops a web page from reaching the sidecar, because browsers
	// cannot set one cross-origin without a preflight the sidecar never answers.
	Request->SetHeader(TEXT("X-Mocara-Client"), TEXT("unreal"));
	return Request;
}

bool FMocaraKimodoClient::RequestShutdown() const
{
	if (!IsHttpAvailable())
	{
		return false;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("POST"), TEXT("/shutdown"));
	Request->SetTimeout(1.5f);
	FString Body;
	int32 Code = 0;
	FString Error;
	WaitForRequest(Request, Body, Code, Error);
	return Code == 200 || Error.Contains(TEXT("HTTP request failed"));
}

void FMocaraKimodoClient::StartGenerateAsync(const FMocaraGenerateRequest& RequestData, const TArray<TSharedPtr<FJsonValue>>* Constraints, FGenerateComplete OnComplete) const
{
	const TSharedRef<bool> bReported = MakeShared<bool>(false);
	auto Report = [bReported, OnComplete](bool bOk, const FString& JobId, const FString& Error)
	{
		if (*bReported)
		{
			return;
		}
		*bReported = true;
		OnComplete(bOk, JobId, Error);
	};

	if (!IsHttpAvailable())
	{
		Report(false, FString(), TEXT("HTTP module is unavailable."));
		return;
	}

	const TSharedRef<FJsonObject> Payload = BuildGeneratePayload(RequestData, Constraints);

	FString Body;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Body);
	FJsonSerializer::Serialize(Payload, Writer);

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("POST"), TEXT("/generate"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(Body);
	Request->SetTimeout(15.0f);
	Request->OnProcessRequestComplete().BindLambda(
		[Report](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
		{
			if (!bSucceeded || !Response.IsValid())
			{
				Report(false, FString(), TEXT("HTTP request failed. Is the Mocara sidecar running?"));
				return;
			}

			const int32 Code = Response->GetResponseCode();
			const FString ResponseBody = Response->GetContentAsString();

			TSharedPtr<FJsonObject> Json;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
			FString JobId;
			if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
			{
				Json->TryGetStringField(TEXT("job_id"), JobId);
			}
			if (JobId.IsEmpty())
			{
				Report(false, FString(), FString::Printf(TEXT("Generate failed (%d): %s"), Code, *ResponseBody));
				return;
			}
			Report(true, JobId, FString());
		});

	if (!Request->ProcessRequest())
	{
		Report(false, FString(), TEXT("Unable to start HTTP request."));
	}
}

TSharedRef<FJsonObject> FMocaraKimodoClient::BuildGeneratePayload(
	const FMocaraGenerateRequest& RequestData,
	const TArray<TSharedPtr<FJsonValue>>* Constraints)
{
	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	const bool bHasTimeline = RequestData.PromptSegments.Num() > 1;
	const FString PrimaryPrompt = bHasTimeline
		? RequestData.PromptSegments[0].Prompt
		: RequestData.Prompt;
	float TotalDuration = RequestData.DurationSeconds;
	if (bHasTimeline)
	{
		TotalDuration = 0.f;
		TArray<TSharedPtr<FJsonValue>> SegmentValues;
		const int32 SegmentCount = FMath::Min(RequestData.PromptSegments.Num(), 16);
		SegmentValues.Reserve(SegmentCount);
		for (int32 Index = 0; Index < SegmentCount; ++Index)
		{
			const FMocaraPromptSegment& Segment = RequestData.PromptSegments[Index];
			const float SegmentDuration = FMath::Clamp(Segment.DurationSeconds, 0.5f, 30.f);
			TotalDuration += SegmentDuration;
			TSharedRef<FJsonObject> SegmentJson = MakeShared<FJsonObject>();
			SegmentJson->SetStringField(TEXT("prompt"), Segment.Prompt);
			SegmentJson->SetNumberField(TEXT("duration"), SegmentDuration);
			SegmentValues.Add(MakeShared<FJsonValueObject>(SegmentJson));
		}
		Payload->SetArrayField(TEXT("segments"), MoveTemp(SegmentValues));
		Payload->SetNumberField(TEXT("transition_frames"), FMath::Clamp(RequestData.TransitionFrames, 1, 15));
	}
	Payload->SetStringField(TEXT("prompt"), PrimaryPrompt);
	Payload->SetNumberField(TEXT("duration"), FMath::Clamp(TotalDuration, 0.5f, 30.f));
	Payload->SetNumberField(TEXT("diffusion_steps"), FMath::Clamp(RequestData.DiffusionSteps, 1, 500));
	Payload->SetNumberField(TEXT("text_guidance"), FMath::Clamp(RequestData.TextGuidance, 0.f, 10.f));
	Payload->SetNumberField(TEXT("constraint_guidance"), FMath::Clamp(RequestData.ConstraintGuidance, 0.f, 10.f));
	Payload->SetNumberField(TEXT("candidate_count"), FMath::Clamp(RequestData.CandidateCount, 1, 4));
	Payload->SetBoolField(TEXT("in_place"), RequestData.bInPlace);
	Payload->SetBoolField(TEXT("bvh_standard_tpose"), true);
	if (RequestData.bUseSeed)
	{
		Payload->SetNumberField(TEXT("seed"), FMath::Max(0, RequestData.Seed));
	}
	if (!RequestData.ConstraintPreset.IsEmpty())
	{
		Payload->SetStringField(TEXT("constraint_preset"), RequestData.ConstraintPreset);
	}
	if (Constraints)
	{
		// Serialized by the caller before its stack unwinds, so the source array does not
		// need to outlive StartGenerateAsync.
		Payload->SetArrayField(TEXT("constraints"), *Constraints);
	}
	return Payload;
}

bool FMocaraKimodoClient::ParseJobState(
	const FString& Body,
	const FString& JobId,
	FMocaraJobState& OutState,
	FString& OutError)
{
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		OutError = Body;
		return false;
	}

	OutState.JobId = JobId;
	if (!Json->TryGetStringField(TEXT("status"), OutState.Status))
	{
		OutError = TEXT("Job response has no status field.");
		return false;
	}
	Json->TryGetStringField(TEXT("error"), OutState.Error);
	Json->TryGetStringField(TEXT("windows_bvh_path"), OutState.BvhPath);
	if (OutState.BvhPath.IsEmpty())
	{
		Json->TryGetStringField(TEXT("bvh_path"), OutState.BvhPath);
	}
	Json->TryGetStringField(TEXT("windows_npz_path"), OutState.NpzPath);
	if (Json->HasTypedField<EJson::Number>(TEXT("fps")))
	{
		OutState.Fps = Json->GetNumberField(TEXT("fps"));
	}
	if (Json->HasTypedField<EJson::Number>(TEXT("num_frames")))
	{
		OutState.NumFrames = static_cast<int32>(Json->GetNumberField(TEXT("num_frames")));
	}
	if (Json->HasTypedField<EJson::Number>(TEXT("completed_candidates")))
	{
		OutState.CompletedCandidates = static_cast<int32>(Json->GetNumberField(TEXT("completed_candidates")));
	}
	Json->TryGetStringField(TEXT("windows_provenance_path"), OutState.ProvenancePath);
	if (OutState.ProvenancePath.IsEmpty())
	{
		Json->TryGetStringField(TEXT("provenance_path"), OutState.ProvenancePath);
	}
	Json->TryGetStringField(TEXT("created_at"), OutState.CreatedAt);

	const TArray<TSharedPtr<FJsonValue>>* ArtifactValues = nullptr;
	if (Json->TryGetArrayField(TEXT("artifacts"), ArtifactValues) && ArtifactValues)
	{
		for (const TSharedPtr<FJsonValue>& Value : *ArtifactValues)
		{
			const TSharedPtr<FJsonObject> ArtifactJson = Value.IsValid() ? Value->AsObject() : nullptr;
			if (!ArtifactJson.IsValid())
			{
				continue;
			}
			FMocaraJobArtifact Artifact;
			if (ArtifactJson->HasTypedField<EJson::Number>(TEXT("candidate_index")))
			{
				Artifact.CandidateIndex = static_cast<int32>(ArtifactJson->GetNumberField(TEXT("candidate_index")));
			}
			if (ArtifactJson->HasTypedField<EJson::Number>(TEXT("seed")))
			{
				Artifact.Seed = static_cast<int32>(ArtifactJson->GetNumberField(TEXT("seed")));
				Artifact.bHasSeed = true;
			}
			ArtifactJson->TryGetStringField(TEXT("windows_bvh_path"), Artifact.BvhPath);
			if (Artifact.BvhPath.IsEmpty())
			{
				ArtifactJson->TryGetStringField(TEXT("bvh_path"), Artifact.BvhPath);
			}
			ArtifactJson->TryGetStringField(TEXT("windows_npz_path"), Artifact.NpzPath);
			if (Artifact.NpzPath.IsEmpty())
			{
				ArtifactJson->TryGetStringField(TEXT("npz_path"), Artifact.NpzPath);
			}
			if (!Artifact.BvhPath.IsEmpty())
			{
				OutState.Artifacts.Add(MoveTemp(Artifact));
			}
		}
	}
	if (OutState.Artifacts.IsEmpty() && !OutState.BvhPath.IsEmpty())
	{
		FMocaraJobArtifact Artifact;
		Artifact.BvhPath = OutState.BvhPath;
		Artifact.NpzPath = OutState.NpzPath;
		OutState.Artifacts.Add(MoveTemp(Artifact));
	}

	const TSharedPtr<FJsonObject>* Provenance = nullptr;
	if (Json->TryGetObjectField(TEXT("provenance"), Provenance) && Provenance && Provenance->IsValid())
	{
		(*Provenance)->TryGetStringField(TEXT("prompt"), OutState.Prompt);
		(*Provenance)->TryGetStringField(TEXT("model"), OutState.Model);
		(*Provenance)->TryGetStringField(TEXT("constraint_preset"), OutState.ConstraintPreset);
		(*Provenance)->TryGetStringField(TEXT("text_encoder_precision"), OutState.TextEncoderPrecision);
		(*Provenance)->TryGetStringField(TEXT("backend"), OutState.Backend);
		if ((*Provenance)->HasTypedField<EJson::Number>(TEXT("duration")))
		{
			OutState.DurationSeconds = static_cast<float>((*Provenance)->GetNumberField(TEXT("duration")));
		}
		if ((*Provenance)->HasTypedField<EJson::Number>(TEXT("transition_frames")))
		{
			OutState.TransitionFrames = FMath::Clamp(
				static_cast<int32>((*Provenance)->GetNumberField(TEXT("transition_frames"))), 1, 15);
		}
		const TArray<TSharedPtr<FJsonValue>>* SegmentValues = nullptr;
		if ((*Provenance)->TryGetArrayField(TEXT("segments"), SegmentValues)
			&& SegmentValues && SegmentValues->Num() <= 16)
		{
			for (const TSharedPtr<FJsonValue>& SegmentValue : *SegmentValues)
			{
				const TSharedPtr<FJsonObject> SegmentJson = SegmentValue.IsValid()
					? SegmentValue->AsObject() : nullptr;
				if (!SegmentJson.IsValid())
				{
					continue;
				}
				FMocaraPromptSegment Segment;
				if (SegmentJson->TryGetStringField(TEXT("prompt"), Segment.Prompt)
					&& SegmentJson->HasTypedField<EJson::Number>(TEXT("duration")))
				{
					Segment.DurationSeconds = FMath::Clamp(
						static_cast<float>(SegmentJson->GetNumberField(TEXT("duration"))), 0.5f, 30.f);
					OutState.PromptSegments.Add(MoveTemp(Segment));
				}
			}
		}
		const TSharedPtr<FJsonObject>* ModelBundle = nullptr;
		if ((*Provenance)->TryGetObjectField(TEXT("model_bundle"), ModelBundle)
			&& ModelBundle && ModelBundle->IsValid())
		{
			(*ModelBundle)->TryGetStringField(TEXT("bundle_sha256"), OutState.ModelBundleSha256);
		}
		if ((*Provenance)->HasTypedField<EJson::Number>(TEXT("seed")))
		{
			OutState.Seed = static_cast<int32>((*Provenance)->GetNumberField(TEXT("seed")));
			OutState.bHasSeed = true;
		}
		if ((*Provenance)->HasTypedField<EJson::Number>(TEXT("text_guidance")))
		{
			OutState.TextGuidance = static_cast<float>((*Provenance)->GetNumberField(TEXT("text_guidance")));
		}
		if ((*Provenance)->HasTypedField<EJson::Number>(TEXT("constraint_guidance")))
		{
			OutState.ConstraintGuidance = static_cast<float>((*Provenance)->GetNumberField(TEXT("constraint_guidance")));
		}
		if ((*Provenance)->HasTypedField<EJson::Number>(TEXT("candidate_count")))
		{
			OutState.CandidateCount = static_cast<int32>((*Provenance)->GetNumberField(TEXT("candidate_count")));
		}
	}
	return true;
}

bool FMocaraKimodoClient::ParseHistoryResponse(
	const FString& Body,
	TArray<FMocaraJobState>& OutJobs,
	FString& OutError)
{
	OutJobs.Reset();
	if (Body.Len() > 2 * 1024 * 1024)
	{
		OutError = TEXT("History response exceeds the 2 MiB client limit.");
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		OutError = TEXT("History response is not valid JSON.");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* JobValues = nullptr;
	if (!Root->TryGetArrayField(TEXT("jobs"), JobValues) || !JobValues || JobValues->Num() > 50)
	{
		OutError = TEXT("History response has an invalid jobs list.");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Value : *JobValues)
	{
		const TSharedPtr<FJsonObject> JobJson = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!JobJson.IsValid())
		{
			continue;
		}
		FString JobId;
		if (!JobJson->TryGetStringField(TEXT("job_id"), JobId) || JobId.IsEmpty())
		{
			continue;
		}
		FString JobBody;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JobBody);
		FJsonSerializer::Serialize(JobJson.ToSharedRef(), Writer);
		FMocaraJobState State;
		FString ParseError;
		if (ParseJobState(JobBody, JobId, State, ParseError) && State.Status == TEXT("done"))
		{
			OutJobs.Add(MoveTemp(State));
		}
	}
	return true;
}

void FMocaraKimodoClient::QueryJobAsync(const FString& JobId, FJobQueryComplete OnComplete) const
{
	// ProcessRequest() can fail without ever firing the completion delegate, and on some
	// paths fires it first. Report through this so the caller hears back exactly once --
	// a missed callback would strand the in-flight flag and stop polling for good.
	const TSharedRef<bool> bReported = MakeShared<bool>(false);
	auto Report = [bReported, OnComplete](bool bOk, const FMocaraJobState& State, const FString& Error)
	{
		if (*bReported)
		{
			return;
		}
		*bReported = true;
		OnComplete(bOk, State, Error);
	};

	if (!IsHttpAvailable())
	{
		Report(false, FMocaraJobState(), TEXT("HTTP module is unavailable."));
		return;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(TEXT("GET"), FString::Printf(TEXT("/jobs/%s"), *JobId));
	Request->SetTimeout(5.0f);
	Request->OnProcessRequestComplete().BindLambda(
		[JobId, Report](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
		{
			FMocaraJobState State;
			if (!bSucceeded || !Response.IsValid())
			{
				Report(false, State, TEXT("HTTP request failed. Is the Mocara sidecar running?"));
				return;
			}
			FString Error;
			const bool bParsed = FMocaraKimodoClient::ParseJobState(Response->GetContentAsString(), JobId, State, Error);
			Report(bParsed, State, Error);
		});

	if (!Request->ProcessRequest())
	{
		Report(false, FMocaraJobState(), TEXT("Unable to start HTTP request."));
	}
}

void FMocaraKimodoClient::QueryHistoryAsync(int32 Limit, FHistoryQueryComplete OnComplete) const
{
	const TSharedRef<bool> bReported = MakeShared<bool>(false);
	auto Report = [bReported, OnComplete](bool bOk, const TArray<FMocaraJobState>& Jobs, const FString& Error)
	{
		if (*bReported)
		{
			return;
		}
		*bReported = true;
		OnComplete(bOk, Jobs, Error);
	};
	if (!IsHttpAvailable())
	{
		Report(false, TArray<FMocaraJobState>(), TEXT("HTTP module is unavailable."));
		return;
	}
	const int32 SafeLimit = FMath::Clamp(Limit, 1, 50);
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = MakeRequest(
		TEXT("GET"), FString::Printf(TEXT("/history?limit=%d"), SafeLimit));
	Request->SetTimeout(5.0f);
	Request->OnProcessRequestComplete().BindLambda(
		[Report](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
		{
			TArray<FMocaraJobState> Jobs;
			if (!bSucceeded || !Response.IsValid())
			{
				Report(false, Jobs, TEXT("HTTP request failed. Is the Mocara sidecar running?"));
				return;
			}
			if (Response->GetResponseCode() != 200)
			{
				Report(false, Jobs, FString::Printf(
					TEXT("History request failed (%d): %s"),
					Response->GetResponseCode(),
					*Response->GetContentAsString()));
				return;
			}
			FString Error;
			const bool bParsed = FMocaraKimodoClient::ParseHistoryResponse(
				Response->GetContentAsString(), Jobs, Error);
			Report(bParsed, Jobs, Error);
		});
	if (!Request->ProcessRequest())
	{
		Report(false, TArray<FMocaraJobState>(), TEXT("Unable to start HTTP request."));
	}
}
