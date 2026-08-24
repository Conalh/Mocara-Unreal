#include "MocaraEditorModule.h"
#include "SMocaraWindow.h"
#include "MocaraSettings.h"
#include "MocaraSidecarLauncher.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Application/SlateApplication.h"
#include "MocaraKimodoClient.h"
#include "Misc/CoreDelegates.h"
#include "HAL/IConsoleManager.h"

#define LOCTEXT_NAMESPACE "Mocara"

static const FName MocaraTabName("MocaraTab");

void FMocaraEditorModule::StartupModule()
{
	FMocaraSidecarLauncher::Get().StartWatching();

	// Shut the sidecar down here rather than in ShutdownModule: this fires while the
	// HTTP module and the UObject system are still alive, so the graceful /shutdown
	// POST can actually be sent. ShutdownModule runs after teardown has begun, and
	// touching settings CDOs there was crashing the editor on every exit.
	EnginePreExitHandle = FCoreDelegates::OnEnginePreExit.AddRaw(this, &FMocaraEditorModule::HandleEnginePreExit);

	// Automation's `Quit` command requests a forced process exit on Windows. That
	// path bypasses OnEnginePreExit but broadcasts this delegate before terminating
	// the process, so it must share the same idempotent sidecar cleanup.
	ApplicationWillTerminateHandle = FCoreDelegates::GetApplicationWillTerminateDelegate().AddRaw(
		this, &FMocaraEditorModule::HandleEnginePreExit);

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FMocaraEditorModule::RegisterMenus));
}

void FMocaraEditorModule::HandleEnginePreExit()
{
	FMocaraSidecarLauncher::Get().StopWatching();
}

void FMocaraEditorModule::ShutdownModule()
{
	if (ApplicationWillTerminateHandle.IsValid())
	{
		FCoreDelegates::GetApplicationWillTerminateDelegate().Remove(ApplicationWillTerminateHandle);
		ApplicationWillTerminateHandle.Reset();
	}

	if (EnginePreExitHandle.IsValid())
	{
		FCoreDelegates::OnEnginePreExit.Remove(EnginePreExitHandle);
		EnginePreExitHandle.Reset();
	}

	// Idempotent: normally already done in HandleEnginePreExit. This covers the
	// plugin being unloaded while the editor keeps running.
	FMocaraSidecarLauncher::Get().StopWatching();

	// UToolMenus and the global tab manager are UObject/Slate singletons; skip them
	// once teardown has started rather than resurrecting them.
	if (FMocaraKimodoClient::IsUObjectAccessSafe())
	{
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
	}

	if (FSlateApplication::IsInitialized())
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(MocaraTabName);
	}
}

void FMocaraEditorModule::RegisterMenus()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(MocaraTabName, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			.Label(LOCTEXT("TabLabel", "Mocara"))
			[
				SNew(SMocaraWindow)
			];
	}))
	.SetDisplayName(LOCTEXT("TabTitle", "Mocara"))
	.SetTooltipText(LOCTEXT("TabTooltip", "Generate Kimodo motion and retarget onto a UE5 mannequin or MetaHuman body"))
	.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
	.SetMenuType(ETabSpawnerMenuType::Enabled);

	FToolMenuOwnerScoped OwnerScoped(this);
	if (UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window"))
	{
		FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
		Section.AddMenuEntry(
			"OpenMocara",
			LOCTEXT("OpenMocara", "Mocara"),
			LOCTEXT("OpenMocaraTooltip", "Open the Mocara Kimodo generator"),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateRaw(this, &FMocaraEditorModule::OpenMocaraWindow)));
	}
}

void FMocaraEditorModule::OpenMocaraWindow()
{
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(MocaraTabName));
}

/** Automation hook: open the Mocara tab without going through the Window menu. */
static FAutoConsoleCommand GMocaraOpenTabCommand(
	TEXT("Mocara.OpenTab"),
	TEXT("Open the Mocara tab."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		FGlobalTabmanager::Get()->TryInvokeTab(FTabId(MocaraTabName));
	}));

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FMocaraEditorModule, MocaraEditor)
