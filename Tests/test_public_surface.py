from pathlib import Path
import hashlib
import json


REPO_ROOT = Path(__file__).resolve().parents[1]


def test_public_repository_has_required_community_files() -> None:
    required = {
        "README.md",
        "LICENSE",
        "NOTICE",
        "THIRD_PARTY_NOTICES.md",
        "CONTRIBUTING.md",
        "SECURITY.md",
        "CHANGELOG.md",
        "docs/ARCHITECTURE.md",
        "docs/BUILD_YOUR_OWN.md",
    }

    missing = sorted(path for path in required if not (REPO_ROOT / path).is_file())
    assert missing == [], f"missing public repository files: {missing}"


def test_public_repository_contains_only_the_plugin_not_a_host_project() -> None:
    assert list(REPO_ROOT.glob("*.uproject")) == []
    assert not (REPO_ROOT / "Content").exists()
    assert not (REPO_ROOT / "Unreal").exists()


def test_no_model_weights_are_bundled() -> None:
    weight_suffixes = {".bin", ".ckpt", ".onnx", ".pt", ".pth", ".safetensors"}
    bundled = sorted(
        path.relative_to(REPO_ROOT).as_posix()
        for path in REPO_ROOT.rglob("*")
        if path.is_file() and ".git" not in path.parts and path.suffix.lower() in weight_suffixes
    )

    assert bundled == []


def test_soma_reference_pose_matches_the_attributed_kimodo_resource() -> None:
    resource = REPO_ROOT / "Resources" / "somaskel77_standard_tpose.bvh"

    assert hashlib.sha256(resource.read_bytes()).hexdigest() == (
        "3e8cdaf72d2b12a25450ff1af7da261175a830e186c1e07ff094f99ef604d85b"
    )


def test_plugin_descriptor_preserves_the_beta_release_identity() -> None:
    descriptor = json.loads((REPO_ROOT / "Mocara.uplugin").read_text(encoding="utf-8"))

    assert descriptor["VersionName"] == "0.2.0"
    assert descriptor["IsBetaVersion"] is True
    assert descriptor["Modules"] == [
        {"Name": "MocaraEditor", "Type": "Editor", "LoadingPhase": "Default"}
    ]


def test_public_text_has_no_private_checkout_path_or_embedded_engine_token() -> None:
    private_windows_root = "C:" + "\\BlindMeridian\\DevTools\\Mocara"
    private_wsl_root = "/mnt/c/" + "BlindMeridian/DevTools/Mocara"
    forbidden = []
    for path in REPO_ROOT.rglob("*"):
        if not path.is_file() or ".git" in path.parts or path == Path(__file__):
            continue
        try:
            contents = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        if (
            private_windows_root in contents
            or private_wsl_root in contents
            or "SecurityToken=" in contents
        ):
            forbidden.append(path.relative_to(REPO_ROOT).as_posix())

    assert sorted(forbidden) == []
