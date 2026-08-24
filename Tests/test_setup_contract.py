from pathlib import Path
import re

try:
    import tomllib
except ModuleNotFoundError:  # Python 3.10, which is Mocara's provisioned version.
    import tomli as tomllib

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
PLUGIN_ROOT = REPO_ROOT

# This public repository is the installable plugin root, so the runtime has one
# canonical Scripts/ and Sidecar/ location.
PLUGIN_SCRIPTS = PLUGIN_ROOT / "Scripts"
PLUGIN_SIDECAR = PLUGIN_ROOT / "Sidecar"

SETUP_SCRIPTS = [PLUGIN_SCRIPTS / "setup_kimodo.sh"]
RUN_SCRIPTS = [PLUGIN_SCRIPTS / "run_sidecar.sh"]
STOP_SCRIPTS = [PLUGIN_SCRIPTS / "stop_sidecar.sh"]


def test_repository_root_is_the_installable_plugin() -> None:
    assert (PLUGIN_ROOT / "Mocara.uplugin").is_file()
    assert not (REPO_ROOT / "Unreal" / "Plugins" / "Mocara").exists()


def test_sidecar_test_client_dependencies_are_declared() -> None:
    pyproject = tomllib.loads(
        (PLUGIN_SIDECAR / "pyproject.toml").read_text(encoding="utf-8")
    )

    test_dependencies = pyproject["project"]["optional-dependencies"]["test"]
    dependency_names = {
        re.split(r"[<>=; ]", dependency, maxsplit=1)[0]
        for dependency in test_dependencies
    }
    assert {"httpx", "httpx2", "pytest"} <= dependency_names


@pytest.mark.parametrize("script", SETUP_SCRIPTS, ids=lambda path: str(path.relative_to(REPO_ROOT)))
def test_setup_provisions_pytest_in_the_mocara_venv(script: Path) -> None:
    contents = script.read_text(encoding="utf-8")

    install_line = next(
        line for line in contents.splitlines() if line.startswith("uv pip install --python")
    )
    assert "pytest" in install_line


@pytest.mark.parametrize("script", RUN_SCRIPTS, ids=lambda path: str(path.relative_to(REPO_ROOT)))
def test_sidecar_pidfile_records_a_process_ownership_token(script: Path) -> None:
    contents = script.read_text(encoding="utf-8")

    assert "MOCARA_SIDECAR_TOKEN" in contents
    assert "trap cleanup EXIT" in contents
    assert "exec python -m uvicorn" not in contents


@pytest.mark.parametrize("script", STOP_SCRIPTS, ids=lambda path: str(path.relative_to(REPO_ROOT)))
def test_stop_script_validates_pid_ownership_without_process_name_scanning(script: Path) -> None:
    contents = script.read_text(encoding="utf-8")

    assert "/proc/$pid/environ" in contents
    assert "/proc/$pid/cmdline" in contents
    assert "MOCARA_SIDECAR_TOKEN=$owner_token" in contents
    assert "pkill" not in contents


def test_editor_shutdown_uses_the_ownership_checked_stop_script() -> None:
    launcher = (
        PLUGIN_ROOT
        / "Source"
        / "MocaraEditor"
        / "Private"
        / "MocaraSidecarLauncher.cpp"
    ).read_text(encoding="utf-8")

    kill_method = launcher.split(
        "void FMocaraSidecarLauncher::KillWslProcesses()", maxsplit=1
    )[1].split("FString FMocaraSidecarLauncher::ToWslPath", maxsplit=1)[0]
    assert 'PluginScriptPath(TEXT("stop_sidecar.sh"))' in kill_method
    assert "pkill -f" not in kill_method
    assert "sed -i" not in kill_method


def test_script_launchers_normalize_without_modifying_plugin_content() -> None:
    launcher = (
        PLUGIN_ROOT
        / "Source"
        / "MocaraEditor"
        / "Private"
        / "MocaraSidecarLauncher.cpp"
    ).read_text(encoding="utf-8")
    powershell_launcher = (PLUGIN_SCRIPTS / "run_sidecar.ps1").read_text(
        encoding="utf-8"
    )

    assert "sed -i" not in launcher
    assert "sed -i" not in powershell_launcher
    assert launcher.count("BuildReadOnlyWslScriptArgs(") == 5  # helper + four call sites
    assert "bash -o pipefail" in launcher
    assert "sed 's/\\\\r$//'" in launcher
    assert "| bash" in launcher
    assert "sed 's/\\r$//'" in powershell_launcher
    assert "| bash" in powershell_launcher


def test_shell_scripts_are_pinned_to_lf_in_git() -> None:
    attributes = (REPO_ROOT / ".gitattributes").read_text(encoding="utf-8")

    assert re.search(r"^\*\.sh\s+text\s+eol=lf$", attributes, flags=re.MULTILINE)


@pytest.mark.parametrize("script", SETUP_SCRIPTS, ids=lambda path: str(path.relative_to(REPO_ROOT)))
def test_setup_fails_when_torch_is_still_unusable(script: Path) -> None:
    contents = script.read_text(encoding="utf-8")

    failure_blocks = re.findall(
        r'if ! torch_ok "\$\{SM:-\}"; then(?P<body>.*?)\nfi',
        contents,
        flags=re.DOTALL,
    )
    assert len(failure_blocks) == 2
    assert "exit 1" in failure_blocks[-1]


@pytest.mark.parametrize("script", SETUP_SCRIPTS, ids=lambda path: str(path.relative_to(REPO_ROOT)))
def test_missing_nvidia_smi_reaches_the_explicit_no_gpu_diagnostic(script: Path) -> None:
    contents = script.read_text(encoding="utf-8")

    compute_cap_assignment = next(
        line for line in contents.splitlines() if line.startswith('COMPUTE_CAP="')
    )
    gpu_name_assignment = next(
        line for line in contents.splitlines() if line.startswith('GPU_NAME="')
    )
    assert "|| true" in compute_cap_assignment
    assert "|| true" in gpu_name_assignment


def test_editor_setup_forwards_the_configured_mocara_root() -> None:
    launcher = (
        PLUGIN_ROOT
        / "Source"
        / "MocaraEditor"
        / "Private"
        / "MocaraSidecarLauncher.cpp"
    ).read_text(encoding="utf-8")
    begin_setup = launcher.split("void FMocaraSidecarLauncher::BeginSetup()", maxsplit=1)[1]
    begin_setup = begin_setup.split("bool FMocaraSidecarLauncher::TickSetup", maxsplit=1)[0]

    assert 'ExportToWsl(TEXT("MOCARA_ROOT")' in begin_setup


def test_generation_buttons_disable_while_a_request_is_active() -> None:
    window = (
        PLUGIN_ROOT
        / "Source"
        / "MocaraEditor"
        / "Private"
        / "SMocaraWindow.cpp"
    ).read_text(encoding="utf-8")
    generate_button = window.split('LOCTEXT("Generate", "Generate")', maxsplit=1)[1]
    generate_button = generate_button.split('LOCTEXT("ExportFbx"', maxsplit=1)[0]

    regenerate_button = window.split(
        'LOCTEXT("Regen", "Regenerate with constraints")', maxsplit=1
    )[1]
    regenerate_button = regenerate_button.split('LOCTEXT("HideBones"', maxsplit=1)[0]

    # bSubmitInFlight covers the gap opened by the async submit: between the click and
    # the reply that sets bPolling, the buttons would otherwise be live again.
    expected = "!bPolling && !bWaitingForSidecar && !bSubmitInFlight"
    assert expected in generate_button
    assert expected in regenerate_button


def test_no_blocking_http_flush_outside_the_shutdown_path() -> None:
    """A full HTTP flush blocks the game thread on every in-flight request in the process.

    Exactly one call may remain -- editor teardown, where there is no later tick to
    deliver a callback. Everything else must go through the async helpers.
    """
    client = (
        PLUGIN_ROOT
        / "Source"
        / "MocaraEditor"
        / "Private"
        / "MocaraKimodoClient.cpp"
    ).read_text(encoding="utf-8")

    assert client.count("EHttpFlushReason::FullFlush") == 1
    # The one survivor is reachable only from RequestShutdown.
    blocking_callers = [
        name for name in ("Health(", "StartGenerate(", "QueryJob(")
        if f"FMocaraKimodoClient::{name}" in client
    ]
    assert blocking_callers == [], blocking_callers


def test_force_quit_registers_sidecar_cleanup_before_process_termination() -> None:
    module = (
        PLUGIN_ROOT
        / "Source"
        / "MocaraEditor"
        / "Private"
        / "MocaraEditorModule.cpp"
    ).read_text(encoding="utf-8")

    assert "GetApplicationWillTerminateDelegate().AddRaw" in module
    assert "GetApplicationWillTerminateDelegate().Remove" in module


def test_asset_save_failures_reach_the_user_visible_status() -> None:
    importer_header = (
        PLUGIN_ROOT
        / "Source"
        / "MocaraEditor"
        / "Public"
        / "MocaraBvhImporter.h"
    ).read_text(encoding="utf-8")
    importer = (
        PLUGIN_ROOT
        / "Source"
        / "MocaraEditor"
        / "Private"
        / "MocaraBvhImporter.cpp"
    ).read_text(encoding="utf-8")
    window = (
        PLUGIN_ROOT
        / "Source"
        / "MocaraEditor"
        / "Private"
        / "SMocaraWindow.cpp"
    ).read_text(encoding="utf-8")

    assert "bool bAllAssetsSaved = true;" in importer_header
    assert importer.count("OutClip.bAllAssetsSaved &= SaveGeneratedAsset(") == 3
    # Manny sequence + SOMA rig + Manny rig + the forward retargeter. The unused
    # reverse retargeter was intentionally removed in 90e3dc5.
    assert window.count("bAllAssetsSaved &= FMocaraBvhImporter::SaveGeneratedAsset(") == 4
    assert "one or more generated assets could not be saved" in window
