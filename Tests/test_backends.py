from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

from fastapi.testclient import TestClient


REPO_ROOT = Path(__file__).resolve().parents[1]
SIDECAR_ROOT = REPO_ROOT / "Sidecar"
sys.path.insert(0, str(SIDECAR_ROOT))

from mocara_sidecar.backends import (  # noqa: E402
    EXPERIMENTAL_BACKEND_ID,
    KIMODO_CPP_EXPECTED_REVISION,
    PRODUCTION_BACKEND_ID,
    evaluate_promotion,
    get_backend_catalog,
    read_git_revision,
)
from mocara_sidecar.server import CLIENT_HEADER, KimodoRuntime, create_app  # noqa: E402


HEADERS = {CLIENT_HEADER: "pytest"}


def test_catalog_keeps_python_active_and_native_explicitly_gated() -> None:
    catalog = {
        backend.backend_id: backend
        for backend in get_backend_catalog(environment={})
    }

    production = catalog[PRODUCTION_BACKEND_ID]
    native = catalog[EXPERIMENTAL_BACKEND_ID]
    assert production.active is True
    assert production.available is True
    assert production.production_ready is True
    assert native.active is False
    assert native.configured is False
    assert native.available is False
    assert native.production_ready is False
    assert native.capabilities["text_prompt"] is True
    assert native.capabilities["multi_prompt"] is True
    assert native.capabilities["general_constraints"] is False
    assert native.capabilities["soma_77_joint_output"] is False
    assert native.capabilities["bvh_output"] is False
    assert "configuration" in native.failed_gates


def test_native_probe_reports_readiness_without_exposing_local_paths(tmp_path: Path) -> None:
    source = tmp_path / "private-source"
    executable = tmp_path / "private-build" / "kmd-generate.exe"
    motion = tmp_path / "private-models" / "motion.gguf"
    text_bundle = tmp_path / "private-models" / "text-bundle"
    source.mkdir()
    executable.parent.mkdir()
    executable.write_bytes(b"MZ")
    motion.parent.mkdir()
    motion.write_bytes(b"GGUF")
    text_bundle.mkdir()
    environment = {
        "MOCARA_KIMODO_CPP_SOURCE": str(source),
        "MOCARA_KIMODO_CPP_EXECUTABLE": str(executable),
        "MOCARA_KIMODO_CPP_MOTION_MODEL": str(motion),
        "MOCARA_KIMODO_CPP_TEXT_BUNDLE": str(text_bundle),
    }

    native = next(
        backend
        for backend in get_backend_catalog(
            environment=environment,
            revision_reader=lambda _: KIMODO_CPP_EXPECTED_REVISION,
        )
        if backend.backend_id == EXPERIMENTAL_BACKEND_ID
    )

    assert native.configured is True
    assert native.available is True
    assert native.source_revision == KIMODO_CPP_EXPECTED_REVISION
    assert native.source_revision_match is True
    serialized = json.dumps(native.to_public_dict())
    assert str(tmp_path) not in serialized
    assert "private-source" not in serialized
    assert "private-models" not in serialized


def test_native_probe_rejects_relative_or_control_character_paths() -> None:
    environment = {
        "MOCARA_KIMODO_CPP_SOURCE": "relative/source",
        "MOCARA_KIMODO_CPP_EXECUTABLE": "bad\npath",
        "MOCARA_KIMODO_CPP_MOTION_MODEL": "motion.gguf",
        "MOCARA_KIMODO_CPP_TEXT_BUNDLE": "text-bundle",
    }

    native = next(
        backend
        for backend in get_backend_catalog(environment=environment)
        if backend.backend_id == EXPERIMENTAL_BACKEND_ID
    )

    assert native.configured is False
    assert native.available is False
    assert "configuration" in native.failed_gates


def test_revision_probe_rejects_a_locally_patched_checkout(tmp_path: Path) -> None:
    source = tmp_path / "source"
    source.mkdir()
    subprocess.run(["git", "init", "-q", str(source)], check=True)
    subprocess.run(["git", "-C", str(source), "config", "user.email", "test@example.com"], check=True)
    subprocess.run(["git", "-C", str(source), "config", "user.name", "Mocara Tests"], check=True)
    tracked = source / "tracked.txt"
    tracked.write_text("clean\n", encoding="utf-8")
    subprocess.run(["git", "-C", str(source), "add", "tracked.txt"], check=True)
    subprocess.run(["git", "-C", str(source), "commit", "-qm", "fixture"], check=True)

    clean_revision = read_git_revision(source)
    tracked.write_text("patched\n", encoding="utf-8")

    assert clean_revision is not None
    assert read_git_revision(source) is None


def test_promotion_gate_requires_compatibility_parity_performance_and_licensing() -> None:
    rejected = evaluate_promotion(
        {
            "source_revision": KIMODO_CPP_EXPECTED_REVISION,
            "windows_build_pass": True,
        }
    )

    assert rejected.approved is False
    assert rejected.failed_gates == [
        "soma-77-export",
        "general-constraints",
        "embedding-parity",
        "motion-parity",
        "latency",
        "vram",
        "license-review",
    ]

    approved = evaluate_promotion(
        {
            "source_revision": KIMODO_CPP_EXPECTED_REVISION,
            "windows_build_pass": True,
            "soma_77_export_pass": True,
            "constraints_pass": True,
            "embedding_parity_pass": True,
            "motion_parity_pass": True,
            "latency_ratio": 0.95,
            "vram_ratio": 0.90,
            "license_review_pass": True,
        }
    )

    assert approved.approved is True
    assert approved.failed_gates == []


def test_backend_endpoint_is_guarded_and_never_returns_paths() -> None:
    client = TestClient(create_app(runtime=KimodoRuntime(), warmup=False))

    assert client.get("/backends").status_code == 403
    response = client.get("/backends", headers=HEADERS)

    assert response.status_code == 200
    payload = response.json()
    assert [item["backend_id"] for item in payload["backends"]] == [
        PRODUCTION_BACKEND_ID,
        EXPERIMENTAL_BACKEND_ID,
    ]
    assert "path" not in json.dumps(payload).casefold()


def test_experimental_backend_selection_is_rejected_before_a_job_starts() -> None:
    runtime = KimodoRuntime(requested_backend=EXPERIMENTAL_BACKEND_ID)
    client = TestClient(create_app(runtime=runtime, warmup=False))

    health = client.get("/health")
    response = client.post(
        "/generate",
        json={"prompt": "walk"},
        headers=HEADERS,
    )

    assert "experiment" in health.json()["error"].casefold()
    assert response.status_code == 409
    assert "experiment" in response.json()["detail"].casefold()
    assert runtime.jobs == {}
