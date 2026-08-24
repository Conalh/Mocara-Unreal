#pragma once

#include "CoreMinimal.h"
#include "Interfaces/IHttpRequest.h"
#include "MocaraTypes.h"

class FMocaraKimodoClient
{
public:
	/**
	 * Called when an async request resolves. Always invoked on the game thread, exactly
	 * once, whether the request succeeded, failed, or never started.
	 */
	using FJobQueryComplete = TFunction<void(bool /*bOk*/, const FMocaraJobState& /*State*/, const FString& /*Error*/)>;
	using FGenerateComplete = TFunction<void(bool /*bOk*/, const FString& /*JobId*/, const FString& /*Error*/)>;

	/**
	 * Build a request aimed at Path with the client header the sidecar requires.
	 * Every call into the sidecar must go through here so the header cannot be forgotten.
	 */
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> MakeRequest(const TCHAR* Verb, const FString& Path) const;

	/** Serialize the additive /generate contract. Shared with automation tests. */
	static TSharedRef<FJsonObject> BuildGeneratePayload(
		const FMocaraGenerateRequest& Request,
		const TArray<TSharedPtr<FJsonValue>>* Constraints);

	/** Decode one /jobs/{id} response, including candidate paths and provenance. */
	static bool ParseJobState(
		const FString& Body,
		const FString& JobId,
		FMocaraJobState& OutState,
		FString& OutError);

	/**
	 * Blocking, and deliberately so: this runs on the editor teardown path where there is
	 * no later tick to deliver a callback. It is the only blocking call left in this
	 * client -- see QueryJobAsync for why the others were converted.
	 */
	bool RequestShutdown() const;

	/** Submit a generation without blocking. Constraints are copied before returning. */
	void StartGenerateAsync(const FMocaraGenerateRequest& Request, const TArray<TSharedPtr<FJsonValue>>* Constraints, FGenerateComplete OnComplete) const;

	/**
	 * Poll a job without blocking. The blocking variant this replaced ran a full HTTP
	 * flush on the game thread every 0.4s for the life of a generation, which stalls the
	 * whole editor -- Flush() waits on every in-flight request in the process, not just
	 * this one. Callers must tolerate a response arriving after the job was abandoned.
	 */
	void QueryJobAsync(const FString& JobId, FJobQueryComplete OnComplete) const;

	FString MakeUrl(const FString& Path) const;

	/**
	 * Re-read the sidecar URL from UMocaraSettings into the process-wide cache.
	 * Silently does nothing once the UObject system is being torn down, so callers
	 * on the shutdown path keep working against the last known-good URL.
	 */
	static void RefreshCachedSettings();

	/** True while it is safe to touch UObjects (CDOs, settings) from this thread. */
	static bool IsUObjectAccessSafe();

	/** True while the HTTP module is loaded and usable. */
	static bool IsHttpAvailable();
};
