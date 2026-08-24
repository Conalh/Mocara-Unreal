#include "MocaraSidecarLauncher.h"
#include "MocaraKimodoClient.h"
#include "MocaraSettings.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Interfaces/IPluginManager.h"
#include "HAL/IConsoleManager.h"
#include "Async/Async.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <winreg.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogMocaraSidecar, Log, All);

static FString PluginScriptPath(const TCHAR* ScriptName);
static FString ResolveSidecarRootWindowsPath();

static FString BuildReadOnlyWslScriptArgs(
	const FString& Distro,
	const FString& WslScript,
	const bool bMergeStdErr = false)
{
	return FString::Printf(
		TEXT("-d %s -- bash -o pipefail -c \"sed 's/\\r$//' '%s' | bash%s\""),
		*Distro,
		*WslScript,
		bMergeStdErr ? TEXT(" 2>&1") : TEXT(""));
}

FMocaraSidecarLauncher& FMocaraSidecarLauncher::Get()
{
	static FMocaraSidecarLauncher Instance;
	return Instance;
}

void FMocaraSidecarLauncher::StartWatching()
{
	if (bWatching)
	{
		return;
	}
	bWatching = true;
	bStopped = false;
	RefreshCachedSettings();
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FMocaraSidecarLauncher::Tick), 1.5f);

	const UMocaraSettings* Settings = GetDefault<UMocaraSettings>();
	if (Settings && Settings->bAutoStartSidecar)
	{
		EnsureStarted();
	}
	else
	{
		PollHealth();
	}
}

void FMocaraSidecarLauncher::StopWatching()
{
	bTearingDown = true;
	if (TickHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	bWatching = false;

	if (SetupTickHandle.IsValid())
	{
		FTSTicker::RemoveTicker(SetupTickHandle);
		SetupTickHandle.Reset();
	}
	if (SetupProcHandle.IsValid())
	{
		if (FPlatformProcess::IsProcRunning(SetupProcHandle))
		{
			FPlatformProcess::TerminateProc(SetupProcHandle, true);
		}
		FPlatformProcess::CloseProc(SetupProcHandle);
		SetupProcHandle.Reset();
	}
	if (SetupPipeRead || SetupPipeWrite)
	{
		FPlatformProcess::ClosePipe(SetupPipeRead, SetupPipeWrite);
		SetupPipeRead = nullptr;
		SetupPipeWrite = nullptr;
	}

	// Called from both OnEnginePreExit and ShutdownModule; only do the work once.
	if (bStopped)
	{
		return;
	}
	bStopped = true;
	StopSidecar();
}

void FMocaraSidecarLauncher::RefreshCachedSettings()
{
	if (!FMocaraKimodoClient::IsUObjectAccessSafe())
	{
		return;
	}
	FMocaraKimodoClient::RefreshCachedSettings();
	if (const UMocaraSettings* Settings = GetDefault<UMocaraSettings>())
	{
		if (!Settings->WslDistro.IsEmpty())
		{
			CachedDistro = Settings->WslDistro;
		}
	}
}

void FMocaraSidecarLauncher::StopSidecar()
{
	UE_LOG(LogMocaraSidecar, Display, TEXT("Stopping Kimodo sidecar"));

	// Graceful /shutdown only while HTTP is alive. On the editor teardown path the
	// module is already gone, and the WSL kill below is enough to stop the process.
	if (FMocaraKimodoClient::IsHttpAvailable())
	{
		FMocaraKimodoClient Client;
		Client.RequestShutdown();
	}

	if (ProcHandle.IsValid())
	{
		if (FPlatformProcess::IsProcRunning(ProcHandle))
		{
			FPlatformProcess::TerminateProc(ProcHandle, true);
		}
		FPlatformProcess::CloseProc(ProcHandle);
		ProcHandle.Reset();
	}

	KillWslProcesses();
	State = EMocaraSidecarState::Unknown;
	StatusText = TEXT("Sidecar: stopped");
	bLaunchAttempted = false;
}

FString FMocaraSidecarLauncher::CurrentDistro() const
{
	if (FMocaraKimodoClient::IsUObjectAccessSafe())
	{
		if (const UMocaraSettings* Settings = GetDefault<UMocaraSettings>())
		{
			if (!Settings->WslDistro.IsEmpty())
			{
				return Settings->WslDistro;
			}
		}
	}
	return CachedDistro.IsEmpty() ? TEXT("Ubuntu") : CachedDistro;
}

void FMocaraSidecarLauncher::KillWslProcesses()
{
	const FString StopScriptWin = PluginScriptPath(TEXT("stop_sidecar.sh"));
	if (StopScriptWin.IsEmpty())
	{
		UE_LOG(LogMocaraSidecar, Warning, TEXT("stop_sidecar.sh not found; skipping PID cleanup."));
		return;
	}

	const FString WslExe = FindWslExe();
	const FString Distro = CurrentDistro();
	const FString StopScriptWsl = ToWslPath(StopScriptWin);
	const FString Args = BuildReadOnlyWslScriptArgs(Distro, StopScriptWsl);

	uint32 Pid = 0;
	FProcHandle KillHandle = FPlatformProcess::CreateProc(
		*WslExe,
		*Args,
		true,
		true,
		true,
		&Pid,
		0,
		nullptr,
		nullptr);
	if (KillHandle.IsValid())
	{
		FPlatformProcess::WaitForProc(KillHandle);
		FPlatformProcess::CloseProc(KillHandle);
	}
}

void FMocaraSidecarLauncher::EnsureStarted()
{
	if (State == EMocaraSidecarState::Ready || State == EMocaraSidecarState::Loading || State == EMocaraSidecarState::Starting)
	{
		return;
	}

	// Ask first, launch second. This used to be a blocking /health that stalled the game
	// thread for up to 3s (plus a full HTTP flush) on every Generate click; PollHealth
	// answers the same question asynchronously and LaunchWsl happens in its reply.
	bLaunchIfProbeFails = true;
	PollHealth();
}

bool FMocaraSidecarLauncher::Tick(float)
{
	RefreshCachedSettings();
	PollHealth();
	if ((State == EMocaraSidecarState::Unknown || State == EMocaraSidecarState::Error) && !bLaunchAttempted)
	{
		const UMocaraSettings* Settings = GetDefault<UMocaraSettings>();
		if (Settings && Settings->bAutoStartSidecar)
		{
			LaunchWsl();
		}
	}
	return true;
}

FString FMocaraSidecarLauncher::ToWslPathPublic(const FString& WindowsPath)
{
	return ToWslPath(WindowsPath);
}

FString FMocaraSidecarLauncher::ToWslPath(const FString& WindowsPath)
{
	FString Normalized = FPaths::ConvertRelativePathToFull(WindowsPath);
	FPaths::NormalizeFilename(Normalized);
	Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (Normalized.Len() >= 2 && Normalized[1] == TEXT(':'))
	{
		const TCHAR Drive = FChar::ToLower(Normalized[0]);
		return FString::Printf(TEXT("/mnt/%c%s"), Drive, *Normalized.Mid(2));
	}
	return Normalized;
}

FString FMocaraSidecarLauncher::FindWslExe()
{
	const FString SystemRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("SystemRoot"));
	const FString Candidate = FPaths::Combine(SystemRoot, TEXT("System32"), TEXT("wsl.exe"));
	if (FPaths::FileExists(Candidate))
	{
		return Candidate;
	}
	return TEXT("wsl.exe");
}

FString FMocaraSidecarLauncher::SidecarScriptWindowsPath()
{
	// 1. Explicit override.
	if (FMocaraKimodoClient::IsUObjectAccessSafe())
	{
		if (const UMocaraSettings* Settings = GetDefault<UMocaraSettings>())
		{
			if (!Settings->SidecarScriptPath.IsEmpty() && FPaths::FileExists(Settings->SidecarScriptPath))
			{
				return FPaths::ConvertRelativePathToFull(Settings->SidecarScriptPath);
			}
		}
	}

	// 2. The copy that ships inside the plugin -- the only copy. This is what makes
	//    Mocara portable; a sibling <repo>/scripts/ folder is only ever present inside
	//    the Mocara repo itself, so it is not a fallback worth keeping.
	return PluginScriptPath(TEXT("run_sidecar.sh"));
}

FString FMocaraSidecarLauncher::ResolveHfToken()
{
	FString Token = FPlatformMisc::GetEnvironmentVariable(TEXT("HF_TOKEN")).TrimStartAndEnd();
	if (Token.StartsWith(TEXT("hf_")) && Token.Len() < 200)
	{
		return Token;
	}

#if PLATFORM_WINDOWS
	HKEY Key = nullptr;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_READ, &Key) == ERROR_SUCCESS)
	{
		WCHAR Buffer[256] = {};
		DWORD Size = sizeof(Buffer);
		DWORD Type = 0;
		if (RegQueryValueExW(Key, L"HF_TOKEN", nullptr, &Type, reinterpret_cast<LPBYTE>(Buffer), &Size) == ERROR_SUCCESS
			&& (Type == REG_SZ || Type == REG_EXPAND_SZ))
		{
			Token = FString(Buffer).TrimStartAndEnd();
		}
		RegCloseKey(Key);
	}
	if (Token.StartsWith(TEXT("hf_")) && Token.Len() < 200)
	{
		return Token;
	}
#endif

	const FString TokenFile = FPaths::Combine(
		FPlatformMisc::GetEnvironmentVariable(TEXT("USERPROFILE")),
		TEXT(".cache"),
		TEXT("huggingface"),
		TEXT("token"));
	FString FileToken;
	if (FFileHelper::LoadFileToString(FileToken, *TokenFile))
	{
		FileToken.TrimStartAndEndInline();
		if (FileToken.StartsWith(TEXT("hf_")) && FileToken.Len() < 200)
		{
			return FileToken;
		}
	}
	return FString();
}

/** Windows path to a script that ships alongside run_sidecar.sh in the plugin. */
static FString PluginScriptPath(const TCHAR* ScriptName)
{
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Mocara")))
	{
		const FString Path = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Scripts"), ScriptName);
		if (FPaths::FileExists(Path))
		{
			return FPaths::ConvertRelativePathToFull(Path);
		}
	}
	return FString();
}

/**
 * Root containing the packaged Sidecar/ directory. An explicit setting remains useful
 * for sidecar development, but installed plugins must work without a source checkout.
 */
static FString ResolveSidecarRootWindowsPath()
{
	if (FMocaraKimodoClient::IsUObjectAccessSafe())
	{
		if (const UMocaraSettings* Settings = GetDefault<UMocaraSettings>())
		{
			if (!Settings->SidecarRoot.IsEmpty())
			{
				return FPaths::ConvertRelativePathToFull(Settings->SidecarRoot);
			}
		}
	}

	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Mocara")))
	{
		return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
	}
	return FString();
}

/**
 * Port out of a sidecar URL, or empty if it has none. Split from the right so an IPv6
 * host ("http://[::1]:8765") does not get chopped at the wrong colon.
 */
static FString PortFromSidecarUrl(const FString& Url)
{
	FString HostPort = Url;
	FString Scheme;
	FString AfterScheme;
	if (HostPort.Split(TEXT("://"), &Scheme, &AfterScheme))
	{
		HostPort = AfterScheme;
	}

	// Drop any path so "127.0.0.1:8765/health" cannot leak the path into the port.
	int32 SlashIndex = INDEX_NONE;
	if (HostPort.FindChar(TEXT('/'), SlashIndex))
	{
		HostPort.LeftInline(SlashIndex);
	}

	FString Host;
	FString Port;
	if (HostPort.Split(TEXT(":"), &Host, &Port, ESearchCase::CaseSensitive, ESearchDir::FromEnd)
		&& !Port.IsEmpty()
		&& Port.IsNumeric())
	{
		return Port;
	}
	return FString();
}

/** Set a Windows env var and make sure WSLENV forwards it into the distro. */
static void ExportToWsl(const TCHAR* Name, const FString& Value)
{
	if (Value.IsEmpty())
	{
		return;
	}
	FPlatformMisc::SetEnvironmentVar(Name, *Value);

	FString WslEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("WSLENV"));
	const FString Entry = FString(Name) + TEXT("/u");
	if (!WslEnv.Contains(Name))
	{
		WslEnv = WslEnv.IsEmpty() ? Entry : WslEnv + TEXT(":") + Entry;
		FPlatformMisc::SetEnvironmentVar(TEXT("WSLENV"), *WslEnv);
	}
}

void FMocaraSidecarLauncher::LaunchWsl()
{
	const double Now = FPlatformTime::Seconds();
	if (Now - LastLaunchTime < 8.0)
	{
		return;
	}
	LastLaunchTime = Now;
	bLaunchAttempted = true;

	const FString Distro = CurrentDistro();
	const FString ScriptWin = SidecarScriptWindowsPath();
	if (!FPaths::FileExists(ScriptWin))
	{
		State = EMocaraSidecarState::Error;
		LastError = ScriptWin.IsEmpty()
			? TEXT("run_sidecar.sh is missing from the Mocara plugin's Scripts folder.")
			: FString::Printf(TEXT("Missing sidecar script: %s"), *ScriptWin);
		StatusText = LastError;
		UE_LOG(LogMocaraSidecar, Error, TEXT("%s"), *LastError);
		return;
	}

	const FString Token = ResolveHfToken();
	if (!Token.IsEmpty())
	{
		FPlatformMisc::SetEnvironmentVar(TEXT("HF_TOKEN"), *Token);
		FString WslEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("WSLENV"));
		if (!WslEnv.Contains(TEXT("HF_TOKEN")))
		{
			WslEnv = WslEnv.IsEmpty() ? TEXT("HF_TOKEN/u") : WslEnv + TEXT(":HF_TOKEN/u");
			FPlatformMisc::SetEnvironmentVar(TEXT("WSLENV"), *WslEnv);
		}
	}

	// Generated clips belong to whichever project is running, not to the Mocara repo.
	// run_sidecar.sh already honours these two, so no script edit is needed.
	ExportToWsl(TEXT("MOCARA_OUTPUT_DIR"),
		ToWslPath(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Kimodo")))));

	// Bind where the client is configured to look, so SidecarUrl is a real setting
	// rather than one that silently points at a port nothing ever listens on.
	const FMocaraKimodoClient UrlSource;
	ExportToWsl(TEXT("MOCARA_PORT"), PortFromSidecarUrl(UrlSource.MakeUrl(FString())));

	// Scripts are CR-stripped and streamed through stdin, so they cannot infer their
	// installed path from BASH_SOURCE. Always forward the packaged plugin root (or the
	// explicit developer override) instead of relying on a machine-specific fallback.
	ExportToWsl(TEXT("MOCARA_ROOT"), ToWslPath(ResolveSidecarRootWindowsPath()));

	const FString WslScript = ToWslPath(ScriptWin);
	const FString WslExe = FindWslExe();
	const FString Args = BuildReadOnlyWslScriptArgs(Distro, WslScript);

	uint32 Pid = 0;
	ProcHandle = FPlatformProcess::CreateProc(
		*WslExe,
		*Args,
		false,
		true,
		true,
		&Pid,
		0,
		nullptr,
		nullptr);

	if (!ProcHandle.IsValid())
	{
		State = EMocaraSidecarState::Error;
		LastError = TEXT("Failed to start wsl.exe. Is WSL Ubuntu installed?");
		StatusText = LastError;
		UE_LOG(LogMocaraSidecar, Error, TEXT("%s"), *LastError);
		return;
	}

	State = EMocaraSidecarState::Starting;
	StatusText = TEXT("Sidecar: starting WSL Kimodo (first load 30-60s)...");
	UE_LOG(LogMocaraSidecar, Display, TEXT("Started sidecar pid=%u script=%s"), Pid, *WslScript);
}

void FMocaraSidecarLauncher::PollHealth()
{
	if (bHealthInFlight)
	{
		return;
	}
	if (!FMocaraKimodoClient::IsHttpAvailable())
	{
		return;
	}
	bHealthInFlight = true;

	const FMocaraKimodoClient Client;
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = Client.MakeRequest(TEXT("GET"), TEXT("/health"));
	Request->SetTimeout(2.0f);
	Request->OnProcessRequestComplete().BindLambda([this](FHttpRequestPtr, FHttpResponsePtr Response, bool bSucceeded)
	{
		bHealthInFlight = false;
		if (!bSucceeded || !Response.IsValid() || Response->GetResponseCode() != 200)
		{
			if (State != EMocaraSidecarState::Starting)
			{
				State = EMocaraSidecarState::Unknown;
				StatusText = TEXT("Sidecar: offline");
			}
			// Confirmed down, so EnsureStarted's deferred launch is safe to run now.
			if (bLaunchIfProbeFails)
			{
				bLaunchIfProbeFails = false;
				LaunchWsl();
			}
			return;
		}

		// Something answered /health -- never launch a second one on top of it.
		bLaunchIfProbeFails = false;

		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Response->GetContentAsString());
		if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
		{
			return;
		}

		const bool bReady = Json->GetBoolField(TEXT("ready"));
		const bool bLoading = Json->GetBoolField(TEXT("loading"));
		Json->TryGetStringField(TEXT("device_name"), DeviceName);
		FString Error;
		Json->TryGetStringField(TEXT("error"), Error);

		if (bReady)
		{
			State = EMocaraSidecarState::Ready;
			StatusText = DeviceName.IsEmpty()
				? TEXT("Sidecar: ready")
				: FString::Printf(TEXT("Sidecar: ready (%s)"), *DeviceName);
		}
		else if (bLoading)
		{
			State = EMocaraSidecarState::Loading;
			StatusText = TEXT("Sidecar: loading Kimodo...");
		}
		else if (!Error.IsEmpty())
		{
			State = EMocaraSidecarState::Error;
			LastError = Error;
			StatusText = FString::Printf(TEXT("Sidecar error: %s"), *Error);
		}
	});
	if (!Request->ProcessRequest())
	{
		bHealthInFlight = false;
		if (bLaunchIfProbeFails)
		{
			bLaunchIfProbeFails = false;
			LaunchWsl();
		}
	}
}

/** Run doctor.sh and parse its KEY=STATUS|detail lines. Blocking; safe off the game thread. */
static TArray<FMocaraCheck> ExecuteDoctor(const FString& WslExe, const FString& Distro, const FString& ScriptWin)
{
	TArray<FMocaraCheck> Checks;
	const FString WslScript = FMocaraSidecarLauncher::ToWslPathPublic(ScriptWin);
	const FString Args = BuildReadOnlyWslScriptArgs(Distro, WslScript);

	int32 ReturnCode = 0;
	FString StdOut;
	FString StdErr;
	FPlatformProcess::ExecProcess(*WslExe, *Args, &ReturnCode, &StdOut, &StdErr);

	TArray<FString> Lines;
	StdOut.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		FString Payload;
		if (!Line.Split(TEXT("MOCARA_CHECK "), nullptr, &Payload))
		{
			continue;
		}
		FMocaraCheck Check;
		FString Rest;
		if (!Payload.Split(TEXT("="), &Check.Key, &Rest) || Check.Key == TEXT("done"))
		{
			continue;
		}
		if (!Rest.Split(TEXT("|"), &Check.Status, &Check.Detail))
		{
			Check.Status = Rest;
		}
		Checks.Add(MoveTemp(Check));
	}

	if (Checks.Num() == 0)
	{
		FMocaraCheck Failure;
		Failure.Key = TEXT("wsl");
		Failure.Status = TEXT("FAIL");
		Failure.Detail = StdErr.IsEmpty()
			? FString::Printf(TEXT("preflight produced no output (exit %d)"), ReturnCode)
			: StdErr.Left(160).TrimStartAndEnd();
		Checks.Add(MoveTemp(Failure));
	}
	return Checks;
}

int32 FMocaraSidecarLauncher::GetProblemCount() const
{
	int32 Count = 0;
	for (const FMocaraCheck& Check : LastChecks)
	{
		Count += Check.IsProblem() ? 1 : 0;
	}
	return Count;
}

void FMocaraSidecarLauncher::RunDoctorAsync()
{
	if (bDoctorRunning || bTearingDown)
	{
		return;
	}
	const FString ScriptWin = PluginScriptPath(TEXT("doctor.sh"));
	if (ScriptWin.IsEmpty())
	{
		return;
	}
	bDoctorRunning = true;

	// Forward the Windows user so the script can find the Windows-side HF token.
	ExportToWsl(TEXT("WINUSER"), FPlatformProcess::UserName(false));

	// doctor.sh shells out and hits the network; it must not run on the game thread.
	const FString WslExe = FindWslExe();
	const FString Distro = CurrentDistro();
	Async(EAsyncExecution::Thread, [this, WslExe, Distro, ScriptWin]()
	{
		TArray<FMocaraCheck> Result = ExecuteDoctor(WslExe, Distro, ScriptWin);
		AsyncTask(ENamedThreads::GameThread, [this, Result = MoveTemp(Result)]() mutable
		{
			bDoctorRunning = false;
			if (bTearingDown)
			{
				return;
			}
			LastChecks = MoveTemp(Result);
			bDoctorHasRun = true;
			UE_LOG(LogMocaraSidecar, Display, TEXT("Preflight: %d problem(s)."), GetProblemCount());
		});
	});
}

void FMocaraSidecarLauncher::RunDoctor()
{
	const FString ScriptWin = PluginScriptPath(TEXT("doctor.sh"));
	if (ScriptWin.IsEmpty())
	{
		UE_LOG(LogMocaraSidecar, Error, TEXT("doctor.sh not found in the Mocara plugin."));
		return;
	}
	ExportToWsl(TEXT("WINUSER"), FPlatformProcess::UserName(false));

	UE_LOG(LogMocaraSidecar, Display, TEXT("Running Mocara preflight in WSL (%s)..."), *CurrentDistro());
	LastChecks = ExecuteDoctor(FindWslExe(), CurrentDistro(), ScriptWin);
	bDoctorHasRun = true;

	for (const FMocaraCheck& Check : LastChecks)
	{
		if (Check.IsProblem())
		{
			UE_LOG(LogMocaraSidecar, Warning, TEXT("  [%-7s] %-9s %s"), *Check.Status, *Check.Key, *Check.Detail);
		}
		else
		{
			UE_LOG(LogMocaraSidecar, Display, TEXT("  [%-7s] %-9s %s"), *Check.Status, *Check.Key, *Check.Detail);
		}
	}
	UE_LOG(LogMocaraSidecar, Display, TEXT("Preflight finished: %d problem(s)."), GetProblemCount());
}

static FAutoConsoleCommand GMocaraDoctorCommand(
	TEXT("Mocara.Doctor"),
	TEXT("Check the WSL/Kimodo prerequisites and report what is missing."),
	FConsoleCommandDelegate::CreateStatic([]() { FMocaraSidecarLauncher::Get().RunDoctor(); }));

void FMocaraSidecarLauncher::BeginSetup()
{
	if (IsSetupRunning() || bTearingDown)
	{
		return;
	}
	const FString ScriptWin = PluginScriptPath(TEXT("setup_kimodo.sh"));
	if (ScriptWin.IsEmpty())
	{
		SetupStatus = TEXT("setup_kimodo.sh not found in the plugin.");
		return;
	}

	ExportToWsl(TEXT("WINUSER"), FPlatformProcess::UserName(false));
	if (const FString Token = ResolveHfToken(); !Token.IsEmpty())
	{
		ExportToWsl(TEXT("HF_TOKEN"), Token);
	}
	ExportToWsl(TEXT("MOCARA_ROOT"), ToWslPath(ResolveSidecarRootWindowsPath()));

	// setup_kimodo.sh clones repos and installs torch -- minutes, not seconds. Run it
	// with a pipe so the panel can show progress instead of appearing hung.
	if (!FPlatformProcess::CreatePipe(SetupPipeRead, SetupPipeWrite))
	{
		SetupStatus = TEXT("Could not create a pipe for setup output.");
		return;
	}

	const FString WslScript = ToWslPath(ScriptWin);
	const FString Args = BuildReadOnlyWslScriptArgs(CurrentDistro(), WslScript, true);

	uint32 Pid = 0;
	SetupProcHandle = FPlatformProcess::CreateProc(
		*FindWslExe(), *Args, false, true, true, &Pid, 0, nullptr, SetupPipeWrite);

	if (!SetupProcHandle.IsValid())
	{
		FPlatformProcess::ClosePipe(SetupPipeRead, SetupPipeWrite);
		SetupPipeRead = SetupPipeWrite = nullptr;
		SetupStatus = TEXT("Failed to start setup (is WSL installed?).");
		return;
	}

	SetupBuffer.Reset();
	SetupStatus = TEXT("Setup: starting...");
	UE_LOG(LogMocaraSidecar, Display, TEXT("Mocara setup started (pid=%u)"), Pid);

	SetupTickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateRaw(this, &FMocaraSidecarLauncher::TickSetup), 0.25f);
}

bool FMocaraSidecarLauncher::TickSetup(float)
{
	if (!SetupProcHandle.IsValid())
	{
		return false;
	}

	// Drain whatever the script has written since the last tick.
	const FString Chunk = FPlatformProcess::ReadPipe(SetupPipeRead);
	if (!Chunk.IsEmpty())
	{
		SetupBuffer += Chunk;
		TArray<FString> Lines;
		SetupBuffer.ParseIntoArrayLines(Lines, false);
		// Keep the trailing partial line in the buffer for next tick.
		if (Lines.Num() > 0 && !SetupBuffer.EndsWith(TEXT("\n")))
		{
			SetupBuffer = Lines.Pop();
		}
		else
		{
			SetupBuffer.Reset();
		}
		for (const FString& Line : Lines)
		{
			const FString Trimmed = Line.TrimStartAndEnd();
			if (!Trimmed.IsEmpty())
			{
				UE_LOG(LogMocaraSidecar, Display, TEXT("  setup| %s"), *Trimmed);
				SetupStatus = FString::Printf(TEXT("Setup: %s"), *Trimmed.Left(90));
			}
		}
	}

	if (FPlatformProcess::IsProcRunning(SetupProcHandle))
	{
		return true;   // keep ticking
	}

	int32 ReturnCode = 0;
	FPlatformProcess::GetProcReturnCode(SetupProcHandle, &ReturnCode);
	FPlatformProcess::CloseProc(SetupProcHandle);
	SetupProcHandle.Reset();
	FPlatformProcess::ClosePipe(SetupPipeRead, SetupPipeWrite);
	SetupPipeRead = SetupPipeWrite = nullptr;
	SetupTickHandle.Reset();

	SetupStatus = (ReturnCode == 0)
		? TEXT("Setup finished. Re-running preflight...")
		: FString::Printf(TEXT("Setup failed (exit %d) - see Output Log."), ReturnCode);
	UE_LOG(LogMocaraSidecar, Display, TEXT("Mocara setup exited with %d"), ReturnCode);

	// Whatever happened, the prerequisite picture has changed.
	RunDoctorAsync();
	return false;
}

static FAutoConsoleCommand GMocaraSetupCommand(
	TEXT("Mocara.Setup"),
	TEXT("Install the Kimodo sidecar prerequisites in WSL (streams progress)."),
	FConsoleCommandDelegate::CreateStatic([]() { FMocaraSidecarLauncher::Get().BeginSetup(); }));
