#pragma once

#include "CoreMinimal.h"
#include "MocaraTypes.generated.h"

USTRUCT(BlueprintType)
struct FMocaraGenerateRequest
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category="Mocara")
	FString Prompt = TEXT("A person walks forward.");

	UPROPERTY(EditAnywhere, Category="Mocara")
	float DurationSeconds = 3.0f;

	UPROPERTY(EditAnywhere, Category="Mocara")
	int32 Seed = 1337;

	UPROPERTY(EditAnywhere, Category="Mocara")
	bool bUseSeed = true;

	/** Independent classifier-free guidance applied to the text condition. */
	UPROPERTY(EditAnywhere, Category="Mocara", meta=(ClampMin="0.0", ClampMax="10.0"))
	float TextGuidance = 2.0f;

	/** Independent classifier-free guidance applied to pose/end-effector constraints. */
	UPROPERTY(EditAnywhere, Category="Mocara", meta=(ClampMin="0.0", ClampMax="10.0"))
	float ConstraintGuidance = 2.0f;

	/** Generate this many deterministic alternatives sequentially at single-sample VRAM. */
	UPROPERTY(EditAnywhere, Category="Mocara", meta=(ClampMin="1", ClampMax="4"))
	int32 CandidateCount = 3;

	/** Optional preset identifier recorded with the job; constraints carry the behavior. */
	FString ConstraintPreset;

	UPROPERTY(EditAnywhere, Category="Mocara")
	bool bInPlace = false;

	UPROPERTY(EditAnywhere, Category="Mocara")
	int32 DiffusionSteps = 100;
};

USTRUCT()
struct FMocaraJobArtifact
{
	GENERATED_BODY()

	int32 CandidateIndex = 0;
	int32 Seed = 0;
	bool bHasSeed = false;
	FString BvhPath;
	FString NpzPath;
};

enum class EMocaraPoseIntervalHandle : uint8
{
	Move,
	EaseInStart,
	HoldEnd,
	EaseOutEnd,
};

USTRUCT(BlueprintType)
struct FMocaraPoseKey
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category="Mocara")
	int32 Frame = 0;

	UPROPERTY(EditAnywhere, Category="Mocara")
	FName BoneName;

	UPROPERTY(EditAnywhere, Category="Mocara")
	FRotator RotationOffset = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, Category="Mocara")
	FVector TranslationOffset = FVector::ZeroVector;

	/** Frames before Frame used to blend from the source animation into this pose. */
	UPROPERTY(EditAnywhere, Category="Mocara", meta=(ClampMin="0"))
	int32 EaseInFrames = 8;

	/** Full-strength frames beginning at Frame. */
	UPROPERTY(EditAnywhere, Category="Mocara", meta=(ClampMin="1"))
	int32 HoldFrames = 1;

	/** Frames after the hold used to blend back to the source animation. */
	UPROPERTY(EditAnywhere, Category="Mocara", meta=(ClampMin="0"))
	int32 EaseOutFrames = 8;

	UPROPERTY(EditAnywhere, Category="Mocara")
	bool bUseTwoBoneIK = false;
};

/** Transactional editor state so viewport pose edits participate in Unreal undo/redo. */
UCLASS(Transient)
class MOCARAEDITOR_API UMocaraPoseEditState : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TArray<FMocaraPoseKey> Keys;
};

USTRUCT()
struct FMocaraJobState
{
	GENERATED_BODY()

	FString JobId;
	FString Status;
	FString Error;
	FString BvhPath;
	FString NpzPath;
	TArray<FMocaraJobArtifact> Artifacts;
	int32 CandidateIndex = 0;
	float Fps = 30.0f;
	int32 NumFrames = 0;
	int32 CompletedCandidates = 0;
	FString ProvenancePath;
	FString Prompt;
	FString Model;
	FString ConstraintPreset;
	FString TextEncoderPrecision;
	int32 Seed = 0;
	bool bHasSeed = false;
	float TextGuidance = 2.0f;
	float ConstraintGuidance = 2.0f;
	int32 CandidateCount = 1;
};
