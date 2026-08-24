#include "SMocaraViewport.h"
#include "MocaraViewportMath.h"

#include "AdvancedPreviewScene.h"
#include "AnimPreviewInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/DebugSkelMeshComponent.h"
#include "BoneControllers/AnimNode_ModifyBone.h"
#include "Components/SkeletalMeshComponent.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "InputCoreTypes.h"
#include "MocaraBoneMap.h"
#include "MocaraPoseEditing.h"
#include "MocaraPreviewCharacterResolver.h"
#include "MocaraSettings.h"
#include "MocaraTargetProfile.h"
#include "PersonaSelectionProxies.h"
#include "ReferenceSkeleton.h"
#include "SceneManagement.h"
#include "Misc/FileHelper.h"
#include "TwoBoneIK.h"
#include "UnrealClient.h"

namespace
{
	/**
	 * Skip virtual/IK bones in the overlay. They are parented under the static root and sit
	 * at the world origin, so once the character travels they draw metre-long streaks across
	 * the viewport. Covers the UE5 mannequin rig and the SOMA "Root" placeholder.
	 */
	bool IsOverlayBone(const FName BoneName)
	{
		return FMocaraPoseEditing::IsSelectableBone(BoneName);
	}
}

/** Viewport client: ticks the preview world and draws the bone overlay. */
class FMocaraViewportClient : public FEditorViewportClient
{
public:
	FMocaraViewportClient(FAdvancedPreviewScene* InPreviewScene, const TSharedRef<SMocaraViewport>& InViewport)
		: FEditorViewportClient(nullptr, InPreviewScene, StaticCastSharedRef<SEditorViewport>(InViewport))
		, MocaraViewportPtr(InViewport)
	{
		SetRealtime(true);
		SetViewMode(VMI_Lit);
		bSetListenerPosition = false;

		// Roughly waist height, backed off enough to frame a ~1.8m character.
		// Front three-quarter view: the useful initial framing is the one that shows the
		// character's front. Travelling clips can opt into follow mode from the toolbar.
		SetViewLocation(FVector(260.f, 260.f, 130.f));
		SetLookAtLocation(FVector(0.f, 0.f, 95.f));

		EngineShowFlags.SetGrid(true);
		EngineShowFlags.SetSelectionOutline(true);
		EngineShowFlags.SetModeWidgets(true);
		// This self-contained asset viewport supplies its own transform target and delta
		// handling. Disable the selection-driven interactive gizmo path so Unreal renders
		// the legacy native widget that calls the overrides below.
		GetModeTools()->SetSupportsViewportITF(false);
		GetModeTools()->ActivateDefaultMode();
		GetModeTools()->SetWidgetMode(UE::Widget::WM_Rotate);
		GetModeTools()->SetWidgetScale(0.55f);
	}

	virtual void Tick(float DeltaSeconds) override
	{
		FEditorViewportClient::Tick(DeltaSeconds);

		// A preview scene's world does not tick itself.
		if (PreviewScene)
		{
			PreviewScene->GetWorld()->Tick(LEVELTICK_All, DeltaSeconds);
		}
	}

	virtual void Draw(const FSceneView* View, FPrimitiveDrawInterface* PDI) override
	{
		FEditorViewportClient::Draw(View, PDI);

		const TSharedPtr<SMocaraViewport> ViewportWidget = MocaraViewportPtr.Pin();
		if (!ViewportWidget.IsValid() || !ViewportWidget->IsShowingBones())
		{
			return;
		}

		UDebugSkelMeshComponent* Component = ViewportWidget->GetPreviewComponent();
		if (!Component || !Component->GetSkeletalMeshAsset())
		{
			return;
		}

		// Draw the skeleton as component-space line segments. Doing this ourselves rather
		// than relying on the scene proxy keeps it available for bone picking later.
		const FReferenceSkeleton& Ref = Component->GetSkeletalMeshAsset()->GetRefSkeleton();
		const TArray<FTransform>& Spaces = Component->GetComponentSpaceTransforms();
		if (Spaces.Num() != Ref.GetNum())
		{
			return;
		}

		const FTransform& ToWorld = Component->GetComponentTransform();
		for (int32 Index = 0; Index < Ref.GetNum(); ++Index)
		{
			const FName BoneName = Ref.GetBoneName(Index);
			if (!IsOverlayBone(BoneName))
			{
				continue;
			}
			const bool bSelected = BoneName == ViewportWidget->GetSelectedBone();
			const int32 Parent = Ref.GetParentIndex(Index);
			const FVector Head = ToWorld.TransformPosition(Spaces[Index].GetLocation());
			PDI->SetHitProxy(new HPersonaBoneHitProxy(Index, BoneName));

			// Only the selected bone draws through the mesh, so it stays findable and
			// pickable. Drawing the WHOLE skeleton in the foreground group disables depth
			// testing for it, so far-side bones paint over the near-side body -- on a
			// T-pose that reads fine, but on a running figure with limbs crossing it looks
			// like scattered markers floating free of the character rather than a skeleton.
			const ESceneDepthPriorityGroup Depth = bSelected ? SDPG_Foreground : SDPG_World;
			if (Parent != INDEX_NONE && IsOverlayBone(Ref.GetBoneName(Parent)))
			{
				const FVector Tail = ToWorld.TransformPosition(Spaces[Parent].GetLocation());
				PDI->DrawLine(Tail, Head,
					bSelected ? FLinearColor(1.f, 0.42f, 0.08f) : FLinearColor(0.15f, 0.85f, 0.35f),
					Depth, bSelected ? 2.2f : 0.9f);
			}
			PDI->DrawPoint(Head,
				bSelected ? FLinearColor(1.f, 0.18f, 0.02f) : FLinearColor(1.f, 0.85f, 0.1f),
				bSelected ? 8.f : 5.f, Depth);
			PDI->SetHitProxy(nullptr);
		}
	}

	virtual void ProcessClick(FSceneView& View, HHitProxy* HitProxy, FKey Key, EInputEvent Event, uint32 HitX, uint32 HitY) override
	{
		if (Key == EKeys::LeftMouseButton)
		{
			if (HPersonaBoneHitProxy* BoneProxy = HitProxyCast<HPersonaBoneHitProxy>(HitProxy))
			{
				if (const TSharedPtr<SMocaraViewport> ViewportWidget = MocaraViewportPtr.Pin())
				{
					ViewportWidget->HandleBonePicked(BoneProxy->BoneName);
					SetWidgetMode(ViewportWidget->IsTranslationMode() && ViewportWidget->CanTranslateSelectedBone()
						? UE::Widget::WM_Translate : UE::Widget::WM_Rotate);
					Invalidate();
					return;
				}
			}
		}
		FEditorViewportClient::ProcessClick(View, HitProxy, Key, Event, HitX, HitY);
	}

	virtual bool InputWidgetDelta(FViewport* InViewport, EAxisList::Type CurrentAxis, FVector& Drag, FRotator& Rot, FVector& Scale) override
	{
		const TSharedPtr<SMocaraViewport> ViewportWidget = MocaraViewportPtr.Pin();
		if (!ViewportWidget.IsValid() || CurrentAxis == EAxisList::None)
		{
			return false;
		}

		const bool bHasTranslation = !Drag.IsNearlyZero() && ViewportWidget->IsTranslationMode();
		const bool bHasRotation = !Rot.IsNearlyZero() && !ViewportWidget->IsTranslationMode();
		if (!bHasTranslation && !bHasRotation)
		{
			return false;
		}
		ViewportWidget->HandleWidgetDelta(Drag, Rot);
		if (bHasRotation)
		{
			ApplyDeltaToRotateWidget(Rot);
		}
		InViewport->Invalidate();
		return true;
	}

	virtual void TrackingStarted(const FInputEventState& InInputState, bool bIsDraggingWidget, bool bNudge) override
	{
		bManipulatingBone = bIsDraggingWidget && !GetSelectedBoneName().IsNone();
		if (bManipulatingBone)
		{
			if (const TSharedPtr<SMocaraViewport> ViewportWidget = MocaraViewportPtr.Pin())
			{
				ViewportWidget->NotifyManipulationStarted();
			}
		}
	}

	virtual void TrackingStopped() override
	{
		if (bManipulatingBone)
		{
			if (const TSharedPtr<SMocaraViewport> ViewportWidget = MocaraViewportPtr.Pin())
			{
				ViewportWidget->NotifyManipulationEnded();
			}
		}
		bManipulatingBone = false;
	}

	virtual bool CanSetWidgetMode(UE::Widget::EWidgetMode NewMode) const override
	{
		if (NewMode == UE::Widget::WM_Rotate)
		{
			return true;
		}
		if (NewMode == UE::Widget::WM_Translate)
		{
			if (const TSharedPtr<SMocaraViewport> ViewportWidget = MocaraViewportPtr.Pin())
			{
				return ViewportWidget->CanTranslateSelectedBone();
			}
		}
		return false;
	}

	virtual bool CanCycleWidgetMode() const override { return false; }

	virtual FVector GetWidgetLocation() const override
	{
		if (const TSharedPtr<SMocaraViewport> ViewportWidget = MocaraViewportPtr.Pin())
		{
			return ViewportWidget->GetSelectedBoneWorldTransform().GetLocation();
		}
		return FVector::ZeroVector;
	}

	virtual FMatrix GetWidgetCoordSystem() const override
	{
		if (const TSharedPtr<SMocaraViewport> ViewportWidget = MocaraViewportPtr.Pin())
		{
			if (ViewportWidget->IsUsingLocalCoordinates())
			{
				return ViewportWidget->GetSelectedBoneWorldTransform().ToMatrixNoScale().RemoveTranslation();
			}
		}
		return FMatrix::Identity;
	}

	virtual ECoordSystem GetWidgetCoordSystemSpace() const override
	{
		if (const TSharedPtr<SMocaraViewport> ViewportWidget = MocaraViewportPtr.Pin())
		{
			return ViewportWidget->IsUsingLocalCoordinates() ? COORD_Local : COORD_World;
		}
		return COORD_World;
	}

private:
	FName GetSelectedBoneName() const
	{
		if (const TSharedPtr<SMocaraViewport> ViewportWidget = MocaraViewportPtr.Pin())
		{
			return ViewportWidget->GetSelectedBone();
		}
		return NAME_None;
	}

	TWeakPtr<SMocaraViewport> MocaraViewportPtr;
	bool bManipulatingBone = false;
};

void SMocaraViewport::Construct(const FArguments& InArgs)
{
	OnBoneSelected = InArgs._OnBoneSelected;
	OnBoneTransformChanged = InArgs._OnBoneTransformChanged;
	OnManipulationStarted = InArgs._OnManipulationStarted;
	OnManipulationEnded = InArgs._OnManipulationEnded;

	FAdvancedPreviewScene::ConstructionValues CVS;
	CVS.bCreatePhysicsScene = false;
	CVS.LightBrightness = 3.f;
	CVS.SkyBrightness = 1.f;
	PreviewScene = MakeShared<FAdvancedPreviewScene>(CVS);
	PreviewScene->SetFloorVisibility(true);

	PreviewComponent = NewObject<UDebugSkelMeshComponent>();
	PreviewComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	PreviewComponent->bSelectable = false;
	PreviewScene->AddComponent(PreviewComponent, FTransform::Identity);

	SEditorViewport::Construct(SEditorViewport::FArguments());
}

SMocaraViewport::~SMocaraViewport()
{
	DestroyAssembledPreview();
	if (PreviewComponent && PreviewScene.IsValid())
	{
		PreviewScene->RemoveComponent(PreviewComponent);
	}
	PreviewComponent = nullptr;
	MocaraViewportClient.Reset();
	PreviewScene.Reset();
}

TSharedRef<FEditorViewportClient> SMocaraViewport::MakeEditorViewportClient()
{
	MocaraViewportClient = MakeShared<FMocaraViewportClient>(PreviewScene.Get(), SharedThis(this));
	return MocaraViewportClient.ToSharedRef();
}

void SMocaraViewport::BindCommands()
{
	SEditorViewport::BindCommands();
}

void SMocaraViewport::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObject(PreviewComponent);
	Collector.AddReferencedObject(PreviewActor);
	Collector.AddReferencedObject(PreviewBodyComponent);
}

USkeletalMesh* SMocaraViewport::GetPreviewMesh() const
{
	return PreviewComponent ? PreviewComponent->GetSkeletalMeshAsset() : nullptr;
}

void SMocaraViewport::SetPreview(USkeletalMesh* Mesh, UAnimSequence* Sequence)
{
	if (!PreviewComponent)
	{
		return;
	}

	// A mesh with no LODs would fatally assert inside the component, and the importer can
	// legitimately hand us nothing when a generate failed.
	if (Mesh && Mesh->GetLODNum() <= 0)
	{
		Mesh = nullptr;
		Sequence = nullptr;
	}

	ClearPosePreview();
	DestroyAssembledPreview();
	PreviewComponent->SetSkeletalMesh(Mesh);
	if (Mesh && Sequence)
	{
		PreviewComponent->EnablePreview(true, Sequence);
		if (PreviewComponent->PreviewInstance)
		{
			PreviewComponent->PreviewInstance->SetPosition(0.f, false);
			PreviewComponent->PreviewInstance->SetPlaying(false);
		}
	}
	PreviewComponent->MarkRenderStateDirty();
	const bool bUsingAssembledPreview = Mesh && TryCreateAssembledPreview(Mesh);
	PreviewComponent->SetVisibility(!bUsingAssembledPreview, false);
	bHasTrackedLocation = false;

	if (Mesh)
	{
		SelectedBone = ResolveBoneName(SelectedBone);
		ApplyPosePreview();
		OnBoneSelected.ExecuteIfBound(SelectedBone);
		FocusOnPreview();
	}
}

bool SMocaraViewport::TryCreateAssembledPreview(USkeletalMesh* TargetMesh)
{
	if (!TargetMesh || !PreviewScene.IsValid() || !PreviewScene->GetWorld())
	{
		return false;
	}

	UClass* PreferredClass = nullptr;
	if (const UMocaraSettings* Settings = GetDefault<UMocaraSettings>())
	{
		PreferredClass = Settings->PreviewCharacterClass.LoadSynchronous();
	}
	const TOptional<FMocaraTargetProfile> TargetProfile = FMocaraTargetProfile::ForMesh(TargetMesh);
	if (!PreferredClass && (!TargetProfile.IsSet() || TargetProfile->ProfileName != TEXT("MetaHumanBody")))
	{
		return false;
	}
	FName BodyComponentName;
	UClass* PreviewClass = FMocaraPreviewCharacterResolver::FindCompatibleClass(
		TargetMesh, PreferredClass, BodyComponentName);
	if (!PreviewClass || PreviewClass->HasAnyClassFlags(CLASS_Abstract))
	{
		return false;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags |= RF_Transient;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* SpawnedActor = PreviewScene->GetWorld()->SpawnActor<AActor>(
		PreviewClass, FTransform::Identity, SpawnParameters);
	USkeletalMeshComponent* BodyComponent = FMocaraPreviewCharacterResolver::FindCompatibleBodyComponent(
		SpawnedActor, TargetMesh, BodyComponentName);
	if (!SpawnedActor || !BodyComponent)
	{
		if (SpawnedActor)
		{
			SpawnedActor->Destroy();
		}
		return false;
	}

	PreviewActor = SpawnedActor;
	PreviewBodyComponent = BodyComponent;
	PreviewComponent->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	PreviewBodyComponent->SetLeaderPoseComponent(PreviewComponent, true, false);
	SyncAssembledPreview();
	return true;
}

void SMocaraViewport::SyncAssembledPreview()
{
	if (!PreviewActor || !PreviewBodyComponent || !PreviewComponent)
	{
		return;
	}
	PreviewBodyComponent->RefreshBoneTransforms();
	TInlineComponentArray<USkeletalMeshComponent*> Components(PreviewActor);
	for (USkeletalMeshComponent* Component : Components)
	{
		if (Component)
		{
			Component->MarkRenderTransformDirty();
			Component->MarkRenderDynamicDataDirty();
		}
	}
}

void SMocaraViewport::DestroyAssembledPreview()
{
	if (PreviewBodyComponent)
	{
		PreviewBodyComponent->SetLeaderPoseComponent(nullptr, true, false);
	}
	PreviewBodyComponent = nullptr;
	if (PreviewActor)
	{
		PreviewActor->Destroy();
	}
	PreviewActor = nullptr;
	if (PreviewComponent)
	{
		PreviewComponent->SetVisibility(true, false);
	}
}

void SMocaraViewport::SetPlaying(bool bInPlaying)
{
	// The Mocara timeline is the single clock: it advances CurrentFrame and pushes each
	// frame in via SetPlayheadFrame. Letting the single-node instance also self-advance
	// would give us two clocks that drift apart, so keep the component stopped.
	bPlaying = bInPlaying;
	if (PreviewComponent && PreviewComponent->GetSkeletalMeshAsset())
	{
		if (PreviewComponent->PreviewInstance)
		{
			PreviewComponent->PreviewInstance->SetPlaying(false);
		}
	}
}

void SMocaraViewport::SetPlayheadFrame(int32 Frame, float Fps)
{
	PoseFrame = FMath::Max(0, Frame);
	if (!PreviewComponent || !PreviewComponent->GetSkeletalMeshAsset())
	{
		return;
	}
	const float SafeFps = (Fps > KINDA_SMALL_NUMBER) ? Fps : 30.f;
	ClearPosePreview();
	PreviewComponent->SetPosition(PoseFrame / SafeFps, false);

	// SetPosition only moves the playhead; without an explicit evaluation the rendered
	// pose stays on whatever frame was last ticked.
	PreviewComponent->TickAnimation(0.f, false);
	PreviewComponent->RefreshBoneTransforms();
	PreviewComponent->MarkRenderTransformDirty();
	ApplyPosePreview();

	UpdateFollowCamera();
}

FName SMocaraViewport::ResolveBoneName(FName BoneName) const
{
	USkeletalMesh* Mesh = GetPreviewMesh();
	if (!Mesh || Mesh->GetRefSkeleton().FindBoneIndex(BoneName) != INDEX_NONE)
	{
		return BoneName;
	}

	for (const TPair<FName, FName>& Pair : FMocaraBoneMap::MannyToSoma())
	{
		if (Pair.Value == BoneName && Mesh->GetRefSkeleton().FindBoneIndex(Pair.Key) != INDEX_NONE)
		{
			return Pair.Key;
		}
	}
	return NAME_None;
}

void SMocaraViewport::SetPoseKeys(const TArray<FMocaraPoseKey>& InPoseKeys)
{
	PoseKeys = InPoseKeys;
	ApplyPosePreview();
}

FName SMocaraViewport::SetSelectedBone(FName BoneName, const FRotator& RotationOffset, const FVector& TranslationOffset)
{
	SelectedBone = ResolveBoneName(BoneName);
	SelectedBoneRotation = RotationOffset.GetNormalized();
	SelectedBoneTranslation = TranslationOffset;
	if (bTranslationMode && !CanTranslateSelectedBone())
	{
		SetTranslationMode(false);
	}
	ApplyPosePreview();
	if (MocaraViewportClient.IsValid())
	{
		MocaraViewportClient->Invalidate();
	}
	return SelectedBone;
}

bool SMocaraViewport::CanTranslateSelectedBone() const
{
	return FMocaraPoseEditing::CanTranslateBone(SelectedBone);
}

void SMocaraViewport::SetTranslationMode(bool bInTranslationMode)
{
	bTranslationMode = bInTranslationMode && CanTranslateSelectedBone();
	if (MocaraViewportClient.IsValid())
	{
		MocaraViewportClient->SetWidgetMode(bTranslationMode ? UE::Widget::WM_Translate : UE::Widget::WM_Rotate);
		MocaraViewportClient->Invalidate();
	}
}

void SMocaraViewport::HandleBonePicked(FName BoneName)
{
	SelectedBone = BoneName;
	SelectedBoneRotation = FRotator::ZeroRotator;
	SelectedBoneTranslation = FVector::ZeroVector;
	OnBoneSelected.ExecuteIfBound(BoneName);
	ApplyPosePreview();
}

void SMocaraViewport::HandleWidgetDelta(const FVector& Drag, const FRotator& Rotation)
{
	if (bTranslationMode)
	{
		if (!CanTranslateSelectedBone())
		{
			return;
		}
		const FVector ComponentDelta = PreviewComponent
			? PreviewComponent->GetComponentTransform().InverseTransformVectorNoScale(Drag)
			: Drag;
		SelectedBoneTranslation += ComponentDelta;
	}
	else
	{
		SelectedBoneRotation = FMocaraPoseEditing::AccumulateRotation(
			SelectedBoneRotation, Rotation, bUseLocalCoordinates);
	}
	OnBoneTransformChanged.ExecuteIfBound(SelectedBoneRotation, SelectedBoneTranslation);
	ApplyPosePreview();
}

void SMocaraViewport::NotifyManipulationStarted()
{
	OnManipulationStarted.ExecuteIfBound();
}

void SMocaraViewport::NotifyManipulationEnded()
{
	OnManipulationEnded.ExecuteIfBound();
}

void SMocaraViewport::ClearPosePreview()
{
	if (!PreviewComponent || !PreviewComponent->PreviewInstance)
	{
		return;
	}
	for (const FName BoneName : PreviewModifiedBones)
	{
		PreviewComponent->PreviewInstance->RemoveBoneModification(BoneName);
	}
	PreviewModifiedBones.Reset();
}

void SMocaraViewport::ApplyPosePreview()
{
	if (!PreviewComponent || !PreviewComponent->PreviewInstance || !PreviewComponent->GetSkeletalMeshAsset())
	{
		return;
	}

	ClearPosePreview();
	TMap<FName, FMocaraPoseKey> EvaluatedPose;
	FMocaraPoseEditing::EvaluatePoseAtFrame(PoseKeys, PoseFrame, EvaluatedPose);
	for (const TPair<FName, FMocaraPoseKey>& Pair : EvaluatedPose)
	{
		const FName PreviewBone = ResolveBoneName(Pair.Key);
		if (PreviewBone.IsNone())
		{
			continue;
		}

		const FMocaraPoseKey& Key = Pair.Value;
		FAnimNode_ModifyBone& Controller = PreviewComponent->PreviewInstance->ModifyBone(PreviewBone);
		Controller.RotationMode = Key.RotationOffset.IsNearlyZero() ? BMM_Ignore : BMM_Additive;
		Controller.RotationSpace = BCS_BoneSpace;
		Controller.Rotation = Key.RotationOffset;
		const bool bIsLimbIk = Key.bUseTwoBoneIK
			&& FMocaraPoseEditing::ClassifyBoneLane(PreviewBone) != EMocaraPoseLane::RootPath;
		Controller.TranslationMode = (!bIsLimbIk && !Key.TranslationOffset.IsNearlyZero()) ? BMM_Additive : BMM_Ignore;
		Controller.TranslationSpace = BCS_ComponentSpace;
		Controller.Translation = Key.TranslationOffset;
		Controller.ScaleMode = BMM_Ignore;
		PreviewModifiedBones.Add(PreviewBone);
	}

	PreviewComponent->TickAnimation(0.f, false);
	PreviewComponent->RefreshBoneTransforms();
	ApplyTwoBoneIkPreview(EvaluatedPose);
	PreviewComponent->TickAnimation(0.f, false);
	PreviewComponent->RefreshBoneTransforms();
	PreviewComponent->MarkRenderDynamicDataDirty();
	SyncAssembledPreview();
}

void SMocaraViewport::ApplyTwoBoneIkPreview(const TMap<FName, FMocaraPoseKey>& EvaluatedPose)
{
	USkeletalMesh* Mesh = GetPreviewMesh();
	if (!Mesh || !PreviewComponent || !PreviewComponent->PreviewInstance)
	{
		return;
	}
	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	const TArray<FTransform>& Spaces = PreviewComponent->GetComponentSpaceTransforms();
	for (const TPair<FName, FMocaraPoseKey>& Pair : EvaluatedPose)
	{
		const FMocaraPoseKey& Key = Pair.Value;
		const FName EndBone = ResolveBoneName(Pair.Key);
		const EMocaraPoseLane Lane = FMocaraPoseEditing::ClassifyBoneLane(EndBone);
		if (!Key.bUseTwoBoneIK || Key.TranslationOffset.IsNearlyZero()
			|| (Lane != EMocaraPoseLane::LeftHand && Lane != EMocaraPoseLane::RightHand
				&& Lane != EMocaraPoseLane::LeftFoot && Lane != EMocaraPoseLane::RightFoot))
		{
			continue;
		}

		const int32 EndIndex = Ref.FindBoneIndex(EndBone);
		const int32 JointIndex = EndIndex == INDEX_NONE ? INDEX_NONE : Ref.GetParentIndex(EndIndex);
		const int32 RootIndex = JointIndex == INDEX_NONE ? INDEX_NONE : Ref.GetParentIndex(JointIndex);
		if (!Spaces.IsValidIndex(EndIndex) || !Spaces.IsValidIndex(JointIndex) || !Spaces.IsValidIndex(RootIndex))
		{
			continue;
		}

		FTransform RootTransform = Spaces[RootIndex];
		FTransform JointTransform = Spaces[JointIndex];
		FTransform EndTransform = Spaces[EndIndex];
		const FVector Chain = EndTransform.GetLocation() - RootTransform.GetLocation();
		const FVector Along = RootTransform.GetLocation()
			+ Chain * FVector::DotProduct(JointTransform.GetLocation() - RootTransform.GetLocation(), Chain)
				/ FMath::Max(Chain.SizeSquared(), UE_SMALL_NUMBER);
		FVector Bend = JointTransform.GetLocation() - Along;
		if (!Bend.Normalize())
		{
			Bend = FVector::CrossProduct(Chain, FVector::UpVector).GetSafeNormal();
		}
		const FVector JointTarget = JointTransform.GetLocation() + Bend * 100.f;
		const FVector EffectorTarget = EndTransform.GetLocation() + Key.TranslationOffset;
		AnimationCore::SolveTwoBoneIK(
			RootTransform, JointTransform, EndTransform, JointTarget, EffectorTarget, false, 1.0, 1.0);

		auto ApplyComponentTransform = [this](FName BoneName, const FTransform& Transform, bool bSetTranslation)
		{
			FAnimNode_ModifyBone& Controller = PreviewComponent->PreviewInstance->ModifyBone(BoneName);
			Controller.RotationMode = BMM_Replace;
			Controller.RotationSpace = BCS_ComponentSpace;
			Controller.Rotation = Transform.Rotator();
			Controller.TranslationMode = bSetTranslation ? BMM_Replace : BMM_Ignore;
			Controller.TranslationSpace = BCS_ComponentSpace;
			Controller.Translation = Transform.GetLocation();
			Controller.ScaleMode = BMM_Ignore;
			PreviewModifiedBones.Add(BoneName);
		};
		ApplyComponentTransform(Ref.GetBoneName(RootIndex), RootTransform, false);
		ApplyComponentTransform(Ref.GetBoneName(JointIndex), JointTransform, false);
		ApplyComponentTransform(EndBone, EndTransform, true);
	}
}

FTransform SMocaraViewport::GetSelectedBoneWorldTransform() const
{
	if (!PreviewComponent || !PreviewComponent->GetSkeletalMeshAsset() || SelectedBone.IsNone())
	{
		return FTransform::Identity;
	}
	const int32 Index = PreviewComponent->GetSkeletalMeshAsset()->GetRefSkeleton().FindBoneIndex(SelectedBone);
	const TArray<FTransform>& Spaces = PreviewComponent->GetComponentSpaceTransforms();
	return Spaces.IsValidIndex(Index) ? Spaces[Index] * PreviewComponent->GetComponentTransform() : FTransform::Identity;
}

void SMocaraViewport::SetUseLocalCoordinates(bool bInUseLocalCoordinates)
{
	bUseLocalCoordinates = bInUseLocalCoordinates;
	if (MocaraViewportClient.IsValid())
	{
		MocaraViewportClient->Invalidate();
	}
}

void SMocaraViewport::SetFollowCharacter(bool bInFollow)
{
	bFollowCharacter = bInFollow;
	bHasTrackedLocation = false;
}

void SMocaraViewport::UpdateFollowCamera()
{
	if (!bFollowCharacter || !MocaraViewportClient.IsValid() || !PreviewComponent)
	{
		return;
	}
	USkeletalMesh* Mesh = PreviewComponent->GetSkeletalMeshAsset();
	if (!Mesh)
	{
		return;
	}

	// Track the pelvis rather than the root: on Manny the root bone is static.
	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	int32 Index = Ref.FindBoneIndex(TEXT("pelvis"));
	if (Index == INDEX_NONE)
	{
		Index = Ref.FindBoneIndex(TEXT("Hips"));
	}
	const TArray<FTransform>& Spaces = PreviewComponent->GetComponentSpaceTransforms();
	if (Index == INDEX_NONE || !Spaces.IsValidIndex(Index))
	{
		return;
	}

	FVector Tracked = PreviewComponent->GetComponentTransform().TransformPosition(Spaces[Index].GetLocation());
	Tracked.Z = 0.f;   // only follow along the ground plane

	if (bHasTrackedLocation)
	{
		// Translate only, so any orbit the user has set is preserved.
		const FVector Delta = Tracked - LastTrackedLocation;

		// A playhead that wraps (or is scrubbed) jumps the pelvis from the end of the
		// travel back to its start. Following that delta throws the camera the entire
		// length of the clip in one frame, every loop. Re-anchor instead of chasing it.
		if (UE::Mocara::Viewport::ShouldReanchorFollowCamera(Delta))
		{
			LastTrackedLocation = Tracked;
			return;
		}

		if (!Delta.IsNearlyZero())
		{
			MocaraViewportClient->SetViewLocation(MocaraViewportClient->GetViewLocation() + Delta);
			MocaraViewportClient->SetLookAtLocation(MocaraViewportClient->GetLookAtLocation() + Delta, true);
		}
	}
	LastTrackedLocation = Tracked;
	bHasTrackedLocation = true;
}

void SMocaraViewport::SetShowBones(bool bInShowBones)
{
	bShowBones = bInShowBones;
	if (MocaraViewportClient.IsValid())
	{
		MocaraViewportClient->Invalidate();
	}
}

void SMocaraViewport::FocusOnPreview()
{
	USkeletalMesh* Mesh = PreviewComponent ? PreviewComponent->GetSkeletalMeshAsset() : nullptr;
	if (!MocaraViewportClient.IsValid() || !Mesh)
	{
		return;
	}

	// Explicit 3/4 framing scaled to the character's height. FocusViewportOnBox fits the
	// box edge-to-edge, which is either claustrophobic (tight mesh bounds) or tiny (padded
	// bounds), so place the camera directly instead.
	const float Height = FMath::Max(Mesh->GetBounds().GetBox().GetSize().Z, 180.f);
	const FVector Target(0.f, 0.f, Height * 0.5f);
	const float Distance = Height * 0.85f;

	const FVector Anchor = bHasTrackedLocation ? FVector(LastTrackedLocation.X, LastTrackedLocation.Y, 0.f) : FVector::ZeroVector;
	MocaraViewportClient->SetViewLocation(Anchor + Target + FVector(Distance * 0.7f, Distance * 0.7f, Height * 0.3f));
	MocaraViewportClient->SetLookAtLocation(Anchor + Target, true);
	MocaraViewportClient->Invalidate();
}

void SMocaraViewport::FocusOnSelectedBone()
{
	if (!MocaraViewportClient.IsValid() || SelectedBone.IsNone())
	{
		return;
	}

	const FVector Target = GetSelectedBoneWorldTransform().GetLocation();
	FVector ViewOffset = MocaraViewportClient->GetViewLocation() - MocaraViewportClient->GetLookAtLocation();
	const float Distance = FMath::Clamp(ViewOffset.Size(), 70.f, 220.f);
	if (!ViewOffset.Normalize())
	{
		ViewOffset = FVector(1.f, 1.f, 0.4f).GetSafeNormal();
	}
	MocaraViewportClient->SetLookAtLocation(Target, true);
	MocaraViewportClient->SetViewLocation(Target + ViewOffset * Distance);
	MocaraViewportClient->Invalidate();
}

void SMocaraViewport::SetViewPreset(EMocaraViewPreset Preset)
{
	ViewPreset = Preset;
	USkeletalMesh* Mesh = PreviewComponent ? PreviewComponent->GetSkeletalMeshAsset() : nullptr;
	if (!MocaraViewportClient.IsValid() || !Mesh)
	{
		return;
	}

	const float Height = FMath::Max(Mesh->GetBounds().GetBox().GetSize().Z, 180.f);
	const FVector Anchor = bHasTrackedLocation
		? FVector(LastTrackedLocation.X, LastTrackedLocation.Y, 0.f)
		: FVector::ZeroVector;
	const FVector Target = Anchor + FVector(0.f, 0.f, Height * 0.5f);
	const float Distance = Height * 1.15f;

	// Character faces +X, so "front" looks back along -X and "side" looks along -Y.
	FVector Offset;
	switch (Preset)
	{
	case EMocaraViewPreset::Front: Offset = FVector(Distance, 0.f, 0.f); break;
	case EMocaraViewPreset::Side:  Offset = FVector(0.f, Distance, 0.f); break;
	case EMocaraViewPreset::Top:   Offset = FVector(0.f, 0.f, Distance); break;
	default:                       Offset = FVector(Distance * 0.7f, Distance * 0.7f, Height * 0.3f); break;
	}

	MocaraViewportClient->SetViewLocation(Target + Offset);
	MocaraViewportClient->SetLookAtLocation(Target, true);
	MocaraViewportClient->Invalidate();
}

void SMocaraViewport::ResetView()
{
	bHasTrackedLocation = false;
	FocusOnPreview();
}

bool SMocaraViewport::CaptureToFile(const FString& Path)
{
	if (!MocaraViewportClient.IsValid() || MocaraViewportClient->Viewport == nullptr)
	{
		return false;
	}
	FViewport* Vp = MocaraViewportClient->Viewport;
	Vp->Draw();

	TArray<FColor> Bitmap;
	if (!Vp->ReadPixels(Bitmap))
	{
		return false;
	}
	const FIntPoint Size = Vp->GetSizeXY();
	for (FColor& Pixel : Bitmap)
	{
		Pixel.A = 255;
	}
	return FFileHelper::CreateBitmap(*Path, Size.X, Size.Y, Bitmap.GetData());
}
