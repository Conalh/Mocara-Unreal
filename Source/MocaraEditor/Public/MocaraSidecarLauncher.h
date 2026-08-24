#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"

/** One prerequisite reported by doctor.sh. */
struct FMocaraCheck
{
	FString Key;
	FString Status;   // OK / SKIP / MISSING / FAIL
	FString Detail;

	bool IsProblem() const { return Status != TEXT("OK") && Status != TEXT("SKIP"); }
};

enum class EMocaraSidecarState : uint8
{
	Unknown,
	Starting,
	Loading,
	Ready,
	Error
};

/** Starts the WSL Kimodo sidecar from the editor and tracks /health. */
class FMocaraSidecarLauncher
{
public:
	static FMocaraSidecarLauncher& Get();

	void StartWatching();
	void StopWatching();
	void EnsureStarted();
	void StopSidecar();

	/** Re-read settings into the shutdown-safe cache. No-op once UObjects are gone. */
	void RefreshCachedSettings();

	/** Report on WSL/Kimodo prerequisites. Logs one line per check. Blocking. */
	void RunDoctor();

	/** Same checks, off the game thread. Results land in GetChecks(). */
	void RunDoctorAsync();
	bool IsDoctorRunning() const { return bDoctorRunning; }
	bool HasDoctorRun() const { return bDoctorHasRun; }
	const TArray<FMocaraCheck>& GetChecks() const { return LastChecks; }
	int32 GetProblemCount() const;

	/** Run setup_kimodo.sh, streaming progress into GetSetupStatus(). */
	void BeginSetup();
	bool IsSetupRunning() const { return SetupProcHandle.IsValid(); }
	FString GetSetupStatus() const { return SetupStatus; }

	/** Windows path -> /mnt/<drive>/... form. */
	static FString ToWslPathPublic(const FString& WindowsPath);

	EMocaraSidecarState GetState() const { return State; }
	FString GetStatusText() const { return StatusText; }
	bool IsReady() const { return State == EMocaraSidecarState::Ready; }

private:
	bool Tick(float DeltaTime);
	void LaunchWsl();
	void PollHealth();
	void KillWslProcesses();
	FString CurrentDistro() const;
	static FString ToWslPath(const FString& WindowsPath);
	static FString FindWslExe();
	static FString ResolveHfToken();
	static FString SidecarScriptWindowsPath();

	EMocaraSidecarState State = EMocaraSidecarState::Unknown;
	FString StatusText = TEXT("Sidecar: checking...");
	FString LastError;
	FString DeviceName;
	FProcHandle ProcHandle;
	FTSTicker::FDelegateHandle TickHandle;
	double LastLaunchTime = 0.0;
	/** Last known-good distro, so the shutdown path never dereferences a CDO. */
	FString CachedDistro = TEXT("Ubuntu");
	bool bHealthInFlight = false;
	bool bWatching = false;
	bool bLaunchAttempted = false;
	bool bStopped = false;
	/**
	 * Set by EnsureStarted, consumed by the /health reply. The probe has to resolve
	 * before we may launch: starting a second uvicorn against a sidecar that is already
	 * up loses the port race and overwrites the pidfile, orphaning the live process.
	 */
	bool bLaunchIfProbeFails = false;

	TArray<FMocaraCheck> LastChecks;
	bool bDoctorRunning = false;
	bool bDoctorHasRun = false;
	/**
	 * Set once teardown starts. The doctor's worker blocks in ExecProcess against WSL and
	 * cannot be cancelled, so this does not stop it -- it stops the reply from scheduling
	 * further game-thread work (including the setup path's follow-up doctor run) after
	 * the ticker is gone.
	 */
	bool bTearingDown = false;

	FProcHandle SetupProcHandle;
	void* SetupPipeRead = nullptr;
	void* SetupPipeWrite = nullptr;
	FString SetupStatus;
	FString SetupBuffer;
	FTSTicker::FDelegateHandle SetupTickHandle;
	bool TickSetup(float DeltaTime);
};
