#pragma once

#include "CoreMinimal.h"
#include "MocaraTypes.h"
#include "SEditorViewport.h"
#include "UObject/GCObject.h"

class FAdvancedPreviewScene;
class FMocaraViewportClient;
class AActor;
class UAnimSequence;
class UDebugSkelMeshComponent;
class USkeletalMesh;
class USkeletalMeshComponent;

DECLARE_DELEGATE_OneParam(FOnMocaraBoneSelected, FName);
DECLARE_DELEGATE_TwoParams(FOnMocaraBoneTransformChanged, FRotator, FVector);

/**
 * Self-contained 3D preview for the Mocara tab: an advanced preview scene with a single
 * debug skeletal mesh component that plays back whatever clip was last generated.
 *
 * Deliberately not tied to an asset editor -- the Mocara tab is a nomad tab, so it owns
 * its own preview scene rather than borrowing one from a toolkit host.
 */
/** Canonical camera angles for judging a pose. */
enum class EMocaraViewPreset : uint8
{
	Perspective,
	Front,
	Side,
	Top,
};

class SMocaraViewport : public SEditorViewport, public FGCObject
{
public:
	SLATE_BEGIN_ARGS(SMocaraViewport) {}
		SLATE_EVENT(FOnMocaraBoneSelected, OnBoneSelected)
		SLATE_EVENT(FOnMocaraBoneTransformChanged, OnBoneTransformChanged)
		SLATE_EVENT(FSimpleDelegate, OnManipulationStarted)
		SLATE_EVENT(FSimpleDelegate, OnManipulationEnded)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SMocaraViewport() override;

	/** Point the preview at a mesh and clip. Either may be null to clear. */
	void SetPreview(USkeletalMesh* Mesh, UAnimSequence* Sequence);

	/** Start/stop looping playback. */
	void SetPlaying(bool bInPlaying);

	/** Scrub to a frame. Ignored while playing so playback is not fought. */
	void SetPlayheadFrame(int32 Frame, float Fps);

	void SetShowBones(bool bInShowBones);
	bool IsShowingBones() const { return bShowBones; }

	/** Frame the camera on the current preview mesh. */
	void FocusOnPreview();

	/** Snap the camera to a canonical angle. Front/Side make bone alignment obvious. */
	void SetViewPreset(EMocaraViewPreset Preset);
	EMocaraViewPreset GetViewPreset() const { return ViewPreset; }
	void FocusOnSelectedBone();
	void ResetView();

	/** Synchronize the full AutoPose stack and the selected key with the viewport widget. */
	void SetPoseKeys(const TArray<FMocaraPoseKey>& InPoseKeys);
	FName SetSelectedBone(FName BoneName, const FRotator& RotationOffset, const FVector& TranslationOffset);
	FName GetSelectedBone() const { return SelectedBone; }
	FRotator GetSelectedBoneRotation() const { return SelectedBoneRotation; }
	FVector GetSelectedBoneTranslation() const { return SelectedBoneTranslation; }
	bool CanTranslateSelectedBone() const;

	void SetTranslationMode(bool bInTranslationMode);
	bool IsTranslationMode() const { return bTranslationMode; }

	void SetUseLocalCoordinates(bool bInUseLocalCoordinates);
	bool IsUsingLocalCoordinates() const { return bUseLocalCoordinates; }

	/** Render the current frame and write it out as a .bmp. Used for automated visual checks. */
	bool CaptureToFile(const FString& Path);

	/** Keep the character centred as it travels. Preserves the user's orbit. */
	void SetFollowCharacter(bool bInFollow);
	bool IsFollowingCharacter() const { return bFollowCharacter; }

	USkeletalMesh* GetPreviewMesh() const;
	UDebugSkelMeshComponent* GetPreviewComponent() const { return PreviewComponent; }

	// FGCObject
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override { return TEXT("SMocaraViewport"); }

protected:
	// SEditorViewport
	virtual TSharedRef<FEditorViewportClient> MakeEditorViewportClient() override;
	virtual void BindCommands() override;

private:
	void UpdateFollowCamera();
	void ClearPosePreview();
	void ApplyPosePreview();
	void ApplyTwoBoneIkPreview(const TMap<FName, FMocaraPoseKey>& EvaluatedPose);
	bool TryCreateAssembledPreview(USkeletalMesh* TargetMesh);
	void SyncAssembledPreview();
	void DestroyAssembledPreview();
	FName ResolveBoneName(FName BoneName) const;
	void HandleBonePicked(FName BoneName);
	void HandleWidgetDelta(const FVector& Drag, const FRotator& Rotation);
	void NotifyManipulationStarted();
	void NotifyManipulationEnded();
	FTransform GetSelectedBoneWorldTransform() const;

	friend class FMocaraViewportClient;

	TSharedPtr<FAdvancedPreviewScene> PreviewScene;
	TSharedPtr<FMocaraViewportClient> MocaraViewportClient;

	/** Owned by this widget and kept alive via FGCObject. */
	TObjectPtr<UDebugSkelMeshComponent> PreviewComponent;
	/** Optional assembled MetaHuman; its body follows PreviewComponent's edited pose. */
	TObjectPtr<AActor> PreviewActor;
	TObjectPtr<USkeletalMeshComponent> PreviewBodyComponent;
	FOnMocaraBoneSelected OnBoneSelected;
	FOnMocaraBoneTransformChanged OnBoneTransformChanged;
	FSimpleDelegate OnManipulationStarted;
	FSimpleDelegate OnManipulationEnded;
	FName SelectedBone = TEXT("Hips");
	FRotator SelectedBoneRotation = FRotator::ZeroRotator;
	FVector SelectedBoneTranslation = FVector::ZeroVector;
	TArray<FMocaraPoseKey> PoseKeys;
	TSet<FName> PreviewModifiedBones;
	int32 PoseFrame = 0;

	bool bShowBones = true;
	bool bTranslationMode = false;
	bool bUseLocalCoordinates = true;
	bool bPlaying = false;
	EMocaraViewPreset ViewPreset = EMocaraViewPreset::Perspective;
	/**
	 * Off by default so the camera behaves like Unreal's own animation editor: it does not
	 * move, and a travelling clip simply leaves frame. Following looks worse than it
	 * sounds here, because the travel is baked into the bone track and the only thing
	 * available to track is the pelvis, which carries the whole gait oscillation.
	 */
	bool bFollowCharacter = false;
	bool bHasTrackedLocation = false;
	FVector LastTrackedLocation = FVector::ZeroVector;
};
