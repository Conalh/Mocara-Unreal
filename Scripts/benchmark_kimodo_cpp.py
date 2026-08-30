#!/usr/bin/env python3
"""Run a bounded, explicit kimodo.cpp comparison and write path-free evidence.

This script does not clone, download, build, or promote anything.  It only runs
an explicitly supplied ``kmd-generate`` executable from Mocara's audited source
revision and records latency, output shape, and device-level GPU memory.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, BinaryIO, Optional, Sequence


PLUGIN_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PLUGIN_ROOT / "Sidecar"))

from mocara_sidecar.backends import (  # noqa: E402
    EXPERIMENTAL_BACKEND_ID,
    KIMODO_CPP_EXPECTED_REVISION,
    evaluate_promotion,
    read_git_revision,
)


MAX_CAPTURE_BYTES = 1024 * 1024
MAX_TEXT_BUNDLE_FILES = 512
_GENERATED_RE = re.compile(r"generated\s+(\d+)\s+frames\s+with\s+(\d+)\s+joints")


class BenchmarkError(RuntimeError):
    """An input, execution, or output failed the bounded benchmark contract."""


def build_generate_command(
    *,
    executable: Path,
    motion_model: Path,
    text_bundle: Path,
    prompt_file: Path,
    frames: int,
    steps: int,
    seed: int,
    output_dir: Path,
) -> list[str]:
    """Build the single-prompt CLI shape pinned at the audited native revision."""

    return [
        str(executable),
        str(motion_model),
        str(text_bundle),
        str(prompt_file),
        str(frames),
        str(steps),
        str(seed),
        str(output_dir),
    ]


def build_report(
    *,
    source_revision: str,
    platform_name: str,
    native_backend: str,
    frames: int,
    steps: int,
    runs: list[dict[str, Any]],
    gpu_memory_baseline_mib: Optional[float],
    gpu_memory_peak_mib: Optional[float],
    motion_model_sha256: str,
    text_bundle_sha256: str,
    baseline_latency_ms: Optional[float] = None,
    baseline_gpu_delta_mib: Optional[float] = None,
) -> dict[str, Any]:
    """Create a path-free evidence record that fails every unproven gate closed."""

    latencies = [float(run["elapsed_ms"]) for run in runs]
    joints = sorted({int(run["joints"]) for run in runs})
    successful = bool(runs) and all(
        int(run["frames"]) == frames
        and int(run["root_positions_bytes"]) == frames * 3 * 4
        and int(run["local_rotations_bytes"]) == frames * int(run["joints"]) * 4 * 4
        for run in runs
    )
    median_latency = statistics.median(latencies) if latencies else None
    peak_delta = None
    if gpu_memory_baseline_mib is not None and gpu_memory_peak_mib is not None:
        peak_delta = max(0.0, gpu_memory_peak_mib - gpu_memory_baseline_mib)
    latency_ratio = None
    if median_latency is not None and baseline_latency_ms and baseline_latency_ms > 0:
        latency_ratio = median_latency / baseline_latency_ms
    vram_ratio = None
    if peak_delta is not None and baseline_gpu_delta_mib and baseline_gpu_delta_mib > 0:
        vram_ratio = peak_delta / baseline_gpu_delta_mib

    promotion_evidence = {
        "source_revision": source_revision,
        # Executing a user-supplied binary does not prove that it came from the
        # supplied checkout. A controlled clean build must establish this gate.
        "windows_build_pass": False,
        # Joint count alone cannot prove the required mapping and relaxed-hand
        # semantics, even if a future native stream happens to contain 77 joints.
        "soma_77_export_pass": False,
        # The CLI at the pinned revision accepts no general constraint payload.
        "constraints_pass": False,
        # Standalone generation is not a comparison against captured Python
        # tensors, so these parity gates remain false until a fixture run proves them.
        "embedding_parity_pass": False,
        "motion_parity_pass": False,
        "latency_ratio": latency_ratio,
        "vram_ratio": vram_ratio,
        # Licence review is a human release decision, never inferred by a script.
        "license_review_pass": False,
    }
    promotion = evaluate_promotion(promotion_evidence)
    return {
        "schema_version": 1,
        "created_at": datetime.now(timezone.utc)
        .isoformat(timespec="seconds")
        .replace("+00:00", "Z"),
        "backend_id": EXPERIMENTAL_BACKEND_ID,
        "source_revision": source_revision,
        "platform": platform_name,
        "input_identity": {
            "motion_model_sha256": motion_model_sha256,
            "text_bundle_sha256": text_bundle_sha256,
        },
        "generation": {
            "native_backend": native_backend,
            "iterations": len(runs),
            "frames": frames,
            "steps": steps,
            "successful": successful,
            "windows_execution_pass": (
                successful and platform_name.casefold() == "windows"
            ),
            "observed_soma_77_joint_shape": successful and joints == [77],
            "joints": joints,
            "latency_ms": {
                "minimum": min(latencies) if latencies else None,
                "median": median_latency,
                "maximum": max(latencies) if latencies else None,
            },
        },
        "gpu_memory": {
            "scope": "whole-device",
            "baseline_mib": gpu_memory_baseline_mib,
            "peak_mib": gpu_memory_peak_mib,
            "peak_delta_mib": peak_delta,
        },
        "promotion_evidence": promotion_evidence,
        "promotion": promotion.to_public_dict(),
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Benchmark an explicit kimodo.cpp build without cloning, downloading, "
            "building, or enabling it as Mocara's production backend."
        )
    )
    parser.add_argument("--source", required=True, type=Path, help="Pinned kimodo.cpp checkout")
    parser.add_argument(
        "--executable", required=True, type=Path, help="Built kmd-generate executable"
    )
    parser.add_argument("--motion-model", required=True, type=Path, help="SOMA motion GGUF")
    parser.add_argument("--text-bundle", required=True, type=Path, help="LLM2Vec GGUF bundle")
    parser.add_argument("--report", required=True, type=Path, help="New JSON evidence report")
    parser.add_argument("--prompt", default="A person runs forward.")
    parser.add_argument("--frames", type=int, default=90)
    parser.add_argument("--steps", type=int, default=100)
    parser.add_argument("--seed", type=int, default=48271)
    parser.add_argument("--iterations", type=int, default=3)
    parser.add_argument("--timeout-seconds", type=float, default=900.0)
    parser.add_argument("--native-backend", choices=("cpu", "vulkan"), default="vulkan")
    parser.add_argument("--gpu-index", type=int, default=0)
    parser.add_argument("--baseline-latency-ms", type=float)
    parser.add_argument("--baseline-gpu-delta-mib", type=float)
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--force", action="store_true", help="Replace an existing report")
    return parser


def hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(8 * 1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def hash_directory(path: Path) -> str:
    """Hash a bounded dependency tree without emitting any local file names."""

    entries: list[Path] = []
    for entry in path.rglob("*"):
        entries.append(entry)
        if len(entries) > MAX_TEXT_BUNDLE_FILES:
            raise BenchmarkError(
                f"text bundle contains more than {MAX_TEXT_BUNDLE_FILES} entries"
            )
    entries.sort(key=lambda item: item.relative_to(path).as_posix())
    files: list[Path] = []
    for entry in entries:
        if entry.is_symlink():
            raise BenchmarkError("text bundle must not contain symbolic links")
        if entry.is_file():
            files.append(entry)
    if not files:
        raise BenchmarkError("text bundle contains no files")
    digest = hashlib.sha256()
    for entry in files:
        relative = entry.relative_to(path).as_posix().encode("utf-8")
        digest.update(len(relative).to_bytes(4, "big"))
        digest.update(relative)
        digest.update(entry.stat().st_size.to_bytes(8, "big"))
        digest.update(bytes.fromhex(hash_file(entry)))
    return digest.hexdigest()


def _checked_path(path: Path, kind: str) -> Path:
    raw = str(path)
    if any(character in raw for character in ("\0", "\r", "\n")):
        raise BenchmarkError(f"{kind} contains a control character")
    try:
        resolved = path.expanduser().resolve(strict=True)
    except OSError as exc:
        raise BenchmarkError(f"{kind} does not exist") from exc
    if kind in {"source", "text bundle"} and not resolved.is_dir():
        raise BenchmarkError(f"{kind} must be a directory")
    if kind in {"executable", "motion model"} and not resolved.is_file():
        raise BenchmarkError(f"{kind} must be a file")
    return resolved


def _validate_args(args: argparse.Namespace) -> tuple[Path, Path, Path, Path, Path]:
    source = _checked_path(args.source, "source")
    executable = _checked_path(args.executable, "executable")
    motion_model = _checked_path(args.motion_model, "motion model")
    text_bundle = _checked_path(args.text_bundle, "text bundle")
    if executable.stem.casefold() != "kmd-generate":
        raise BenchmarkError("executable must be named kmd-generate or kmd-generate.exe")
    if motion_model.suffix.casefold() != ".gguf":
        raise BenchmarkError("motion model must be a .gguf file")
    if not args.prompt.strip() or len(args.prompt) > 1000 or "\0" in args.prompt:
        raise BenchmarkError("prompt must contain 1 to 1000 non-NUL characters")
    if not 1 <= args.frames <= 900:
        raise BenchmarkError("frames must be between 1 and 900")
    if not 1 <= args.steps <= 500:
        raise BenchmarkError("steps must be between 1 and 500")
    if not 0 <= args.seed <= 2_147_483_647:
        raise BenchmarkError("seed must be between 0 and 2147483647")
    if not 1 <= args.iterations <= 10:
        raise BenchmarkError("iterations must be between 1 and 10")
    if not 1 <= args.timeout_seconds <= 3600:
        raise BenchmarkError("timeout-seconds must be between 1 and 3600")
    if not 0 <= args.gpu_index <= 31:
        raise BenchmarkError("gpu-index must be between 0 and 31")
    for value, name in (
        (args.baseline_latency_ms, "baseline-latency-ms"),
        (args.baseline_gpu_delta_mib, "baseline-gpu-delta-mib"),
    ):
        if value is not None and value <= 0:
            raise BenchmarkError(f"{name} must be positive")
    report = args.report.expanduser().resolve()
    if report.exists() and not args.force:
        raise BenchmarkError("report already exists; pass --force to replace it")
    return source, executable, motion_model, text_bundle, report


def _query_gpu_memory_mib(gpu_index: int) -> Optional[float]:
    try:
        result = subprocess.run(
            [
                "nvidia-smi",
                f"--id={gpu_index}",
                "--query-gpu=memory.used",
                "--format=csv,noheader,nounits",
            ],
            check=False,
            capture_output=True,
            text=True,
            timeout=2,
        )
        value = result.stdout.strip().splitlines()[0]
        return float(value) if result.returncode == 0 else None
    except (OSError, subprocess.SubprocessError, ValueError, IndexError):
        return None


def _drain_bounded(stream: BinaryIO, chunks: list[bytes]) -> None:
    retained = 0
    while True:
        chunk = stream.read(8192)
        if not chunk:
            break
        if retained < MAX_CAPTURE_BYTES:
            kept = chunk[: MAX_CAPTURE_BYTES - retained]
            chunks.append(kept)
            retained += len(kept)


def _monitor_gpu_memory(
    stop: threading.Event,
    gpu_index: int,
    peak: list[Optional[float]],
) -> None:
    while not stop.is_set():
        used = _query_gpu_memory_mib(gpu_index)
        if used is not None:
            peak[0] = used if peak[0] is None else max(peak[0], used)
        stop.wait(0.1)


def _run_once(
    command: list[str],
    *,
    working_directory: Path,
    native_backend: str,
    timeout_seconds: float,
    gpu_index: int,
    baseline_gpu_mib: Optional[float],
    expected_frames: int,
) -> tuple[dict[str, Any], Optional[float]]:
    environment = os.environ.copy()
    environment["KIMODO_BACKEND"] = native_backend
    process = subprocess.Popen(
        command,
        cwd=working_directory,
        env=environment,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    assert process.stdout is not None and process.stderr is not None
    stdout_chunks: list[bytes] = []
    stderr_chunks: list[bytes] = []
    readers = [
        threading.Thread(
            target=_drain_bounded, args=(process.stdout, stdout_chunks), daemon=True
        ),
        threading.Thread(
            target=_drain_bounded, args=(process.stderr, stderr_chunks), daemon=True
        ),
    ]
    for reader in readers:
        reader.start()
    stop_monitor = threading.Event()
    peak_gpu = [baseline_gpu_mib]
    monitor = threading.Thread(
        target=_monitor_gpu_memory,
        args=(stop_monitor, gpu_index, peak_gpu),
        daemon=True,
    )
    monitor.start()
    started = time.perf_counter()
    timed_out = False
    try:
        process.wait(timeout=timeout_seconds)
    except subprocess.TimeoutExpired:
        timed_out = True
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)
    elapsed_ms = (time.perf_counter() - started) * 1000.0
    stop_monitor.set()
    monitor.join(timeout=3)
    for reader in readers:
        reader.join(timeout=2)
    if timed_out:
        raise BenchmarkError(f"kmd-generate exceeded the {timeout_seconds:g}s timeout")
    stdout = b"".join(stdout_chunks).decode("utf-8", errors="replace")
    stderr = b"".join(stderr_chunks).decode("utf-8", errors="replace")
    if process.returncode != 0:
        detail = stderr.strip() or stdout.strip() or "no diagnostic output"
        raise BenchmarkError(f"kmd-generate exited {process.returncode}: {detail}")
    match = _GENERATED_RE.search(stdout)
    if match is None:
        raise BenchmarkError("kmd-generate did not report its frame and joint counts")
    output_dir = Path(command[-1])
    root_output = output_dir / "root_positions.f32"
    rotation_output = output_dir / "local_rotations_xyzw.f32"
    if not root_output.is_file() or not rotation_output.is_file():
        raise BenchmarkError("kmd-generate did not produce both raw motion streams")
    generated_frames = int(match.group(1))
    generated_joints = int(match.group(2))
    root_bytes = root_output.stat().st_size
    rotation_bytes = rotation_output.stat().st_size
    if generated_frames != expected_frames or not 1 <= generated_joints <= 256:
        raise BenchmarkError("kmd-generate reported an unexpected output shape")
    if (
        root_bytes != generated_frames * 3 * 4
        or rotation_bytes != generated_frames * generated_joints * 4 * 4
    ):
        raise BenchmarkError("kmd-generate wrote an unexpected raw-stream byte size")
    return (
        {
            "elapsed_ms": elapsed_ms,
            "frames": generated_frames,
            "joints": generated_joints,
            "root_positions_bytes": root_bytes,
            "local_rotations_bytes": rotation_bytes,
        },
        peak_gpu[0],
    )


def _write_report(path: Path, report: dict[str, Any], force: bool) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    try:
        temporary.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        if path.exists() and not force:
            raise BenchmarkError("report already exists; pass --force to replace it")
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        source, executable, motion_model, text_bundle, report_path = _validate_args(args)
        revision = read_git_revision(source)
        if revision != KIMODO_CPP_EXPECTED_REVISION:
            raise BenchmarkError(
                "source revision mismatch: benchmark only the audited "
                f"{KIMODO_CPP_EXPECTED_REVISION} revision"
            )
        with tempfile.TemporaryDirectory(prefix="mocara-kimodo-cpp-") as temporary_raw:
            temporary = Path(temporary_raw)
            prompt_file = temporary / "prompt.txt"
            prompt_file.write_text(args.prompt, encoding="utf-8")
            first_command = build_generate_command(
                executable=executable,
                motion_model=motion_model,
                text_bundle=text_bundle,
                prompt_file=prompt_file,
                frames=args.frames,
                steps=args.steps,
                seed=args.seed,
                output_dir=temporary / "run-01",
            )
            if args.dry_run:
                print(subprocess.list2cmdline(first_command))
                return 0
            motion_model_sha256 = hash_file(motion_model)
            text_bundle_sha256 = hash_directory(text_bundle)
            baseline_gpu = _query_gpu_memory_mib(args.gpu_index)
            peak_gpu = baseline_gpu
            runs: list[dict[str, Any]] = []
            for index in range(args.iterations):
                output_dir = temporary / f"run-{index + 1:02d}"
                command = build_generate_command(
                    executable=executable,
                    motion_model=motion_model,
                    text_bundle=text_bundle,
                    prompt_file=prompt_file,
                    frames=args.frames,
                    steps=args.steps,
                    seed=args.seed,
                    output_dir=output_dir,
                )
                run, run_peak = _run_once(
                    command,
                    working_directory=executable.parent,
                    native_backend=args.native_backend,
                    timeout_seconds=args.timeout_seconds,
                    gpu_index=args.gpu_index,
                    baseline_gpu_mib=baseline_gpu,
                    expected_frames=args.frames,
                )
                runs.append(run)
                if run_peak is not None:
                    peak_gpu = run_peak if peak_gpu is None else max(peak_gpu, run_peak)
        report = build_report(
            source_revision=revision,
            platform_name=platform.system(),
            native_backend=args.native_backend,
            frames=args.frames,
            steps=args.steps,
            runs=runs,
            gpu_memory_baseline_mib=baseline_gpu,
            gpu_memory_peak_mib=peak_gpu,
            motion_model_sha256=motion_model_sha256,
            text_bundle_sha256=text_bundle_sha256,
            baseline_latency_ms=args.baseline_latency_ms,
            baseline_gpu_delta_mib=args.baseline_gpu_delta_mib,
        )
        _write_report(report_path, report, args.force)
        print(f"wrote {report_path}")
        if not report["promotion"]["approved"]:
            print("native backend remains experimental; see promotion.failed_gates")
        return 0
    except BenchmarkError as exc:
        parser.error(str(exc))
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
