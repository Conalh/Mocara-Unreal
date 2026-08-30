import json
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:  # Python 3.10 uses the declared test extra.
    import tomli as tomllib


REPO_ROOT = Path(__file__).resolve().parents[1]
PLUGIN_ROOT = REPO_ROOT
PLUGIN_SIDECAR = PLUGIN_ROOT / "Sidecar"


def test_release_version_is_aligned_across_plugin_and_sidecar() -> None:
    descriptor = json.loads((PLUGIN_ROOT / "Mocara.uplugin").read_text(encoding="utf-8"))
    pyproject = tomllib.loads(
        (PLUGIN_SIDECAR / "pyproject.toml").read_text(encoding="utf-8")
    )
    server = (PLUGIN_SIDECAR / "mocara_sidecar" / "server.py").read_text(
        encoding="utf-8"
    )

    assert descriptor["Version"] == 3
    assert descriptor["VersionName"] == "0.3.0"
    assert pyproject["project"]["version"] == "0.3.0"
    assert 'version="0.3.0"' in server


def test_sidecar_runtime_is_owned_by_the_installable_plugin() -> None:
    assert (PLUGIN_SIDECAR / "pyproject.toml").is_file()
    assert (PLUGIN_SIDECAR / "mocara_sidecar" / "__init__.py").is_file()
    assert (PLUGIN_SIDECAR / "mocara_sidecar" / "backends.py").is_file()
    assert (PLUGIN_SIDECAR / "mocara_sidecar" / "server.py").is_file()
    assert (PLUGIN_ROOT / "Scripts" / "benchmark_kimodo_cpp.py").is_file()
    assert (
        PLUGIN_ROOT / "docs" / "decisions" / "0002-verifiable-motion-authoring.md"
    ).is_file()
    assert (
        PLUGIN_ROOT / "docs" / "decisions" / "0003-native-backend-experiment.md"
    ).is_file()
    assert (PLUGIN_ROOT / "Mocara.uplugin").is_file()
    assert not (REPO_ROOT / "Unreal" / "Plugins" / "Mocara").exists()


def test_package_filter_includes_the_complete_local_runtime() -> None:
    contents = (PLUGIN_ROOT / "Config" / "FilterPlugin.ini").read_text(encoding="utf-8")

    assert "/Scripts/..." in contents
    assert "/Sidecar/..." in contents
    assert "/Resources/..." in contents
    assert "/docs/..." in contents
    assert "/LICENSE" in contents
    assert "/NOTICE" in contents
    assert "/THIRD_PARTY_NOTICES.md" in contents
    assert "-/Scripts/.../__pycache__/..." in contents
    assert "-/Scripts/.../*.pyc" in contents
    assert "-/Sidecar/.../__pycache__/..." in contents
    assert "-/Sidecar/.../*.pyc" in contents
    assert "-/Sidecar/.../*.egg-info/..." in contents


def test_runtime_scripts_have_no_checkout_specific_default() -> None:
    for name in ("run_sidecar.sh", "setup_kimodo.sh"):
        contents = (PLUGIN_ROOT / "Scripts" / name).read_text(encoding="utf-8")

        assert 'SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"' in contents
        assert 'PLUGIN_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"' in contents

    powershell_launcher = (PLUGIN_ROOT / "Scripts" / "run_sidecar.ps1").read_text(
        encoding="utf-8"
    )
    assert "ConvertTo-WslPath" in powershell_launcher
    assert 'Join-Path $PSScriptRoot ".."' in powershell_launcher


def test_editor_exports_the_packaged_plugin_root_for_streamed_scripts() -> None:
    launcher = (
        PLUGIN_ROOT
        / "Source"
        / "MocaraEditor"
        / "Private"
        / "MocaraSidecarLauncher.cpp"
    ).read_text(encoding="utf-8")

    assert "ResolveSidecarRootWindowsPath" in launcher
    assert launcher.count('ExportToWsl(TEXT("MOCARA_ROOT"), ToWslPath(ResolveSidecarRootWindowsPath()))') == 2
