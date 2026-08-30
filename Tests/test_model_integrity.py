from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import sys

import pytest


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PLUGIN_ROOT / "Sidecar"))

from mocara_sidecar.model_integrity import (  # noqa: E402
    ModelManifestError,
    materialize_text_adapter_overlay,
    prepare_verified_bundle,
)


def _sha256(contents: bytes) -> str:
    return hashlib.sha256(contents).hexdigest()


def _write_manifest(path: Path, payload: bytes) -> None:
    path.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "bundle_id": "test-bundle",
                "backend": {
                    "id": "nvidia-kimodo-python",
                    "source_url": "https://github.com/nv-tlabs/kimodo.git",
                    "source_revision": "1" * 40,
                    "license": "Apache-2.0",
                },
                "repositories": [
                    {
                        "role": "motion",
                        "repo_id": "example/motion",
                        "revision": "2" * 40,
                        "license": "test-license",
                        "required_files": [
                            {
                                "path": "weights/model.safetensors",
                                "size": len(payload),
                                "sha256": _sha256(payload),
                            }
                        ],
                    }
                ],
            }
        ),
        encoding="utf-8",
    )


def test_verified_bundle_uses_immutable_revision_and_records_weight_hashes(tmp_path: Path) -> None:
    payload = b"known model weights"
    manifest_path = tmp_path / "manifest.json"
    snapshot = tmp_path / "snapshot"
    weight = snapshot / "weights" / "model.safetensors"
    weight.parent.mkdir(parents=True)
    weight.write_bytes(payload)
    _write_manifest(manifest_path, payload)
    calls: list[tuple[str, str, tuple[str, ...]]] = []

    def download(repo_id: str, revision: str, allow_patterns: list[str]) -> str:
        calls.append((repo_id, revision, tuple(allow_patterns)))
        return str(snapshot)

    bundle = prepare_verified_bundle(
        manifest_path,
        verification_cache_path=tmp_path / "verification.json",
        snapshot_download_fn=download,
    )

    assert calls == [
        ("example/motion", "2" * 40, ("weights/model.safetensors",))
    ]
    assert bundle.backend_id == "nvidia-kimodo-python"
    assert bundle.snapshot_for("motion") == snapshot
    assert bundle.public_record["status"] == "verified"
    assert bundle.public_record["repositories"][0]["revision"] == "2" * 40
    assert bundle.public_record["repositories"][0]["weights"] == [
        {
            "path": "weights/model.safetensors",
            "size": len(payload),
            "sha256": _sha256(payload),
        }
    ]
    assert len(bundle.public_record["bundle_sha256"]) == 64


def test_verified_bundle_rejects_corrupted_weight(tmp_path: Path) -> None:
    payload = b"known model weights"
    manifest_path = tmp_path / "manifest.json"
    snapshot = tmp_path / "snapshot"
    weight = snapshot / "weights" / "model.safetensors"
    weight.parent.mkdir(parents=True)
    weight.write_bytes(b"tampered model data")
    _write_manifest(manifest_path, payload)

    with pytest.raises(ModelManifestError, match="sha256"):
        prepare_verified_bundle(
            manifest_path,
            verification_cache_path=tmp_path / "verification.json",
            snapshot_download_fn=lambda **_: str(snapshot),
        )


def test_verification_cache_rehashes_a_same_size_replacement(tmp_path: Path) -> None:
    payload = b"known model weights"
    manifest_path = tmp_path / "manifest.json"
    snapshot = tmp_path / "snapshot"
    weight = snapshot / "weights" / "model.safetensors"
    weight.parent.mkdir(parents=True)
    weight.write_bytes(payload)
    _write_manifest(manifest_path, payload)
    cache = tmp_path / "verification.json"
    download = lambda **_: str(snapshot)
    prepare_verified_bundle(
        manifest_path,
        verification_cache_path=cache,
        snapshot_download_fn=download,
    )
    original = weight.stat()
    replacement = weight.with_suffix(".replacement")
    replacement.write_bytes(b"x" * len(payload))
    os.utime(replacement, ns=(original.st_atime_ns, original.st_mtime_ns))
    os.replace(replacement, weight)

    with pytest.raises(ModelManifestError, match="sha256"):
        prepare_verified_bundle(
            manifest_path,
            verification_cache_path=cache,
            snapshot_download_fn=download,
        )


@pytest.mark.parametrize("unsafe_path", ["../outside.bin", "/absolute.bin", "weights/../../outside.bin"])
def test_manifest_rejects_paths_outside_snapshot(tmp_path: Path, unsafe_path: str) -> None:
    payload = b"weights"
    manifest_path = tmp_path / "manifest.json"
    _write_manifest(manifest_path, payload)
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    manifest["repositories"][0]["required_files"][0]["path"] = unsafe_path
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")

    with pytest.raises(ModelManifestError, match="relative"):
        prepare_verified_bundle(
            manifest_path,
            verification_cache_path=tmp_path / "verification.json",
            snapshot_download_fn=lambda **_: str(tmp_path / "snapshot"),
        )


def test_text_adapter_overlay_pins_foundation_path_without_mutating_snapshot(tmp_path: Path) -> None:
    source = tmp_path / "adapter-snapshot"
    source.mkdir()
    original = {
        "base_model_name_or_path": "moving/main",
        "inference_mode": True,
    }
    (source / "adapter_config.json").write_text(json.dumps(original), encoding="utf-8")
    (source / "adapter_model.safetensors").write_bytes(b"adapter")
    foundation = tmp_path / "foundation-snapshot"
    foundation.mkdir()

    overlay = materialize_text_adapter_overlay(source, foundation, tmp_path / "overlays")

    pinned = json.loads((overlay / "adapter_config.json").read_text(encoding="utf-8"))
    assert pinned["base_model_name_or_path"] == str(foundation.resolve())
    assert json.loads((source / "adapter_config.json").read_text(encoding="utf-8")) == original
    assert (overlay / "adapter_model.safetensors").read_bytes() == b"adapter"
