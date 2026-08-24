#include "SMocaraWindow.h"
#include "SMocaraTimeline.h"
#include "SMocaraViewport.h"
#include "AssetExportTask.h"
#include "UObject/GCObjectScopeGuard.h"
#include "Misc/TransactionObjectEvent.h"
#include "DesktopPlatformModule.h"
#include "Exporters/Exporter.h"
#include "Exporters/FbxExportOption.h"
#include "IDesktopPlatform.h"
#include "MocaraAutoPose.h"
#include "MocaraBoneMap.h"
#include "MocaraBvhImporter.h"
#include "MocaraPoseEditing.h"
#include "MocaraRetargeter.h"
#include "MocaraSettings.h"
#include "MocaraTargetProfile.h"
#include "MocaraSidecarLauncher.h"
#include "Animation/AnimSequence.h"
#include "Engine/SkeletalMesh.h"
#include "Editor.h"
#include "Containers/Ticker.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "ScopedTransaction.h"
#include "InputCoreTypes.h"
#include "Retargeter/IKRetargeter.h"
#include "Rig/IKRigDefinition.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "Mocara"

namespace
{
	FString WithAssetSaveWarning(const FString& Status, bool bAutoSave, bool bAllAssetsSaved)
	{
		if (!bAutoSave || bAllAssetsSaved)
		{
			return Status;
		}
		return Status + TEXT(" Warning: one or more generated assets could not be saved; check the Output Log before closing the editor.");
	}

	/**
	 * Export an animation to FBX. This is the hand-off format: the .uasset only travels
	 * between people who share the project, whereas an FBX opens in Blender/Maya and
	 * imports into any other Unreal project.
	 */
	bool ExportSequenceToFbx(UAnimSequence* Sequence, const FString& Filename, FString& OutError)
	{
		if (!Sequence)
		{
			OutError = TEXT("No animation to export. Generate or load a clip first.");
			return false;
		}

		// Animation-only by default: no Geometry/Deformer records, just the skeleton and
		// curves. bExportPreviewMesh is what decides that, so drive it from settings
		// rather than leaving Options null and inheriting whatever the editor last used.
		UFbxExportOption* Options = NewObject<UFbxExportOption>();
		FGCObjectScopeGuard OptionsGuard(Options);
		Options->bExportPreviewMesh = false;
		if (const UMocaraSettings* ExportSettings = GetDefault<UMocaraSettings>())
		{
			Options->bExportPreviewMesh = ExportSettings->bExportMeshWithAnimation;
		}

		UAssetExportTask* Task = NewObject<UAssetExportTask>();
		FGCObjectScopeGuard TaskGuard(Task);
		Task->Options = Options;
		Task->Object = Sequence;
		Task->Exporter = nullptr;        // resolved from the file extension
		Task->Filename = Filename;
		Task->bSelected = false;
		Task->bReplaceIdentical = true;
		Task->bPrompt = false;           // no modal dialogs
		Task->bAutomated = true;
		Task->bUseFileArchive = false;
		Task->bWriteEmptyFiles = false;

		if (!UExporter::RunAssetExportTask(Task))
		{
			OutError = Task->Errors.Num()
				? FString::Join(Task->Errors, TEXT("; "))
				: FString::Printf(TEXT("FBX export failed for %s"), *Sequence->GetName());
			return false;
		}
		return true;
	}

	/** Weak handle to the open tab so console commands can drive it. */
	TWeakPtr<SMocaraWindow> GLiveMocaraWindow;
}

TWeakPtr<SMocaraWindow> SMocaraWindow::GetLiveWindow()
{
	return GLiveMocaraWindow;
}

SMocaraWindow::~SMocaraWindow()
{
	if (bTimelineTransactionActive && GEditor)
	{
		GEditor->EndTransaction();
		bTimelineTransactionActive = false;
	}
	if (bPoseControlTransactionActive && GEditor)
	{
		GEditor->EndTransaction();
		bPoseControlTransactionActive = false;
	}
	if (bViewportTransactionActive && GEditor)
	{
		GEditor->EndTransaction();
		bViewportTransactionActive = false;
	}
	if (GEditor)
	{
		GEditor->UnregisterForUndo(this);
	}
	if (GLiveMocaraWindow.HasSameObject(this))
	{
		GLiveMocaraWindow.Reset();
	}
}

bool SMocaraWindow::ExportLatestToFbx(const FString& Filename, FString& OutError)
{
	// Prefer the retargeted clip: that is the one on the character people actually use.
	UAnimSequence* Sequence = LastTargetSequence.IsValid() ? LastTargetSequence.Get() : LastSomaSequence.Get();
	return ExportSequenceToFbx(Sequence, Filename, OutError);
}

bool SMocaraWindow::LoadClipFromFile(const FString& BvhPath)
{
	FMocaraJobState Job;
	Job.JobId = FPaths::GetBaseFilename(BvhPath);
	Job.BvhPath = BvhPath;
	Job.Status = TEXT("done");
	return ImportAndRetarget(Job);
}

void SMocaraWindow::Construct(const FArguments& InArgs)
{
	PoseEditState.Reset(NewObject<UMocaraPoseEditState>());
	PoseEditState->SetFlags(RF_Transactional);
	if (GEditor)
	{
		GEditor->RegisterForUndo(this);
	}
	FMocaraSidecarLauncher::Get().EnsureStarted();
	GLiveMocaraWindow = SharedThis(this);

	// Check the WSL/Kimodo prerequisites the first time the tab is opened, so a machine
	// that is missing something says so here instead of failing at Generate time.
	if (!FMocaraSidecarLauncher::Get().HasDoctorRun())
	{
		FMocaraSidecarLauncher::Get().RunDoctorAsync();
	}
	RegisterActiveTimer(0.4f, FWidgetActiveTimerDelegate::CreateSP(this, &SMocaraWindow::OnPoll));
	RegisterActiveTimer(1.f / 30.f, FWidgetActiveTimerDelegate::CreateSP(this, &SMocaraWindow::OnPlaybackTick));

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 8, 8, 0)
		[
			SNew(STextBlock).Text(LOCTEXT("Title", "Mocara")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 16))
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 4, 8, 4)
		[
			SNew(STextBlock)
			.AutoWrapText(true)
			.Text_Lambda([] { return FText::FromString(FMocaraSidecarLauncher::Get().GetStatusText()); })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 4)
		[
			SNew(SBorder)
			.Padding(6)
			.Visibility_Lambda([]
			{
				// Only intrude when something is actually wrong, or while checking.
				const FMocaraSidecarLauncher& Launcher = FMocaraSidecarLauncher::Get();
				const bool bShow = Launcher.IsDoctorRunning() || Launcher.IsSetupRunning()
					|| Launcher.GetProblemCount() > 0;
				return bShow ? EVisibility::Visible : EVisibility::Collapsed;
			})
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.ColorAndOpacity(FSlateColor(FLinearColor(1.f, 0.75f, 0.25f)))
					.Text_Lambda([]
					{
						const FMocaraSidecarLauncher& Launcher = FMocaraSidecarLauncher::Get();
						if (Launcher.IsSetupRunning())
						{
							return FText::FromString(Launcher.GetSetupStatus());
						}
						if (Launcher.IsDoctorRunning())
						{
							return LOCTEXT("PreflightRunning", "Checking Kimodo prerequisites...");
						}
						FString Text;
						for (const FMocaraCheck& Check : Launcher.GetChecks())
						{
							if (Check.IsProblem())
							{
								Text += FString::Printf(TEXT("%s: %s\n"), *Check.Key, *Check.Detail);
							}
						}
						return FText::FromString(Text.TrimEnd());
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0).VAlign(VAlign_Center)
				[
					SNew(SButton)
					.Text(LOCTEXT("RunSetup", "Run Setup"))
					.ToolTipText(LOCTEXT("RunSetupTip", "Install the Kimodo sidecar prerequisites in WSL. Takes several minutes."))
					.IsEnabled_Lambda([]
					{
						const FMocaraSidecarLauncher& Launcher = FMocaraSidecarLauncher::Get();
						return !Launcher.IsSetupRunning() && !Launcher.IsDoctorRunning();
					})
					.OnClicked_Lambda([]
					{
						FMocaraSidecarLauncher::Get().BeginSetup();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0).VAlign(VAlign_Center)
				[
					SNew(SButton)
					.Text(LOCTEXT("Recheck", "Re-check"))
					.IsEnabled_Lambda([]
					{
						const FMocaraSidecarLauncher& Launcher = FMocaraSidecarLauncher::Get();
						return !Launcher.IsSetupRunning() && !Launcher.IsDoctorRunning();
					})
					.OnClicked_Lambda([]
					{
						FMocaraSidecarLauncher::Get().RunDoctorAsync();
						return FReply::Handled();
					})
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("Generate", "Generate"))
				.IsEnabled_Lambda([this] { return !bPolling && !bWaitingForSidecar && !bSubmitInFlight; })
				.OnClicked(this, &SMocaraWindow::OnGenerate)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("ExportFbx", "Export FBX"))
				.ToolTipText(LOCTEXT("ExportFbxTip", "Write the retargeted clip out as FBX for another project or DCC."))
				.IsEnabled_Lambda([this] { return LastTargetSequence.IsValid() || LastSomaSequence.IsValid(); })
				.OnClicked(this, &SMocaraWindow::OnExportFbx)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton)
					.Text(LOCTEXT("KeyPose", "Key pose"))
					.ToolTipText(LOCTEXT("KeyPoseTip", "Create or update the selected bone's pose at the playhead."))
					.OnClicked(this, &SMocaraWindow::OnKeyPose)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton)
					.Text(LOCTEXT("DeleteKey", "Delete key"))
					.ToolTipText(LOCTEXT("DeleteKeyTip", "Delete the selected pose key (Delete or Backspace)."))
					.IsEnabled_Lambda([this] { return GetPoseKeys().IsValidIndex(SelectedKeyIndex); })
					.OnClicked(this, &SMocaraWindow::OnDeleteSelectedKey)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton).Text(LOCTEXT("DuplicateKey", "Duplicate")).ToolTipText(LOCTEXT("DuplicateKeyTip", "Duplicate the selected key one frame later (Ctrl+D)."))
				.IsEnabled_Lambda([this] { return GetPoseKeys().IsValidIndex(SelectedKeyIndex) && CurrentFrame < TimelineFrameCount() - 1; })
				.OnClicked(this, &SMocaraWindow::OnDuplicateSelectedKey)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton).Text(LOCTEXT("CopyKey", "Copy")).ToolTipText(LOCTEXT("CopyKeyTip", "Copy the selected pose key (Ctrl+C)."))
				.IsEnabled_Lambda([this] { return GetPoseKeys().IsValidIndex(SelectedKeyIndex); })
				.OnClicked(this, &SMocaraWindow::OnCopySelectedKey)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton).Text(LOCTEXT("PasteKey", "Paste")).ToolTipText(LOCTEXT("PasteKeyTip", "Paste the copied pose key at the playhead (Ctrl+V)."))
				.IsEnabled_Lambda([this] { return CopiedPoseKey.IsSet(); })
				.OnClicked(this, &SMocaraWindow::OnPasteKey)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton)
				.Text_Lambda([this] { return bPlaying ? LOCTEXT("Pause", "Pause") : LOCTEXT("Play", "Play"); })
				.OnClicked(this, &SMocaraWindow::OnTogglePlay)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton).Text(LOCTEXT("Stop", "Stop")).OnClicked(this, &SMocaraWindow::OnStop)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton)
					.Text(LOCTEXT("Polish", "Apply local AutoPose"))
					.ToolTipText(LOCTEXT("PolishTip", "Bake the keyed pose intervals into the current target animation."))
					.IsEnabled_Lambda([this]
					{
						return (LastTargetSequence.IsValid() || LastSomaSequence.IsValid())
							&& !GetPoseKeys().IsEmpty();
					})
					.OnClicked(this, &SMocaraWindow::OnApplyAutoPose)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("Regen", "Regenerate with constraints"))
				.IsEnabled_Lambda([this]
				{
					return LastSomaSequence.IsValid() && !GetPoseKeys().IsEmpty()
						&& !bPolling && !bWaitingForSidecar && !bSubmitInFlight;
				})
				.OnClicked(this, &SMocaraWindow::OnRegenerateWithConstraints)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
			[
				SNew(SButton)
				.Text_Lambda([this]
				{
					return (Viewport.IsValid() && Viewport->IsShowingBones())
						? LOCTEXT("HideBones", "Hide Bones")
						: LOCTEXT("ShowBones", "Show Bones");
				})
				.OnClicked_Lambda([this]
				{
					if (Viewport.IsValid())
					{
						Viewport->SetShowBones(!Viewport->IsShowingBones());
					}
					return FReply::Handled();
				})
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton).Text(LOCTEXT("Focus", "Focus")).OnClicked_Lambda([this]
				{
					if (Viewport.IsValid())
					{
						Viewport->FocusOnPreview();
					}
					return FReply::Handled();
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 4, 8, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1)
			[
				SAssignNew(PromptBox, SEditableTextBox).Text(FText::FromString(Request.Prompt))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0).VAlign(VAlign_Center)
			[
				SNew(SSpinBox<float>)
				.Value_Lambda([this] { return Request.DurationSeconds; })
				.OnValueChanged_Lambda([this](float V) { Request.DurationSeconds = V; TimelineFrames = FMath::Max(1, FMath::RoundToInt(V * TimelineFps)); })
				.MinValue(0.5f).MaxValue(30.f)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("Seconds", "sec"))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 0, 0)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this] { return Request.bInPlace ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState S) { Request.bInPlace = S == ECheckBoxState::Checked; })
				[
					SNew(STextBlock).Text(LOCTEXT("InPlace", "In-place"))
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(SCheckBox)
				.IsChecked_Lambda([this] { return Request.bUseSeed ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
				.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { Request.bUseSeed = State == ECheckBoxState::Checked; })
				[
					SNew(STextBlock).Text(LOCTEXT("UseSeed", "Seed"))
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 12, 0).VAlign(VAlign_Center)
			[
				SNew(SSpinBox<int32>)
				.MinDesiredWidth(82.f)
				.MinValue(0).MaxValue(MAX_int32)
				.IsEnabled_Lambda([this] { return Request.bUseSeed; })
				.Value_Lambda([this] { return Request.Seed; })
				.OnValueChanged_Lambda([this](int32 Value) { Request.Seed = FMath::Max(0, Value); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("CandidateCount", "Variations"))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 12, 0).VAlign(VAlign_Center)
			[
				SNew(SSpinBox<int32>)
				.MinDesiredWidth(42.f)
				.MinValue(1).MaxValue(4)
				.Value_Lambda([this] { return Request.CandidateCount; })
				.OnValueChanged_Lambda([this](int32 Value) { Request.CandidateCount = FMath::Clamp(Value, 1, 4); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("TextGuidance", "Text"))
				.ToolTipText(LOCTEXT("TextGuidanceTip", "How strongly Kimodo follows the text prompt. Default: 2.0."))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 12, 0).VAlign(VAlign_Center)
			[
				SNew(SSpinBox<float>)
				.MinDesiredWidth(52.f)
				.MinValue(0.f).MaxValue(10.f).Delta(0.1f)
				.Value_Lambda([this] { return Request.TextGuidance; })
				.OnValueChanged_Lambda([this](float Value) { Request.TextGuidance = FMath::Clamp(Value, 0.f, 10.f); })
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("ConstraintGuidance", "Constraint"))
				.ToolTipText(LOCTEXT("ConstraintGuidanceTip", "How strongly Kimodo follows pose and grip constraints. Default: 2.0."))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0).VAlign(VAlign_Center)
			[
				SNew(SSpinBox<float>)
				.MinDesiredWidth(52.f)
				.MinValue(0.f).MaxValue(10.f).Delta(0.1f)
				.Value_Lambda([this] { return Request.ConstraintGuidance; })
				.OnValueChanged_Lambda([this](float Value) { Request.ConstraintGuidance = FMath::Clamp(Value, 0.f, 10.f); })
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("TwoHandGrip", "Two-Hand Grip + Regenerate"))
				.ToolTipText(LOCTEXT("TwoHandGripTip", "Keep the generated two-hand center path and item spacing while constraining both wrists to move together."))
				.IsEnabled_Lambda([this]
				{
					return LastSomaSequence.IsValid() && !bPolling && !bWaitingForSidecar && !bSubmitInFlight;
				})
				.OnClicked(this, &SMocaraWindow::OnRegenerateWithTwoHandGrip)
			]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(LOCTEXT("Candidate", "Candidate"))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 6, 0).VAlign(VAlign_Center)
			[
				SNew(SSpinBox<int32>)
				.MinDesiredWidth(48.f)
				.MinValue(1).MaxValue(4)
				.Value_Lambda([this] { return SelectedCandidateIndex + 1; })
				.OnValueChanged_Lambda([this](int32 Value) { SelectedCandidateIndex = FMath::Clamp(Value - 1, 0, 3); })
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 12, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("LoadCandidate", "Load"))
				.IsEnabled_Lambda([this]
				{
					return ActiveJob.Artifacts.IsValidIndex(SelectedCandidateIndex)
						&& !bPolling && !bSubmitInFlight && !bImportPending;
				})
				.OnClicked(this, &SMocaraWindow::OnLoadCandidate)
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.AutoWrapText(true)
				.ColorAndOpacity(FSlateColor(FLinearColor(0.65f, 0.75f, 0.85f)))
				.Text_Lambda([this]
				{
					if (ActiveJob.Prompt.IsEmpty())
					{
						return LOCTEXT("ProvenancePending", "Each result saves its exact prompt and settings.");
					}
					const FString SeedText = ActiveJob.bHasSeed
						? FString::Printf(TEXT("seed %d"), ActiveJob.Seed)
						: TEXT("unseeded");
					return FText::FromString(FString::Printf(
						TEXT("%s | %d candidates | text %.1f | constraint %.1f | %s"),
						*SeedText,
						ActiveJob.CandidateCount,
						ActiveJob.TextGuidance,
						ActiveJob.ConstraintGuidance,
						ActiveJob.TextEncoderPrecision.IsEmpty() ? TEXT("precision unknown") : *ActiveJob.TextEncoderPrecision));
				})
			]
		]
		+ SVerticalBox::Slot().FillHeight(1).Padding(8, 0, 8, 4)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(2.f).Padding(0, 0, 6, 0)
			[
				SNew(SBorder).Padding(2)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight().Padding(2, 2, 2, 4)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
						[
							SNew(SButton)
							.Text(LOCTEXT("RotateMode", "Rotate"))
							.ButtonColorAndOpacity_Lambda([this]
							{
								return Viewport.IsValid() && !Viewport->IsTranslationMode()
									? FLinearColor(0.15f, 0.55f, 0.2f) : FLinearColor(0.12f, 0.12f, 0.12f);
							})
							.OnClicked_Lambda([this]
							{
								if (Viewport.IsValid())
								{
									Viewport->SetTranslationMode(false);
								}
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
						[
							SNew(SButton)
							.Text(LOCTEXT("MoveMode", "Move / IK"))
							.ToolTipText(LOCTEXT("MoveModeTooltip", "Move hips directly, or position hand and foot IK targets."))
							.IsEnabled_Lambda([this]
							{
								return Viewport.IsValid() && Viewport->CanTranslateSelectedBone();
							})
							.ButtonColorAndOpacity_Lambda([this]
							{
								return Viewport.IsValid() && Viewport->IsTranslationMode()
									? FLinearColor(0.15f, 0.55f, 0.2f) : FLinearColor(0.12f, 0.12f, 0.12f);
							})
							.OnClicked_Lambda([this]
							{
								if (Viewport.IsValid())
								{
									Viewport->SetTranslationMode(true);
								}
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
						[
							SNew(SButton)
							.Text_Lambda([this]
							{
								return Viewport.IsValid() && Viewport->IsUsingLocalCoordinates()
									? LOCTEXT("LocalCoordinates", "Local")
									: LOCTEXT("WorldCoordinates", "World");
							})
							.ToolTipText(LOCTEXT("CoordinateTooltip", "Toggle the rotation gizmo between bone-local and world coordinates."))
							.OnClicked_Lambda([this]
							{
								if (Viewport.IsValid())
								{
									Viewport->SetUseLocalCoordinates(!Viewport->IsUsingLocalCoordinates());
								}
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
						[
							SNew(SButton).Text(LOCTEXT("FrameBone", "Frame Bone"))
							.OnClicked_Lambda([this]
							{
								if (Viewport.IsValid())
								{
									Viewport->FocusOnSelectedBone();
								}
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
						[
							SNew(SButton)
							.Text(LOCTEXT("ViewFront", "Front"))
							.ToolTipText(LOCTEXT("ViewFrontTip", "Look straight down the character's facing axis."))
							.OnClicked_Lambda([this]
							{
								if (Viewport.IsValid()) { Viewport->SetViewPreset(EMocaraViewPreset::Front); }
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
						[
							SNew(SButton)
							.Text(LOCTEXT("ViewSide", "Side"))
							.OnClicked_Lambda([this]
							{
								if (Viewport.IsValid()) { Viewport->SetViewPreset(EMocaraViewPreset::Side); }
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
						[
							SNew(SButton)
							.Text(LOCTEXT("ViewPersp", "3/4"))
							.OnClicked_Lambda([this]
							{
								if (Viewport.IsValid()) { Viewport->SetViewPreset(EMocaraViewPreset::Perspective); }
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 4, 0)
						[
							SNew(SButton).Text(LOCTEXT("ResetView", "Reset View"))
							.OnClicked_Lambda([this]
							{
								if (Viewport.IsValid())
								{
									Viewport->ResetView();
								}
								return FReply::Handled();
							})
						]
						+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
						[
							SNew(SCheckBox)
							.IsChecked_Lambda([this]
							{
								return Viewport.IsValid() && Viewport->IsFollowingCharacter()
									? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
							})
							.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
							{
								if (Viewport.IsValid())
								{
									Viewport->SetFollowCharacter(State == ECheckBoxState::Checked);
								}
							})
							[
								SNew(STextBlock).Text(LOCTEXT("FollowCharacter", "Follow"))
							]
						]
					]
					+ SVerticalBox::Slot().FillHeight(1.f)
					[
						SAssignNew(Viewport, SMocaraViewport)
							.OnBoneSelected(this, &SMocaraWindow::SelectBone)
							.OnBoneTransformChanged(this, &SMocaraWindow::OnViewportTransformChanged)
							.OnManipulationStarted(this, &SMocaraWindow::OnViewportManipulationStarted)
							.OnManipulationEnded(this, &SMocaraWindow::OnViewportManipulationEnded)
					]
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f)
			[
			SNew(SScrollBox)
			+ SScrollBox::Slot()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
				[
					SNew(STextBlock).Text(LOCTEXT("PoseTitle", "AutoPose")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
				[
					MakeBoneRow({TEXT("Hips"), TEXT("Spine1"), TEXT("Spine2"), TEXT("Chest")})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
				[
					MakeBoneRow({TEXT("Neck1"), TEXT("Neck2"), TEXT("Head"), TEXT("Jaw")})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
				[
					MakeBoneRow({TEXT("LeftShoulder"), TEXT("LeftArm"), TEXT("LeftForeArm"), TEXT("LeftHand")})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
				[
					MakeBoneRow({TEXT("RightShoulder"), TEXT("RightArm"), TEXT("RightForeArm"), TEXT("RightHand")})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2)
				[
					MakeBoneRow({TEXT("LeftLeg"), TEXT("LeftShin"), TEXT("LeftFoot"), TEXT("LeftToeBase")})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 8)
				[
					MakeBoneRow({TEXT("RightLeg"), TEXT("RightShin"), TEXT("RightFoot"), TEXT("RightToeBase")})
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 3, 0)
					[
						SNew(STextBlock).Text(LOCTEXT("PitchLabel", "Pitch"))
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 6, 0)
					[
						SNew(SSpinBox<float>)
						.Value_Lambda([this] { return PoseRotation.Pitch; })
						.OnBeginSliderMovement(this, &SMocaraWindow::BeginPoseControlTransaction)
						.OnEndSliderMovement_Lambda([this](float) { EndPoseControlTransaction(); })
						.OnValueChanged_Lambda([this](float V) { PoseRotation.Pitch = V; OnPoseControlsChanged(); })
						.MinValue(-180.f).MaxValue(180.f).Delta(1.f)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 3, 0)
					[
						SNew(STextBlock).Text(LOCTEXT("YawLabel", "Yaw"))
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 6, 0)
					[
						SNew(SSpinBox<float>)
						.Value_Lambda([this] { return PoseRotation.Yaw; })
						.OnBeginSliderMovement(this, &SMocaraWindow::BeginPoseControlTransaction)
						.OnEndSliderMovement_Lambda([this](float) { EndPoseControlTransaction(); })
						.OnValueChanged_Lambda([this](float V) { PoseRotation.Yaw = V; OnPoseControlsChanged(); })
						.MinValue(-180.f).MaxValue(180.f).Delta(1.f)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 3, 0)
					[
						SNew(STextBlock).Text(LOCTEXT("RollLabel", "Roll"))
					]
					+ SHorizontalBox::Slot().FillWidth(1.f)
					[
						SNew(SSpinBox<float>)
						.Value_Lambda([this] { return PoseRotation.Roll; })
						.OnBeginSliderMovement(this, &SMocaraWindow::BeginPoseControlTransaction)
						.OnEndSliderMovement_Lambda([this](float) { EndPoseControlTransaction(); })
						.OnValueChanged_Lambda([this](float V) { PoseRotation.Roll = V; OnPoseControlsChanged(); })
						.MinValue(-180.f).MaxValue(180.f).Delta(1.f)
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
				[
					SNew(SHorizontalBox)
					.IsEnabled_Lambda([this] { return FMocaraPoseEditing::CanTranslateBone(SelectedBone); })
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 3, 0)
					[
						SNew(STextBlock).Text(LOCTEXT("TranslateXLabel", "Move X"))
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 6, 0)
					[
						SNew(SSpinBox<float>)
						.Value_Lambda([this] { return PoseTranslation.X; })
						.OnBeginSliderMovement(this, &SMocaraWindow::BeginPoseControlTransaction)
						.OnEndSliderMovement_Lambda([this](float) { EndPoseControlTransaction(); })
						.OnValueChanged_Lambda([this](float V) { PoseTranslation.X = V; OnPoseControlsChanged(); })
						.MinValue(-500.f).MaxValue(500.f).Delta(1.f)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 3, 0)
					[
						SNew(STextBlock).Text(LOCTEXT("TranslateYLabel", "Y"))
					]
					+ SHorizontalBox::Slot().FillWidth(1.f).Padding(0, 0, 6, 0)
					[
						SNew(SSpinBox<float>)
						.Value_Lambda([this] { return PoseTranslation.Y; })
						.OnBeginSliderMovement(this, &SMocaraWindow::BeginPoseControlTransaction)
						.OnEndSliderMovement_Lambda([this](float) { EndPoseControlTransaction(); })
						.OnValueChanged_Lambda([this](float V) { PoseTranslation.Y = V; OnPoseControlsChanged(); })
						.MinValue(-500.f).MaxValue(500.f).Delta(1.f)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 3, 0)
					[
						SNew(STextBlock).Text(LOCTEXT("TranslateZLabel", "Z"))
					]
					+ SHorizontalBox::Slot().FillWidth(1.f)
					[
						SNew(SSpinBox<float>)
						.Value_Lambda([this] { return PoseTranslation.Z; })
						.OnBeginSliderMovement(this, &SMocaraWindow::BeginPoseControlTransaction)
						.OnEndSliderMovement_Lambda([this](float) { EndPoseControlTransaction(); })
						.OnValueChanged_Lambda([this](float V) { PoseTranslation.Z = V; OnPoseControlsChanged(); })
						.MinValue(-500.f).MaxValue(500.f).Delta(1.f)
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 0, 0, 4)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 3, 0)
					[
						SNew(STextBlock).Text(LOCTEXT("EaseInLabel", "Ease In"))
						.ToolTipText(LOCTEXT("EaseInTooltip", "Frames used to blend from the generated animation into the keyed pose."))
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SSpinBox<int32>)
						.Value_Lambda([this] { return EaseInFrames; })
						.OnBeginSliderMovement(this, &SMocaraWindow::BeginPoseControlTransaction)
						.OnEndSliderMovement_Lambda([this](int32) { EndPoseControlTransaction(); })
						.OnValueChanged_Lambda([this](int32 V) { EaseInFrames = V; OnPoseControlsChanged(); })
						.MinValue(0).MaxValue(300).MinDesiredWidth(48.f)
						.ToolTipText(LOCTEXT("EaseInSpinTooltip", "Lengthen this to make the pose enter more gradually."))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 3, 0)
					[
						SNew(STextBlock).Text(LOCTEXT("HoldLabel", "Hold"))
						.ToolTipText(LOCTEXT("HoldTooltip", "Frames the keyed pose remains at full strength."))
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SSpinBox<int32>)
						.Value_Lambda([this] { return HoldFrames; })
						.OnBeginSliderMovement(this, &SMocaraWindow::BeginPoseControlTransaction)
						.OnEndSliderMovement_Lambda([this](int32) { EndPoseControlTransaction(); })
						.OnValueChanged_Lambda([this](int32 V) { HoldFrames = V; OnPoseControlsChanged(); })
						.MinValue(1).MaxValue(300).MinDesiredWidth(48.f)
						.ToolTipText(LOCTEXT("HoldSpinTooltip", "Lengthen this to keep the key at full strength for more frames."))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10, 0, 3, 0)
					[
						SNew(STextBlock).Text(LOCTEXT("EaseOutLabel", "Ease Out"))
						.ToolTipText(LOCTEXT("EaseOutTooltip", "Frames used to blend from the keyed pose back to the generated animation."))
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SSpinBox<int32>)
						.Value_Lambda([this] { return EaseOutFrames; })
						.OnBeginSliderMovement(this, &SMocaraWindow::BeginPoseControlTransaction)
						.OnEndSliderMovement_Lambda([this](int32) { EndPoseControlTransaction(); })
						.OnValueChanged_Lambda([this](int32 V) { EaseOutFrames = V; OnPoseControlsChanged(); })
						.MinValue(0).MaxValue(300).MinDesiredWidth(48.f)
						.ToolTipText(LOCTEXT("EaseOutSpinTooltip", "Lengthen this to release the pose more gradually."))
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 0, 0).VAlign(VAlign_Center)
					[
						SNew(SCheckBox)
						.IsEnabled_Lambda([this]
						{
							const EMocaraPoseLane Lane = FMocaraPoseEditing::ClassifyBoneLane(SelectedBone);
							return Lane == EMocaraPoseLane::LeftHand || Lane == EMocaraPoseLane::RightHand
								|| Lane == EMocaraPoseLane::LeftFoot || Lane == EMocaraPoseLane::RightFoot;
						})
						.IsChecked_Lambda([this] { return bUseTwoBoneIK ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
						{
							bUseTwoBoneIK = State == ECheckBoxState::Checked;
							OnPoseControlsChanged();
						})
						[
							SNew(STextBlock).Text(LOCTEXT("UseIk", "Two-bone IK"))
						]
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 4)
				[
					SNew(STextBlock)
					.AutoWrapText(true)
					.Text_Lambda([this]
					{
						return PoseSummaryText();
					})
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return FText::FromString(StatusText); })
				]
			]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(8, 0, 8, 8)
		[
			SNew(SBorder)
			.Padding(4)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(2, 0, 2, 4)
				[
					SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.Text(LOCTEXT(
							"TimelineLegend",
							"Dim = Ease In / Out   |   Solid = Hold   |   Vertical line = keyed pose   |   Drag the body to move; drag a handle to resize"))
				]
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SMocaraTimeline)
					.Keys(&PoseEditState->Keys)
					.ToolTipText(LOCTEXT(
						"TimelineKeyHandlesTooltip",
						"Drag the interval body to move the key. Drag the lower-left handle for Ease In, the upper-right handle of the bright region for Hold, and the lower-right handle for Ease Out. Left/Right nudges the selected key; Ctrl+D duplicates it."))
					.NumFrames_Lambda([this] { return TimelineFrameCount(); })
					.Playhead_Lambda([this] { return CurrentFrame; })
					.SelectedKey_Lambda([this] { return SelectedKeyIndex; })
					.OnPlayheadChanged_Lambda([this](int32 Frame) { SetPlayhead(Frame); })
					.OnKeySelected_Lambda([this](int32 Index) { SelectKey(Index); })
					.OnKeyMoveStarted(this, &SMocaraWindow::OnTimelineKeyMoveStarted)
					.OnKeyMoved(this, &SMocaraWindow::OnTimelineKeyMoved)
					.OnKeyResized(this, &SMocaraWindow::OnTimelineKeyResized)
					.OnKeyMoveEnded(this, &SMocaraWindow::OnTimelineKeyMoveEnded)
				]
			]
		]
	];
}

TSharedRef<SWidget> SMocaraWindow::MakeBoneButton(FName Bone)
{
	return SNew(SButton)
		.Text(FText::FromString(FMocaraPoseEditing::DisplayLabel(Bone)))
		.ToolTipText(FText::Format(
			LOCTEXT("BoneIdentifierTooltip", "Bone: {0}"), FText::FromName(Bone)))
		.ButtonColorAndOpacity_Lambda([this, Bone] { return FSlateColor(BoneButtonTint(Bone)); })
		.OnClicked_Lambda([this, Bone]
		{
			SelectBone(Bone);
			return FReply::Handled();
		});
}

TSharedRef<SWidget> SMocaraWindow::MakeBoneRow(const TArray<FName>& Bones)
{
	TSharedRef<SHorizontalBox> Row = SNew(SHorizontalBox);
	for (const FName Bone : Bones)
	{
		Row->AddSlot().AutoWidth().Padding(0, 0, 4, 0)[MakeBoneButton(Bone)];
	}
	return Row;
}

FLinearColor SMocaraWindow::BoneButtonTint(FName Bone) const
{
	return (SelectedBone == Bone || FMocaraBoneMap::ToSoma(SelectedBone) == Bone)
		? FLinearColor(0.15f, 0.55f, 0.2f)
		: FLinearColor(0.12f, 0.12f, 0.12f);
}

int32 SMocaraWindow::TimelineFrameCount() const
{
	return FMath::Max(1, TimelineFrames);
}

TArray<FMocaraPoseKey>& SMocaraWindow::GetPoseKeys()
{
	check(PoseEditState.IsValid());
	return PoseEditState->Keys;
}

const TArray<FMocaraPoseKey>& SMocaraWindow::GetPoseKeys() const
{
	check(PoseEditState.IsValid());
	return PoseEditState->Keys;
}

void SMocaraWindow::SelectBone(FName Bone)
{
	SelectedBone = Bone;
	if (Viewport.IsValid())
	{
		const FName Resolved = Viewport->SetSelectedBone(SelectedBone, FRotator::ZeroRotator, FVector::ZeroVector);
		if (!Resolved.IsNone())
		{
			SelectedBone = Resolved;
		}
	}
	RefreshSelectionFromKeys();
}

void SMocaraWindow::SetPlayhead(int32 Frame)
{
	CurrentFrame = FMath::Clamp(Frame, 0, TimelineFrameCount() - 1);
	if (Viewport.IsValid())
	{
		Viewport->SetPlayheadFrame(CurrentFrame, TimelineFps);
	}
	RefreshSelectionFromKeys();
}

void SMocaraWindow::SelectKey(int32 Index)
{
	SelectedKeyIndex = Index;
	if (GetPoseKeys().IsValidIndex(Index))
	{
		CurrentFrame = GetPoseKeys()[Index].Frame;
		SelectedBone = GetPoseKeys()[Index].BoneName;
		PoseRotation = GetPoseKeys()[Index].RotationOffset;
		PoseTranslation = GetPoseKeys()[Index].TranslationOffset;
		EaseInFrames = GetPoseKeys()[Index].EaseInFrames;
		HoldFrames = GetPoseKeys()[Index].HoldFrames;
		EaseOutFrames = GetPoseKeys()[Index].EaseOutFrames;
		bUseTwoBoneIK = GetPoseKeys()[Index].bUseTwoBoneIK;
		if (Viewport.IsValid())
		{
			Viewport->SetPlayheadFrame(CurrentFrame, TimelineFps);
			Viewport->SetPoseKeys(GetPoseKeys());
			Viewport->SetSelectedBone(SelectedBone, PoseRotation, PoseTranslation);
		}
	}
}

void SMocaraWindow::RefreshSelectionFromKeys()
{
	SelectedKeyIndex = FMocaraPoseEditing::FindKey(GetPoseKeys(), CurrentFrame, SelectedBone);
	if (GetPoseKeys().IsValidIndex(SelectedKeyIndex))
	{
		const FMocaraPoseKey& Key = GetPoseKeys()[SelectedKeyIndex];
		PoseRotation = Key.RotationOffset;
		PoseTranslation = Key.TranslationOffset;
		EaseInFrames = Key.EaseInFrames;
		HoldFrames = Key.HoldFrames;
		EaseOutFrames = Key.EaseOutFrames;
		bUseTwoBoneIK = Key.bUseTwoBoneIK;
	}
	else
	{
		PoseRotation = FRotator::ZeroRotator;
		PoseTranslation = FVector::ZeroVector;
		const EMocaraPoseLane Lane = FMocaraPoseEditing::ClassifyBoneLane(SelectedBone);
		bUseTwoBoneIK = Lane == EMocaraPoseLane::LeftHand || Lane == EMocaraPoseLane::RightHand
			|| Lane == EMocaraPoseLane::LeftFoot || Lane == EMocaraPoseLane::RightFoot;
	}

	if (Viewport.IsValid())
	{
		Viewport->SetPoseKeys(GetPoseKeys());
		Viewport->SetSelectedBone(SelectedBone, PoseRotation, PoseTranslation);
	}
}

void SMocaraWindow::OnViewportTransformChanged(FRotator Rotation, FVector Translation)
{
	PoseRotation = Rotation;
	PoseTranslation = Translation;
	if (FMocaraPoseEditing::CanTranslateBone(SelectedBone))
	{
		const EMocaraPoseLane Lane = FMocaraPoseEditing::ClassifyBoneLane(SelectedBone);
		bUseTwoBoneIK = Lane != EMocaraPoseLane::RootPath;
	}
	SelectedKeyIndex = FMocaraPoseEditing::UpsertTransformKey(
		GetPoseKeys(), CurrentFrame, SelectedBone, PoseRotation, PoseTranslation,
		EaseInFrames, HoldFrames, EaseOutFrames, bUseTwoBoneIK);
	if (Viewport.IsValid())
	{
		Viewport->SetPoseKeys(GetPoseKeys());
	}
	SetStatus(FString::Printf(
		TEXT("Viewport keyed %s at frame %d"),
		*FMocaraPoseEditing::DisplayLabel(SelectedBone),
		CurrentFrame));
}

void SMocaraWindow::OnViewportManipulationStarted()
{
	if (bViewportTransactionActive || !PoseEditState.IsValid() || !GEditor)
	{
		return;
	}
	GEditor->BeginTransaction(LOCTEXT("RotateBoneTransaction", "Rotate Mocara Bone"));
	PoseEditState->Modify();
	bViewportTransactionActive = true;
}

void SMocaraWindow::OnViewportManipulationEnded()
{
	if (bViewportTransactionActive && GEditor)
	{
		GEditor->EndTransaction();
	}
	bViewportTransactionActive = false;
}

void SMocaraWindow::BeginPoseControlTransaction()
{
	if (bPoseControlTransactionActive || !PoseEditState.IsValid() || !GEditor)
	{
		return;
	}
	GEditor->BeginTransaction(LOCTEXT("EditPoseControlsTransaction", "Edit Mocara Pose Transform"));
	PoseEditState->Modify();
	bPoseControlTransactionActive = true;
}

void SMocaraWindow::EndPoseControlTransaction()
{
	if (bPoseControlTransactionActive && GEditor)
	{
		GEditor->EndTransaction();
	}
	bPoseControlTransactionActive = false;
}

void SMocaraWindow::OnPoseControlsChanged()
{
	const bool bOwnTransaction = !bPoseControlTransactionActive;
	if (bOwnTransaction)
	{
		BeginPoseControlTransaction();
	}
	SelectedKeyIndex = FMocaraPoseEditing::UpsertTransformKey(
		GetPoseKeys(), CurrentFrame, SelectedBone, PoseRotation, PoseTranslation,
		EaseInFrames, HoldFrames, EaseOutFrames, bUseTwoBoneIK);
	if (Viewport.IsValid())
	{
		Viewport->SetPoseKeys(GetPoseKeys());
		Viewport->SetSelectedBone(SelectedBone, PoseRotation, PoseTranslation);
	}
	if (bOwnTransaction)
	{
		EndPoseControlTransaction();
	}
}

void SMocaraWindow::PostUndo(bool bSuccess)
{
	if (bSuccess)
	{
		const int32 NearestKey = FMocaraPoseEditing::FindNearestKey(
			GetPoseKeys(), CurrentFrame, SelectedBone);
		if (GetPoseKeys().IsValidIndex(NearestKey))
		{
			SelectKey(NearestKey);
		}
		else
		{
			RefreshSelectionFromKeys();
		}
	}
}

bool SMocaraWindow::MatchesContext(
	const FTransactionContext& InContext,
	const TArray<TPair<UObject*, FTransactionObjectEvent>>& TransactionObjectContexts) const
{
	return TransactionObjectContexts.ContainsByPredicate([this](const TPair<UObject*, FTransactionObjectEvent>& Entry)
	{
		return Entry.Key == PoseEditState.Get();
	});
}

void SMocaraWindow::PostRedo(bool bSuccess)
{
	PostUndo(bSuccess);
}

void SMocaraWindow::SetStatus(const FString& Text)
{
	StatusText = Text;
}

FText SMocaraWindow::PoseSummaryText() const
{
	const FText BoneLabel = FText::FromString(FMocaraPoseEditing::DisplayLabel(SelectedBone));
	if (GetPoseKeys().IsValidIndex(SelectedKeyIndex))
	{
		const FMocaraPoseKey& Key = GetPoseKeys()[SelectedKeyIndex];
		return FText::Format(
			LOCTEXT("SelectedFrameFmt", "Frame {0} / {1}  |  {2}  |  Ease In {3} / Hold {4} / Ease Out {5}  |  {6} keys"),
			FText::AsNumber(CurrentFrame),
			FText::AsNumber(TimelineFrameCount() - 1),
			BoneLabel,
			FText::AsNumber(Key.EaseInFrames),
			FText::AsNumber(Key.HoldFrames),
			FText::AsNumber(Key.EaseOutFrames),
			FText::AsNumber(GetPoseKeys().Num()));
	}
	return FText::Format(
		LOCTEXT("UnkeyedFrameFmt", "Frame {0} / {1}  |  {2}  |  No key at playhead  |  {3} keys"),
		FText::AsNumber(CurrentFrame),
		FText::AsNumber(TimelineFrameCount() - 1),
		BoneLabel,
		FText::AsNumber(GetPoseKeys().Num()));
}

FReply SMocaraWindow::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.IsControlDown())
	{
		if (InKeyEvent.GetKey() == EKeys::C)
		{
			return OnCopySelectedKey();
		}
		if (InKeyEvent.GetKey() == EKeys::V)
		{
			return OnPasteKey();
		}
		if (InKeyEvent.GetKey() == EKeys::D)
		{
			return OnDuplicateSelectedKey();
		}
	}
	if (InKeyEvent.GetKey() == EKeys::Delete || InKeyEvent.GetKey() == EKeys::BackSpace)
	{
		return OnDeleteSelectedKey();
	}
	if (InKeyEvent.GetKey() == EKeys::SpaceBar)
	{
		return OnTogglePlay();
	}
	if ((InKeyEvent.GetKey() == EKeys::Left || InKeyEvent.GetKey() == EKeys::Right)
		&& GetPoseKeys().IsValidIndex(SelectedKeyIndex))
	{
		const FScopedTransaction Transaction(LOCTEXT("NudgePoseKeyTransaction", "Nudge Mocara Pose Key"));
		PoseEditState->Modify();
		const int32 Delta = InKeyEvent.GetKey() == EKeys::Left ? -1 : 1;
		SelectedKeyIndex = FMocaraPoseEditing::MoveKey(
			GetPoseKeys(), SelectedKeyIndex,
			FMath::Clamp(GetPoseKeys()[SelectedKeyIndex].Frame + Delta, 0, TimelineFrameCount() - 1));
		SelectKey(SelectedKeyIndex);
		return FReply::Handled();
	}
	return SCompoundWidget::OnKeyDown(MyGeometry, InKeyEvent);
}

FReply SMocaraWindow::OnExportFbx()
{
	UAnimSequence* Sequence = LastTargetSequence.IsValid() ? LastTargetSequence.Get() : LastSomaSequence.Get();
	if (!Sequence)
	{
		SetStatus(TEXT("Nothing to export yet."));
		return FReply::Handled();
	}

	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	if (!Desktop)
	{
		SetStatus(TEXT("No desktop platform available for the save dialog."));
		return FReply::Handled();
	}

	TArray<FString> Chosen;
	const void* ParentHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(SharedThis(this));
	const bool bPicked = Desktop->SaveFileDialog(
		ParentHandle,
		TEXT("Export animation as FBX"),
		FPaths::ProjectSavedDir(),
		Sequence->GetName() + TEXT(".fbx"),
		TEXT("FBX file|*.fbx"),
		EFileDialogFlags::None,
		Chosen);

	if (!bPicked || Chosen.Num() == 0)
	{
		return FReply::Handled();
	}

	FString Error;
	if (ExportLatestToFbx(Chosen[0], Error))
	{
		SetStatus(FString::Printf(TEXT("Exported %s"), *Chosen[0]));
	}
	else
	{
		SetStatus(Error);
	}
	return FReply::Handled();
}

FReply SMocaraWindow::OnKeyPose()
{
	const bool bUpdating = FMocaraPoseEditing::FindKey(GetPoseKeys(), CurrentFrame, SelectedBone) != INDEX_NONE;
	const FScopedTransaction Transaction(LOCTEXT("KeyPoseTransaction", "Key Mocara Pose"));
	PoseEditState->Modify();
	SelectedKeyIndex = FMocaraPoseEditing::UpsertTransformKey(
		GetPoseKeys(), CurrentFrame, SelectedBone, PoseRotation, PoseTranslation,
		EaseInFrames, HoldFrames, EaseOutFrames, bUseTwoBoneIK);
	if (Viewport.IsValid())
	{
		Viewport->SetPoseKeys(GetPoseKeys());
	}
	SetStatus(bUpdating
		? FString::Printf(TEXT("Updated %s at frame %d"), *FMocaraPoseEditing::DisplayLabel(SelectedBone), CurrentFrame)
		: FString::Printf(TEXT("Keyed %s at frame %d (%d keys)"), *FMocaraPoseEditing::DisplayLabel(SelectedBone), CurrentFrame, GetPoseKeys().Num()));
	return FReply::Handled();
}

FReply SMocaraWindow::OnDeleteSelectedKey()
{
	if (!GetPoseKeys().IsValidIndex(SelectedKeyIndex))
	{
		SetStatus(TEXT("No key selected. Click a green marker on the timeline."));
		return FReply::Handled();
	}
	const FScopedTransaction Transaction(LOCTEXT("DeletePoseKeyTransaction", "Delete Mocara Pose Key"));
	PoseEditState->Modify();
	const FMocaraPoseKey Removed = GetPoseKeys()[SelectedKeyIndex];
	GetPoseKeys().RemoveAt(SelectedKeyIndex);
	SelectedKeyIndex = INDEX_NONE;
	PoseRotation = FRotator::ZeroRotator;
	PoseTranslation = FVector::ZeroVector;
	if (Viewport.IsValid())
	{
		Viewport->SetPoseKeys(GetPoseKeys());
		Viewport->SetSelectedBone(SelectedBone, PoseRotation, PoseTranslation);
	}
	SetStatus(FString::Printf(
		TEXT("Deleted %s at frame %d"),
		*FMocaraPoseEditing::DisplayLabel(Removed.BoneName),
		Removed.Frame));
	return FReply::Handled();
}

FReply SMocaraWindow::OnCopySelectedKey()
{
	if (!GetPoseKeys().IsValidIndex(SelectedKeyIndex))
	{
		SetStatus(TEXT("No key selected to copy."));
		return FReply::Handled();
	}
	CopiedPoseKey = GetPoseKeys()[SelectedKeyIndex];
	SetStatus(FString::Printf(TEXT("Copied %s at frame %d"),
		*FMocaraPoseEditing::DisplayLabel(CopiedPoseKey->BoneName), CopiedPoseKey->Frame));
	return FReply::Handled();
}

FReply SMocaraWindow::OnPasteKey()
{
	if (!CopiedPoseKey.IsSet())
	{
		SetStatus(TEXT("Copy a pose key before pasting."));
		return FReply::Handled();
	}
	const FScopedTransaction Transaction(LOCTEXT("PastePoseKeyTransaction", "Paste Mocara Pose Key"));
	PoseEditState->Modify();
	const int32 Existing = FMocaraPoseEditing::FindKey(GetPoseKeys(), CurrentFrame, CopiedPoseKey->BoneName);
	if (Existing != INDEX_NONE)
	{
		GetPoseKeys()[Existing] = CopiedPoseKey.GetValue();
		GetPoseKeys()[Existing].Frame = CurrentFrame;
		SelectedKeyIndex = Existing;
	}
	else
	{
		FMocaraPoseKey Pasted = CopiedPoseKey.GetValue();
		Pasted.Frame = CurrentFrame;
		SelectedKeyIndex = GetPoseKeys().Add(Pasted);
	}
	SelectKey(SelectedKeyIndex);
	SetStatus(FString::Printf(
		TEXT("Pasted %s at frame %d"),
		*FMocaraPoseEditing::DisplayLabel(SelectedBone),
		CurrentFrame));
	return FReply::Handled();
}

FReply SMocaraWindow::OnDuplicateSelectedKey()
{
	if (!GetPoseKeys().IsValidIndex(SelectedKeyIndex))
	{
		SetStatus(TEXT("No key selected to duplicate."));
		return FReply::Handled();
	}
	const int32 SourceFrame = GetPoseKeys()[SelectedKeyIndex].Frame;
	if (SourceFrame >= TimelineFrameCount() - 1)
	{
		SetStatus(TEXT("Cannot duplicate a key past the final frame."));
		return FReply::Handled();
	}
	const FScopedTransaction Transaction(LOCTEXT("DuplicatePoseKeyTransaction", "Duplicate Mocara Pose Key"));
	PoseEditState->Modify();
	const int32 NewFrame = SourceFrame + 1;
	SelectedKeyIndex = FMocaraPoseEditing::DuplicateKey(GetPoseKeys(), SelectedKeyIndex, NewFrame);
	SelectKey(SelectedKeyIndex);
	SetStatus(FString::Printf(
		TEXT("Duplicated %s to frame %d"),
		*FMocaraPoseEditing::DisplayLabel(SelectedBone),
		CurrentFrame));
	return FReply::Handled();
}

void SMocaraWindow::OnTimelineKeyMoveStarted()
{
	if (bTimelineTransactionActive || !GEditor)
	{
		return;
	}
	GEditor->BeginTransaction(LOCTEXT("MovePoseKeyTransaction", "Edit Mocara Pose Interval"));
	PoseEditState->Modify();
	bTimelineTransactionActive = true;
}

int32 SMocaraWindow::OnTimelineKeyMoved(int32 KeyIndex, int32 NewFrame)
{
	const int32 MovedIndex = FMocaraPoseEditing::MoveKey(
		GetPoseKeys(), KeyIndex, FMath::Clamp(NewFrame, 0, TimelineFrameCount() - 1));
	if (GetPoseKeys().IsValidIndex(MovedIndex))
	{
		SelectKey(MovedIndex);
		SetStatus(FString::Printf(
			TEXT("Moved %s to frame %d"),
			*FMocaraPoseEditing::DisplayLabel(SelectedBone),
			CurrentFrame));
	}
	return MovedIndex;
}

int32 SMocaraWindow::OnTimelineKeyResized(
	int32 KeyIndex,
	EMocaraPoseIntervalHandle Handle,
	int32 NewFrame)
{
	if (!GetPoseKeys().IsValidIndex(KeyIndex))
	{
		return INDEX_NONE;
	}
	FMocaraPoseEditing::ResizeInterval(GetPoseKeys()[KeyIndex], Handle, NewFrame);
	SelectKey(KeyIndex);
	const FMocaraPoseKey& Key = GetPoseKeys()[KeyIndex];
	SetStatus(FString::Printf(
		TEXT("Resized %s: ease in %d | hold %d | ease out %d"),
		*FMocaraPoseEditing::DisplayLabel(Key.BoneName),
		Key.EaseInFrames,
		Key.HoldFrames,
		Key.EaseOutFrames));
	return KeyIndex;
}

void SMocaraWindow::OnTimelineKeyMoveEnded()
{
	if (bTimelineTransactionActive && GEditor)
	{
		GEditor->EndTransaction();
	}
	bTimelineTransactionActive = false;
}

FReply SMocaraWindow::OnTogglePlay()
{
	bPlaying = !bPlaying;
	if (Viewport.IsValid())
	{
		Viewport->SetPlaying(bPlaying);
	}
	return FReply::Handled();
}

FReply SMocaraWindow::OnStop()
{
	bPlaying = false;
	if (Viewport.IsValid())
	{
		Viewport->SetPlaying(false);
	}
	SetPlayhead(0);
	return FReply::Handled();
}

EActiveTimerReturnType SMocaraWindow::OnPlaybackTick(double, float)
{
	if (bPlaying)
	{
		SetPlayhead((CurrentFrame + 1) % TimelineFrameCount());
	}
	return EActiveTimerReturnType::Continue;
}

FReply SMocaraWindow::OnGenerate()
{
	if (PromptBox.IsValid())
	{
		Request.Prompt = PromptBox->GetText().ToString();
	}
	Request.ConstraintPreset.Reset();
	bHasPendingConstraints = false;
	PendingConstraints.Reset();
	PoseEditState->Modify();
	GetPoseKeys().Reset();
	SelectedKeyIndex = INDEX_NONE;
	bPlaying = false;
	CurrentFrame = 0;
	RefreshSelectionFromKeys();
	if (Viewport.IsValid())
	{
		Viewport->SetPlayheadFrame(CurrentFrame, TimelineFps);
	}
	return SubmitGenerate(nullptr);
}

FReply SMocaraWindow::SubmitGenerate(const TArray<TSharedPtr<FJsonValue>>* Constraints)
{
	FMocaraSidecarLauncher::Get().EnsureStarted();
	if (!FMocaraSidecarLauncher::Get().IsReady())
	{
		bWaitingForSidecar = true;
		if (Constraints)
		{
			PendingConstraints = *Constraints;
			bHasPendingConstraints = true;
		}
		SetStatus(FMocaraSidecarLauncher::Get().GetStatusText() + TEXT(" Generate will run when Kimodo is ready."));
		return FReply::Handled();
	}

	if (bSubmitInFlight)
	{
		return FReply::Handled();
	}

	// Submitting used to block the game thread for up to 15s on the click. Between the
	// submit and the reply that sets bPolling there is now a window where the buttons
	// would otherwise re-enable, so bSubmitInFlight covers it.
	bSubmitInFlight = true;
	SetStatus(Constraints ? TEXT("Regenerating with pose constraints...") : TEXT("Generating..."));

	TWeakPtr<SMocaraWindow> WeakSelf = SharedThis(this);
	Client.StartGenerateAsync(Request, Constraints,
		[WeakSelf](bool bOk, const FString& JobId, const FString& Error)
		{
			const TSharedPtr<SMocaraWindow> Self = WeakSelf.Pin();
			if (!Self.IsValid())
			{
				return;
			}
			Self->bSubmitInFlight = false;
			if (!bOk)
			{
				Self->SetStatus(Error);
				return;
			}
			Self->ActiveJob = FMocaraJobState();
			Self->ActiveJob.JobId = JobId;
			Self->ActiveJob.Status = TEXT("queued");
			Self->bPolling = true;
		});
	return FReply::Handled();
}

FReply SMocaraWindow::OnApplyAutoPose()
{
	FString Error;
	UAnimSequence* Target = LastTargetSequence.IsValid() ? LastTargetSequence.Get() : LastSomaSequence.Get();
	if (!FMocaraAutoPose::ApplyLocalKeys(Target, GetPoseKeys(), true, Error))
	{
		SetStatus(Error);
		return FReply::Handled();
	}
	SetStatus(TEXT("Applied local AutoPose to the last generated animation."));
	return FReply::Handled();
}

FReply SMocaraWindow::OnRegenerateWithConstraints()
{
	TArray<TSharedPtr<FJsonValue>> Constraints;
	FString Error;
	if (!FMocaraAutoPose::BuildConstraintsJson(LastSomaSequence.Get(), GetPoseKeys(), Constraints, Error))
	{
		SetStatus(Error);
		return FReply::Handled();
	}
	if (PromptBox.IsValid())
	{
		Request.Prompt = PromptBox->GetText().ToString();
	}
	Request.ConstraintPreset.Reset();
	return SubmitGenerate(&Constraints);
}

FReply SMocaraWindow::OnRegenerateWithTwoHandGrip()
{
	TArray<TSharedPtr<FJsonValue>> Constraints;
	FString Error;
	if (!FMocaraAutoPose::BuildTwoHandGripConstraintsJson(
		LastSomaSequence.Get(), Constraints, Error))
	{
		SetStatus(Error);
		return FReply::Handled();
	}
	if (PromptBox.IsValid())
	{
		Request.Prompt = PromptBox->GetText().ToString();
	}
	Request.ConstraintPreset = TEXT("two-handed-grip");
	return SubmitGenerate(&Constraints);
}

FMocaraJobState SMocaraWindow::SelectCandidate(
	const FMocaraJobState& Job,
	int32 CandidateIndex) const
{
	FMocaraJobState Selected = Job;
	if (Job.Artifacts.IsValidIndex(CandidateIndex))
	{
		const FMocaraJobArtifact& Artifact = Job.Artifacts[CandidateIndex];
		Selected.CandidateIndex = Artifact.CandidateIndex;
		if (Artifact.bHasSeed)
		{
			Selected.Seed = Artifact.Seed;
			Selected.bHasSeed = true;
		}
		Selected.BvhPath = Artifact.BvhPath;
		Selected.NpzPath = Artifact.NpzPath;
	}
	return Selected;
}

FReply SMocaraWindow::OnLoadCandidate()
{
	if (!ActiveJob.Artifacts.IsValidIndex(SelectedCandidateIndex))
	{
		SetStatus(TEXT("That candidate is not available for the current job."));
		return FReply::Handled();
	}
	ImportAndRetarget(SelectCandidate(ActiveJob, SelectedCandidateIndex));
	return FReply::Handled();
}

EActiveTimerReturnType SMocaraWindow::OnPoll(double, float)
{
	if (bWaitingForSidecar)
	{
		const EMocaraSidecarState SidecarState = FMocaraSidecarLauncher::Get().GetState();
		if (SidecarState == EMocaraSidecarState::Error)
		{
			bWaitingForSidecar = false;
			SetStatus(FMocaraSidecarLauncher::Get().GetStatusText());
		}
		else if (FMocaraSidecarLauncher::Get().IsReady())
		{
			bWaitingForSidecar = false;
			if (bHasPendingConstraints)
			{
				SubmitGenerate(&PendingConstraints);
				bHasPendingConstraints = false;
				PendingConstraints.Reset();
			}
			else
			{
				SubmitGenerate(nullptr);
			}
		}
		else
		{
			SetStatus(FMocaraSidecarLauncher::Get().GetStatusText() + TEXT(" Generate will run when Kimodo is ready."));
		}
	}

	// One query in flight at a time. The poll timer runs at 0.4s but a query is allowed
	// up to 5s, so without this a slow sidecar would pile up overlapping requests.
	if (!bPolling || ActiveJob.JobId.IsEmpty() || bJobQueryInFlight)
	{
		return EActiveTimerReturnType::Continue;
	}

	bJobQueryInFlight = true;
	TWeakPtr<SMocaraWindow> WeakSelf = SharedThis(this);
	Client.QueryJobAsync(ActiveJob.JobId,
		[WeakSelf](bool bOk, const FMocaraJobState& State, const FString& Error)
		{
			if (const TSharedPtr<SMocaraWindow> Self = WeakSelf.Pin())
			{
				Self->bJobQueryInFlight = false;
				Self->HandleJobState(bOk, State, Error);
			}
		});
	return EActiveTimerReturnType::Continue;
}

void SMocaraWindow::HandleJobState(bool bOk, const FMocaraJobState& State, const FString& Error)
{
	if (!bOk)
	{
		SetStatus(Error);
		return;
	}

	// Now that the query is asynchronous a reply can outlive the job that asked for it --
	// Stop, or a second Generate, moves on while the request is still in flight. Anything
	// that no longer matches the active job is stale and must not touch the UI.
	if (!bPolling || State.JobId != ActiveJob.JobId)
	{
		return;
	}

	ActiveJob = State;
	if (State.Status == TEXT("queued") || State.Status == TEXT("running"))
	{
		SetStatus(State.Status == TEXT("running") && State.CandidateCount > 1
			? FString::Printf(
				TEXT("Kimodo running candidate %d/%d..."),
				FMath::Min(State.CompletedCandidates + 1, State.CandidateCount),
				State.CandidateCount)
			: FString::Printf(TEXT("Kimodo %s..."), *State.Status));
		return;
	}

	bPolling = false;
	if (State.Status == TEXT("error"))
	{
		SetStatus(State.Error);
		return;
	}

	// Importing creates packages, runs the IK batch retarget and can open editors.
	// Doing that from inside the HTTP completion dispatch re-enters the UI mid-tick,
	// which is where the Persona access violations were coming from. Run it on the next
	// core tick instead, once the current frame has finished.
	if (!bImportPending)
	{
		bImportPending = true;
		SelectedCandidateIndex = 0;
		const FMocaraJobState PendingJob = SelectCandidate(State, SelectedCandidateIndex);
		TWeakPtr<SMocaraWindow> WeakSelf = SharedThis(this);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[WeakSelf, PendingJob](float) -> bool
			{
				if (const TSharedPtr<SMocaraWindow> Self = WeakSelf.Pin())
				{
					Self->bImportPending = false;
					Self->ImportAndRetarget(PendingJob);
				}
				return false; // one-shot
			}));
	}
}

bool SMocaraWindow::ImportAndRetarget(const FMocaraJobState& Job)
{
	const UMocaraSettings* Settings = GetDefault<UMocaraSettings>();
	if (!Settings)
	{
		SetStatus(TEXT("Mocara settings are unavailable."));
		return false;
	}

	if (Job.BvhPath.IsEmpty())
	{
		SetStatus(TEXT("Kimodo finished but returned no BVH path."));
		return false;
	}

	FMocaraImportedClip Clip;
	FString Error;
	const FString BaseName = FString::Printf(
		TEXT("AS_Kimodo_%s_C%02d"), *Job.JobId, Job.CandidateIndex + 1);
	if (!FMocaraBvhImporter::ImportFile(Job.BvhPath, Settings->GeneratedPath, BaseName, Request.bInPlace, Clip, Error))
	{
		SetStatus(Error);
		return false;
	}
	LastSomaSequence = Clip.Sequence;
	LastSomaMesh = Clip.Mesh;
	bool bAllAssetsSaved = Clip.bAllAssetsSaved;
	if (Job.NumFrames > 0)
	{
		TimelineFrames = Job.NumFrames;
	}
	if (Job.Fps > 0.f)
	{
		TimelineFps = Job.Fps;
	}
	CurrentFrame = 0;

	// Show the SOMA clip straight away; if the retarget succeeds below we swap to the target,
	// which is a far better preview than the proxy mesh the importer builds.
	if (Viewport.IsValid())
	{
		Viewport->SetPreview(Clip.Mesh, Clip.Sequence);
		Viewport->SetPlaying(bPlaying);
	}

	FMocaraTargetProfile TargetProfile;
	USkeletalMesh* TargetMesh = FMocaraRetargeter::FindTargetMesh(TargetProfile, Error);
	if (!TargetMesh)
	{
		SetStatus(WithAssetSaveWarning(
			Error.IsEmpty()
				? TEXT("Imported SOMA clip. Select a target mesh in Project Settings > Plugins > Mocara.")
				: Error,
			Settings->bAutoSaveGenerated, bAllAssetsSaved));
		return true;
	}
	LastTargetMesh = TargetMesh;

	const FName TargetAssetId = FMocaraRetargeter::MakeTargetAssetId(TargetMesh, TargetProfile);
	UIKRigDefinition* SomaRig = FMocaraRetargeter::EnsureIkRig(Clip.Mesh, Settings->RetargetPath, TEXT("IK_SOMA"), true);
	UIKRigDefinition* TargetRig = FMocaraRetargeter::EnsureIkRig(
		TargetMesh, Settings->RetargetPath,
		FName(*FString::Printf(TEXT("IK_%s"), *TargetAssetId.ToString())), false);
	UIKRetargeter* Fwd = FMocaraRetargeter::EnsureRetargeter(
		SomaRig, TargetRig, Settings->RetargetPath,
		FName(*FString::Printf(TEXT("RTG_SomaTo_%s"), *TargetAssetId.ToString())));

	UAnimSequence* TargetSequence = FMocaraRetargeter::Retarget(
		Clip.Sequence, Clip.Mesh, TargetMesh, TargetProfile, Fwd, Settings->GeneratedPath,
		BaseName + TEXT("_") + TargetAssetId.ToString(), Error);
	if (!TargetSequence)
	{
		SetStatus(WithAssetSaveWarning(
			Error.IsEmpty() ? TEXT("SOMA import succeeded; target retarget failed.") : Error,
			Settings->bAutoSaveGenerated, bAllAssetsSaved));
		return true;
	}
	LastTargetSequence = TargetSequence;

	// The batch retarget creates the target clip but does not write it out, and that is the
	// asset people actually use. Persist it, plus the rigs, so a regenerate reuses them
	// instead of rebuilding and so the clip survives closing the editor.
	if (Settings->bAutoSaveGenerated)
	{
		bAllAssetsSaved &= FMocaraBvhImporter::SaveGeneratedAsset(TargetSequence);
		bAllAssetsSaved &= FMocaraBvhImporter::SaveGeneratedAsset(SomaRig);
		bAllAssetsSaved &= FMocaraBvhImporter::SaveGeneratedAsset(TargetRig);
		bAllAssetsSaved &= FMocaraBvhImporter::SaveGeneratedAsset(Fwd);
	}

	if (Viewport.IsValid())
	{
		Viewport->SetPreview(TargetMesh, TargetSequence);
		Viewport->SetPlayheadFrame(CurrentFrame, TimelineFps);
		Viewport->SetPlaying(bPlaying);
	}
	const FString SeedText = Job.bHasSeed ? FString::FromInt(Job.Seed) : TEXT("unseeded");
	SetStatus(WithAssetSaveWarning(
		FString::Printf(
			TEXT("Imported candidate %d/%d as %s (seed %s, text %.1f, constraint %.1f)."),
			Job.CandidateIndex + 1,
			FMath::Max(Job.CandidateCount, Job.Artifacts.Num()),
			*TargetSequence->GetName(),
			*SeedText,
			Job.TextGuidance,
			Job.ConstraintGuidance),
		Settings->bAutoSaveGenerated, bAllAssetsSaved));
	return true;
}

static FAutoConsoleCommand GMocaraCaptureCommand(
	TEXT("Mocara.CaptureViewport"),
	TEXT("Capture the Mocara preview after a delay: Mocara.CaptureViewport <out.bmp> [delaySec] [frame]"),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogTemp, Error, TEXT("Usage: Mocara.CaptureViewport <out.bmp> [delaySec] [frame]"));
			return;
		}
		const FString Out = Args[0];
		const float Delay = Args.IsValidIndex(1) ? FCString::Atof(*Args[1]) : 4.f;
		const int32 Frame = Args.IsValidIndex(2) ? FCString::Atoi(*Args[2]) : INDEX_NONE;

		// The tab needs a few frames to lay out and render before the viewport has a
		// valid render target, so defer rather than capturing inline.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
			[Out, Frame](float) -> bool
			{
				const TSharedPtr<SMocaraWindow> Window = SMocaraWindow::GetLiveWindow().Pin();
				if (!Window.IsValid() || !Window->Viewport.IsValid())
				{
					UE_LOG(LogTemp, Error, TEXT("Mocara.CaptureViewport: no open Mocara tab."));
					return false;
				}
				if (Frame >= 0)
				{
					Window->SetPlayhead(Frame);
				}
				UE_LOG(LogTemp, Display, TEXT("Mocara.CaptureViewport %s -> %s"), *Out,
					Window->Viewport->CaptureToFile(Out) ? TEXT("ok") : TEXT("FAILED"));
				return false;
			}), Delay);
	}));

static FAutoConsoleCommand GMocaraSetViewCommand(
	TEXT("Mocara.SetView"),
	TEXT("Snap the preview camera: Mocara.SetView front|side|top|persp"),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		const TSharedPtr<SMocaraWindow> Window = SMocaraWindow::GetLiveWindow().Pin();
		if (Args.Num() < 1 || !Window.IsValid() || !Window->Viewport.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("Usage: Mocara.SetView front|side|top|persp (needs an open tab)"));
			return;
		}
		const FString Which = Args[0].ToLower();
		EMocaraViewPreset Preset = EMocaraViewPreset::Perspective;
		if (Which == TEXT("front")) { Preset = EMocaraViewPreset::Front; }
		else if (Which == TEXT("side")) { Preset = EMocaraViewPreset::Side; }
		else if (Which == TEXT("top")) { Preset = EMocaraViewPreset::Top; }
		Window->Viewport->SetViewPreset(Preset);
		UE_LOG(LogTemp, Display, TEXT("Mocara.SetView %s"), *Which);
	}));

static FAutoConsoleCommand GMocaraExportFbxCommand(
	TEXT("Mocara.ExportFbx"),
	TEXT("Export the newest clip to FBX: Mocara.ExportFbx <out.fbx>"),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		const TSharedPtr<SMocaraWindow> Window = SMocaraWindow::GetLiveWindow().Pin();
		if (Args.Num() < 1 || !Window.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("Usage: Mocara.ExportFbx <out.fbx> (needs an open Mocara tab)"));
			return;
		}
		FString Error;
		UE_LOG(LogTemp, Display, TEXT("Mocara.ExportFbx %s -> %s"), *Args[0],
			Window->ExportLatestToFbx(Args[0], Error) ? TEXT("ok") : *Error);
	}));

static FAutoConsoleCommand GMocaraLoadClipCommand(
	TEXT("Mocara.LoadClip"),
	TEXT("Load a BVH into the open Mocara tab: Mocara.LoadClip <path>"),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogTemp, Error, TEXT("Usage: Mocara.LoadClip <bvh-path>"));
			return;
		}
		const TSharedPtr<SMocaraWindow> Window = SMocaraWindow::GetLiveWindow().Pin();
		if (!Window.IsValid())
		{
			UE_LOG(LogTemp, Error, TEXT("Mocara.LoadClip: no Mocara tab is open (run Mocara.OpenTab first)."));
			return;
		}
		UE_LOG(LogTemp, Display, TEXT("Mocara.LoadClip: %s -> %s"), *Args[0],
			Window->LoadClipFromFile(Args[0]) ? TEXT("ok") : TEXT("FAILED"));
	}));

#undef LOCTEXT_NAMESPACE
