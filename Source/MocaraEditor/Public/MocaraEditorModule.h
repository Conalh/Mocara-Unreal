#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FMocaraEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterMenus();
	void OpenMocaraWindow();

	/** Stops the sidecar while HTTP and the UObject system are still alive. */
	void HandleEnginePreExit();

	FDelegateHandle EnginePreExitHandle;
	FDelegateHandle ApplicationWillTerminateHandle;
};
