from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[1]
PLUGIN_ROOT = REPO_ROOT
PLUGIN_SIDECAR = PLUGIN_ROOT / "Sidecar"


def test_sidecar_runtime_is_owned_by_the_installable_plugin() -> None:
    assert (PLUGIN_SIDECAR / "pyproject.toml").is_file()
    assert (PLUGIN_SIDECAR / "mocara_sidecar" / "__init__.py").is_file()
    assert (PLUGIN_SIDECAR / "mocara_sidecar" / "server.py").is_file()
    assert (PLUGIN_ROOT / "Mocara.uplugin").is_file()
    assert not (REPO_ROOT / "Unreal" / "Plugins" / "Mocara").exists()


def test_package_filter_includes_the_complete_local_runtime() -> None:
    contents = (PLUGIN_ROOT / "Config" / "FilterPlugin.ini").read_text(encoding="utf-8")

    assert "/Scripts/..." in contents
    assert "/Sidecar/..." in contents
    assert "/Resources/..." in contents
    assert "-/Sidecar/.../__pycache__/..." in contents
    assert "-/Sidecar/.../*.pyc" in contents


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
