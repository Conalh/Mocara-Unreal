#pragma once

#include "CoreMinimal.h"
#include "EditorUndoClient.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/SCompoundWidget.h"
#include "MocaraTypes.h"
#include "MocaraKimodoClient.h"
#include "Dom/JsonValue.h"

class SEditableTextBox;
class SMocaraViewport;
class SButton;
class UAnimSequence;
class USkeletalMesh;

class SMocaraWindow : public SCompoundWidget, public FEditorUndoClient
{
public:
	SLATE_BEGIN_ARGS(SMocaraWindow) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMocaraWindow() override;

	/** Import a BVH from disk, retarget it, and show it in the preview. */
	bool LoadClipFromFile(const FString& BvhPath);
	void SetPlayhead(int32 Frame);

	/** Export the newest retargeted clip (or the SOMA clip) to FBX. */
	bool ExportLatestToFbx(const FString& Filename, FString& OutError);

	/** The most recently constructed Mocara tab, if one is open. */
	static TWeakPtr<SMocaraWindow> GetLiveWindow();
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual bool MatchesContext(
		const FTransactionContext& InContext,
		const TArray<TPair<UObject*, FTransactionObjectEvent>>& TransactionObjectContexts) const override;
	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override;

private:
	FReply OnGenerate();
	FReply OnApplyAutoPose();
	FReply OnRegenerateWithConstraints();
	FReply OnRegenerateWithTwoHandGrip();
	FReply OnLoadCandidate();
	FReply OnKeyPose();
	FReply OnExportFbx();
	FReply OnDeleteSelectedKey();
	FReply OnCopySelectedKey();
	FReply OnPasteKey();
	FReply OnDuplicateSelectedKey();
	FReply OnTogglePlay();
	FReply OnStop();
	EActiveTimerReturnType OnPoll(double CurrentTime, float DeltaTime);
	/** Apply one async /jobs reply. Called on the game thread; may be stale, see impl. */
	void HandleJobState(bool bOk, const FMocaraJobState& State, const FString& Error);
	EActiveTimerReturnType OnPlaybackTick(double CurrentTime, float DeltaTime);
	void SetStatus(const FString& Text);
	FText PoseSummaryText() const;
	FReply SubmitGenerate(const TArray<TSharedPtr<FJsonValue>>* Constraints);
	bool ImportAndRetarget(const FMocaraJobState& Job);
	FMocaraJobState SelectCandidate(const FMocaraJobState& Job, int32 CandidateIndex) const;
	void SelectBone(FName Bone);
	void OnViewportTransformChanged(FRotator Rotation, FVector Translation);
	void OnViewportManipulationStarted();
	void OnViewportManipulationEnded();
	void BeginPoseControlTransaction();
	void EndPoseControlTransaction();
	void OnPoseControlsChanged();
	void OnTimelineKeyMoveStarted();
	int32 OnTimelineKeyMoved(int32 KeyIndex, int32 NewFrame);
	int32 OnTimelineKeyResized(
		int32 KeyIndex,
		EMocaraPoseIntervalHandle Handle,
		int32 NewFrame);
	void OnTimelineKeyMoveEnded();
	void RefreshSelectionFromKeys();
	TArray<FMocaraPoseKey>& GetPoseKeys();
	const TArray<FMocaraPoseKey>& GetPoseKeys() const;

	void SelectKey(int32 Index);
	TSharedRef<SWidget> MakeBoneButton(FName Bone);
	TSharedRef<SWidget> MakeBoneRow(const TArray<FName>& Bones);
	FLinearColor BoneButtonTint(FName Bone) const;
	int32 TimelineFrameCount() const;

	FMocaraKimodoClient Client;
	FMocaraGenerateRequest Request;
	FMocaraJobState ActiveJob;
	TStrongObjectPtr<UMocaraPoseEditState> PoseEditState;
	TArray<TSharedPtr<FJsonValue>> PendingConstraints;
	bool bHasPendingConstraints = false;
	FName SelectedBone = TEXT("Hips");
	FRotator PoseRotation = FRotator::ZeroRotator;
	FVector PoseTranslation = FVector::ZeroVector;
	int32 CurrentFrame = 0;
	int32 SelectedKeyIndex = INDEX_NONE;
	int32 TimelineFrames = 90;
	int32 SelectedCandidateIndex = 0;
	float TimelineFps = 30.f;
	int32 EaseInFrames = 8;
	int32 HoldFrames = 1;
	int32 EaseOutFrames = 8;
	bool bUseTwoBoneIK = false;
	bool bPolling = false;
	bool bJobQueryInFlight = false;
	bool bSubmitInFlight = false;
	bool bImportPending = false;
	bool bWaitingForSidecar = false;
	bool bPlaying = false;
	bool bViewportTransactionActive = false;
	bool bPoseControlTransactionActive = false;
	bool bTimelineTransactionActive = false;
	TOptional<FMocaraPoseKey> CopiedPoseKey;
	FString StatusText;
	TWeakObjectPtr<UAnimSequence> LastSomaSequence;
	TWeakObjectPtr<UAnimSequence> LastTargetSequence;
	TWeakObjectPtr<USkeletalMesh> LastSomaMesh;
	TSharedPtr<SEditableTextBox> PromptBox;
public:
	TSharedPtr<SMocaraViewport> Viewport;
private:
	TWeakObjectPtr<USkeletalMesh> LastTargetMesh;
};
