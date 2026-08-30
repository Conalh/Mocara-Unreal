"""Backend capability catalog and promotion policy for Mocara.

The production runtime remains NVIDIA's Python/CUDA implementation.  This module
only probes an explicitly configured ``kimodo.cpp`` checkout and evaluates a
separate evidence record; it never downloads, builds, or executes native code.
"""

from __future__ import annotations

import json
import math
import os
import subprocess
from collections.abc import Callable, Mapping
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Optional


PRODUCTION_BACKEND_ID = "nvidia-kimodo-python"
EXPERIMENTAL_BACKEND_ID = "localai-kimodo-cpp"
KIMODO_CPP_EXPECTED_REVISION = "f782a7236706749d1ffeabeed140eb14032d19f3"
MAX_EVIDENCE_BYTES = 256 * 1024
MAX_GIT_OUTPUT_BYTES = 64 * 1024

_GATE_DESCRIPTIONS = {
    "configuration": "Explicit source, executable, motion model, and text bundle are required.",
    "source-revision": "The native checkout must match Mocara's audited kimodo.cpp revision.",
    "windows-build": "The pinned native source must build and execute on Mocara's Windows toolchain.",
    "soma-77-export": "SOMA 30-joint output must expand to Mocara's 77-joint presentation skeleton.",
    "general-constraints": "The native adapter must preserve Mocara's general pose constraints.",
    "embedding-parity": "Native and production text embeddings need approved parity evidence.",
    "motion-parity": "Native and production motion output need approved parity evidence.",
    "latency": "Native median latency must be no slower than the production baseline.",
    "vram": "Native peak VRAM must be no higher than the production baseline.",
    "license-review": "The selected native code, text bundle, and motion weights need licence review.",
}


@dataclass(frozen=True)
class PromotionDecision:
    approved: bool
    failed_gates: list[str]

    def to_public_dict(self) -> dict[str, Any]:
        return {
            "approved": self.approved,
            "failed_gates": list(self.failed_gates),
            "blockers": [_GATE_DESCRIPTIONS[gate] for gate in self.failed_gates],
        }


@dataclass(frozen=True)
class BackendRecord:
    backend_id: str
    display_name: str
    tier: str
    active: bool
    configured: bool
    available: bool
    production_ready: bool
    source_revision: Optional[str]
    source_revision_match: bool
    capabilities: dict[str, bool]
    failed_gates: list[str]

    def to_public_dict(self) -> dict[str, Any]:
        return {
            "backend_id": self.backend_id,
            "display_name": self.display_name,
            "tier": self.tier,
            "active": self.active,
            "configured": self.configured,
            "available": self.available,
            "production_ready": self.production_ready,
            "source_revision": self.source_revision,
            "source_revision_match": self.source_revision_match,
            "capabilities": dict(self.capabilities),
            "failed_gates": list(self.failed_gates),
            "blockers": [_GATE_DESCRIPTIONS[gate] for gate in self.failed_gates],
        }


def evaluate_promotion(evidence: Mapping[str, Any]) -> PromotionDecision:
    """Evaluate the compatibility, parity, performance, and licence gates.

    Ratios compare the experimental result to the production Python/CUDA
    baseline on the same machine and workload.  A value at or below 1.0 passes.
    Missing, non-finite, or boolean values fail closed.
    """

    failed: list[str] = []
    if evidence.get("source_revision") != KIMODO_CPP_EXPECTED_REVISION:
        failed.append("source-revision")
    for field, gate in (
        ("windows_build_pass", "windows-build"),
        ("soma_77_export_pass", "soma-77-export"),
        ("constraints_pass", "general-constraints"),
        ("embedding_parity_pass", "embedding-parity"),
        ("motion_parity_pass", "motion-parity"),
    ):
        if evidence.get(field) is not True:
            failed.append(gate)
    for field, gate in (("latency_ratio", "latency"), ("vram_ratio", "vram")):
        value = evidence.get(field)
        if (
            isinstance(value, bool)
            or not isinstance(value, (int, float))
            or not math.isfinite(float(value))
            or float(value) < 0
            or float(value) > 1.0
        ):
            failed.append(gate)
    if evidence.get("license_review_pass") is not True:
        failed.append("license-review")
    return PromotionDecision(approved=not failed, failed_gates=failed)


def read_git_revision(source: Path) -> Optional[str]:
    """Return a clean, bounded Git revision without running checkout code."""

    try:
        result = subprocess.run(
            ["git", "-C", str(source), "rev-parse", "HEAD"],
            check=False,
            capture_output=True,
            text=True,
            timeout=2,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    revision = result.stdout.strip()
    if (
        result.returncode != 0
        or len(result.stdout) > MAX_GIT_OUTPUT_BYTES
        or len(revision) != 40
    ):
        return None
    try:
        int(revision, 16)
    except ValueError:
        return None
    try:
        status = subprocess.run(
            [
                "git",
                "-C",
                str(source),
                "status",
                "--porcelain=v1",
                "--untracked-files=no",
                "--ignore-submodules=none",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=3,
        )
    except (OSError, subprocess.SubprocessError):
        return None
    if (
        status.returncode != 0
        or len(status.stdout) > MAX_GIT_OUTPUT_BYTES
        or bool(status.stdout.strip())
    ):
        return None
    return revision.casefold()


def _configured_path(raw: Optional[str], expected_kind: str) -> Optional[Path]:
    if not raw or any(character in raw for character in ("\0", "\r", "\n")):
        return None
    path = Path(raw)
    if not path.is_absolute():
        return None
    try:
        if expected_kind == "directory" and path.is_dir():
            return path
        if expected_kind == "file" and path.is_file():
            return path
    except OSError:
        return None
    return None


def _load_evidence(environment: Mapping[str, str]) -> Mapping[str, Any]:
    path = _configured_path(environment.get("MOCARA_KIMODO_CPP_EVIDENCE"), "file")
    if path is None or path.is_symlink():
        return {}
    try:
        if path.stat().st_size > MAX_EVIDENCE_BYTES:
            return {}
        loaded = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError):
        return {}
    if not isinstance(loaded, dict):
        return {}
    nested = loaded.get("promotion_evidence", loaded)
    return nested if isinstance(nested, dict) else {}


def _native_record(
    environment: Mapping[str, str],
    revision_reader: Callable[[Path], Optional[str]],
) -> BackendRecord:
    source = _configured_path(environment.get("MOCARA_KIMODO_CPP_SOURCE"), "directory")
    executable = _configured_path(
        environment.get("MOCARA_KIMODO_CPP_EXECUTABLE"), "file"
    )
    motion_model = _configured_path(
        environment.get("MOCARA_KIMODO_CPP_MOTION_MODEL"), "file"
    )
    text_bundle = _configured_path(
        environment.get("MOCARA_KIMODO_CPP_TEXT_BUNDLE"), "directory"
    )
    configured = all(
        item is not None for item in (source, executable, motion_model, text_bundle)
    )
    revision = revision_reader(source) if source is not None else None
    revision_match = revision == KIMODO_CPP_EXPECTED_REVISION
    evidence = dict(_load_evidence(environment))
    # The checkout itself is authoritative for the source revision.  An evidence
    # file cannot claim a different checkout into the promotion decision.
    evidence["source_revision"] = revision
    promotion = evaluate_promotion(evidence)
    failed = list(promotion.failed_gates)
    if not configured:
        failed.insert(0, "configuration")
    failed = list(dict.fromkeys(failed))
    return BackendRecord(
        backend_id=EXPERIMENTAL_BACKEND_ID,
        display_name="kimodo.cpp native experiment",
        tier="experimental",
        active=False,
        configured=configured,
        available=configured and revision_match,
        production_ready=configured and revision_match and promotion.approved,
        source_revision=revision,
        source_revision_match=revision_match,
        capabilities={
            "text_prompt": True,
            "precomputed_embedding": True,
            "multi_prompt": True,
            "root_and_local_rotation_output": True,
            "general_constraints": False,
            "soma_77_joint_output": False,
            "bvh_output": False,
            "cuda": False,
            "vulkan": True,
        },
        failed_gates=failed,
    )


def get_backend_catalog(
    environment: Optional[Mapping[str, str]] = None,
    revision_reader: Callable[[Path], Optional[str]] = read_git_revision,
) -> list[BackendRecord]:
    env = os.environ if environment is None else environment
    requested = env.get("MOCARA_BACKEND", PRODUCTION_BACKEND_ID)
    production = BackendRecord(
        backend_id=PRODUCTION_BACKEND_ID,
        display_name="NVIDIA Kimodo Python/CUDA",
        tier="production",
        active=requested == PRODUCTION_BACKEND_ID,
        configured=True,
        available=True,
        production_ready=True,
        source_revision=None,
        source_revision_match=True,
        capabilities={
            "text_prompt": True,
            "precomputed_embedding": False,
            "multi_prompt": True,
            "root_and_local_rotation_output": True,
            "general_constraints": True,
            "soma_77_joint_output": True,
            "bvh_output": True,
            "cuda": True,
            "vulkan": False,
        },
        failed_gates=[],
    )
    return [production, _native_record(env, revision_reader)]


def backend_selection_error(requested_backend: str) -> Optional[str]:
    if requested_backend == PRODUCTION_BACKEND_ID:
        return None
    if requested_backend == EXPERIMENTAL_BACKEND_ID:
        return (
            "localai-kimodo-cpp is an experiment, not a selectable generation backend; "
            "run benchmark_kimodo_cpp.py and satisfy every promotion gate first"
        )
    return f"unsupported MOCARA_BACKEND value {requested_backend!r}"
