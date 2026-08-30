from pathlib import Path
import hashlib
import json
import re


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
        "docs/decisions/0001-public-plugin-boundary.md",
        "docs/decisions/0002-verifiable-motion-authoring.md",
        "docs/decisions/0003-native-backend-experiment.md",
    }

    missing = sorted(path for path in required if not (REPO_ROOT / path).is_file())
    assert missing == [], f"missing public repository files: {missing}"


def test_public_repository_contains_only_the_plugin_not_a_host_project() -> None:
    assert list(REPO_ROOT.glob("*.uproject")) == []
    assert not (REPO_ROOT / "Content").exists()
    assert not (REPO_ROOT / "Unreal").exists()


def test_no_model_weights_are_bundled() -> None:
    weight_suffixes = {".bin", ".ckpt", ".gguf", ".onnx", ".pt", ".pth", ".safetensors"}
    bundled = sorted(
        path.relative_to(REPO_ROOT).as_posix()
        for path in REPO_ROOT.rglob("*")
        if path.is_file() and ".git" not in path.parts and path.suffix.lower() in weight_suffixes
    )

    assert bundled == []


def test_common_model_weight_formats_are_ignored() -> None:
    ignore_rules = (REPO_ROOT / ".gitignore").read_text(encoding="utf-8").splitlines()

    for suffix in (".bin", ".ckpt", ".gguf", ".onnx", ".pt", ".pth", ".safetensors"):
        assert f"*{suffix}" in ignore_rules

    assert "*.egg-info/" in ignore_rules


def test_soma_reference_pose_matches_the_attributed_kimodo_resource() -> None:
    resource = REPO_ROOT / "Resources" / "somaskel77_standard_tpose.bvh"

    assert hashlib.sha256(resource.read_bytes()).hexdigest() == (
        "3e8cdaf72d2b12a25450ff1af7da261175a830e186c1e07ff094f99ef604d85b"
    )


def test_packaged_model_manifest_uses_only_immutable_content_identities() -> None:
    manifest = json.loads(
        (REPO_ROOT / "Resources" / "model-manifest.json").read_text(encoding="utf-8")
    )

    assert manifest["schema_version"] == 1
    assert re.fullmatch(r"[0-9a-f]{40}", manifest["backend"]["source_revision"])
    assert {repository["role"] for repository in manifest["repositories"]} == {
        "motion",
        "text-base-adapter",
        "text-supervised-adapter",
        "text-foundation",
    }
    for repository in manifest["repositories"]:
        assert re.fullmatch(r"[0-9a-f]{40}", repository["revision"])
        assert repository["required_files"]
        for required in repository["required_files"]:
            assert required["size"] > 0
            assert re.fullmatch(r"[0-9a-f]{64}", required["sha256"])
            assert not Path(required["path"]).is_absolute()


def test_plugin_descriptor_preserves_the_beta_release_identity() -> None:
    descriptor = json.loads((REPO_ROOT / "Mocara.uplugin").read_text(encoding="utf-8"))

    assert descriptor["VersionName"] == "0.3.0"
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


def test_content_dependent_unreal_tests_are_clean_host_safe() -> None:
    expected_guards = {
        "MocaraTargetProfileTests.cpp": "skipping mesh-specific assertions",
        "MocaraRetargetTargetTests.cpp": "skipping explicit MetaHuman target assertions",
        "MocaraPreviewCharacterTests.cpp": "skipping assembled-character assertions",
    }
    test_root = REPO_ROOT / "Source" / "MocaraEditor" / "Private" / "Tests"

    for filename, guard_message in expected_guards.items():
        contents = (test_root / filename).read_text(encoding="utf-8")
        assert "AddWarning" in contents
        assert guard_message in contents


def test_github_actions_are_pinned_to_immutable_commits() -> None:
    workflow = (REPO_ROOT / ".github" / "workflows" / "ci.yml").read_text(
        encoding="utf-8"
    )
    action_revisions = re.findall(r"^\s*- uses: [^@\s]+@([^\s#]+)", workflow, re.MULTILINE)

    assert action_revisions
    assert all(re.fullmatch(r"[0-9a-f]{40}", revision) for revision in action_revisions)
