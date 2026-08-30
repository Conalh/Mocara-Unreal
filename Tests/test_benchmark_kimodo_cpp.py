from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[1]
SCRIPT_PATH = REPO_ROOT / "Scripts" / "benchmark_kimodo_cpp.py"


def _load_benchmark_module():
    spec = importlib.util.spec_from_file_location("benchmark_kimodo_cpp", SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_native_benchmark_command_matches_the_pinned_cli_contract(tmp_path: Path) -> None:
    benchmark = _load_benchmark_module()
    command = benchmark.build_generate_command(
        executable=tmp_path / "kmd-generate.exe",
        motion_model=tmp_path / "motion.gguf",
        text_bundle=tmp_path / "text-bundle",
        prompt_file=tmp_path / "prompt.txt",
        frames=90,
        steps=100,
        seed=48271,
        output_dir=tmp_path / "output",
    )

    assert command == [
        str(tmp_path / "kmd-generate.exe"),
        str(tmp_path / "motion.gguf"),
        str(tmp_path / "text-bundle"),
        str(tmp_path / "prompt.txt"),
        "90",
        "100",
        "48271",
        str(tmp_path / "output"),
    ]


def test_benchmark_report_is_path_free_and_cannot_promote_raw_soma30_output(
    tmp_path: Path,
) -> None:
    benchmark = _load_benchmark_module()
    report = benchmark.build_report(
        source_revision=benchmark.KIMODO_CPP_EXPECTED_REVISION,
        platform_name="Windows",
        native_backend="vulkan",
        frames=90,
        steps=20,
        runs=[
            {
                "elapsed_ms": 1200.0,
                "frames": 90,
                "joints": 30,
                "root_positions_bytes": 1080,
                "local_rotations_bytes": 43200,
            }
        ],
        gpu_memory_baseline_mib=800.0,
        gpu_memory_peak_mib=1400.0,
        motion_model_sha256="a" * 64,
        text_bundle_sha256="b" * 64,
    )

    serialized = json.dumps(report)
    assert str(tmp_path) not in serialized
    assert report["generation"]["successful"] is True
    assert report["generation"]["joints"] == [30]
    assert report["generation"]["windows_execution_pass"] is True
    assert report["promotion_evidence"]["windows_build_pass"] is False
    assert report["promotion_evidence"]["soma_77_export_pass"] is False
    assert report["promotion"]["approved"] is False
    assert "windows-build" in report["promotion"]["failed_gates"]
    assert "soma-77-export" in report["promotion"]["failed_gates"]
    assert report["gpu_memory"]["peak_delta_mib"] == 600.0
    assert report["input_identity"] == {
        "motion_model_sha256": "a" * 64,
        "text_bundle_sha256": "b" * 64,
    }


def test_content_hashes_change_with_inputs_but_never_include_paths(tmp_path: Path) -> None:
    benchmark = _load_benchmark_module()
    motion = tmp_path / "private-motion.gguf"
    text_bundle = tmp_path / "private-text-bundle"
    text_bundle.mkdir()
    component = text_bundle / "component.gguf"
    motion.write_bytes(b"motion-v1")
    component.write_bytes(b"text-v1")

    first_motion = benchmark.hash_file(motion)
    first_text = benchmark.hash_directory(text_bundle)
    motion.write_bytes(b"motion-v2")
    component.write_bytes(b"text-v2")

    assert benchmark.hash_file(motion) != first_motion
    assert benchmark.hash_directory(text_bundle) != first_text
    assert str(tmp_path) not in first_motion
    assert str(tmp_path) not in first_text


def test_text_bundle_hash_rejects_more_than_the_bounded_entry_count(
    tmp_path: Path,
) -> None:
    benchmark = _load_benchmark_module()
    text_bundle = tmp_path / "text-bundle"
    text_bundle.mkdir()
    for index in range(benchmark.MAX_TEXT_BUNDLE_FILES + 1):
        (text_bundle / f"{index:04d}.json").write_text("{}", encoding="utf-8")

    with pytest.raises(benchmark.BenchmarkError, match="more than"):
        benchmark.hash_directory(text_bundle)


def test_backend_policy_import_does_not_require_the_fastapi_server() -> None:
    code = (
        "import sys; "
        f"sys.path.insert(0, {str(SCRIPT_PATH.parents[1] / 'Sidecar')!r}); "
        "import mocara_sidecar.backends; "
        "assert 'mocara_sidecar.server' not in sys.modules"
    )

    result = subprocess.run([sys.executable, "-c", code], capture_output=True, text=True)

    assert result.returncode == 0, result.stderr


def test_native_runner_accepts_only_complete_raw_streams(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    benchmark = _load_benchmark_module()
    fake = tmp_path / "fake_generate.py"
    fake.write_text(
        "from pathlib import Path\n"
        "import sys\n"
        "out = Path(sys.argv[-1]); out.mkdir(parents=True)\n"
        "(out / 'root_positions.f32').write_bytes(bytes(2 * 3 * 4))\n"
        "(out / 'local_rotations_xyzw.f32').write_bytes(bytes(2 * 3 * 4 * 4))\n"
        "print('generated 2 frames with 3 joints')\n",
        encoding="utf-8",
    )
    output = tmp_path / "complete"
    monkeypatch.setattr(benchmark, "_query_gpu_memory_mib", lambda _: 100.0)

    run, peak = benchmark._run_once(
        [sys.executable, str(fake), str(output)],
        working_directory=tmp_path,
        native_backend="cpu",
        timeout_seconds=2.0,
        gpu_index=0,
        baseline_gpu_mib=90.0,
        expected_frames=2,
    )

    assert run["frames"] == 2
    assert run["joints"] == 3
    assert peak == 100.0


def test_native_runner_rejects_truncated_raw_streams(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    benchmark = _load_benchmark_module()
    fake = tmp_path / "fake_generate.py"
    fake.write_text(
        "from pathlib import Path\n"
        "import sys\n"
        "out = Path(sys.argv[-1]); out.mkdir(parents=True)\n"
        "(out / 'root_positions.f32').write_bytes(b'x')\n"
        "(out / 'local_rotations_xyzw.f32').write_bytes(b'x')\n"
        "print('generated 2 frames with 3 joints')\n",
        encoding="utf-8",
    )
    monkeypatch.setattr(benchmark, "_query_gpu_memory_mib", lambda _: None)

    with pytest.raises(benchmark.BenchmarkError, match="unexpected raw-stream byte size"):
        benchmark._run_once(
            [sys.executable, str(fake), str(tmp_path / "truncated")],
            working_directory=tmp_path,
            native_backend="cpu",
            timeout_seconds=2.0,
            gpu_index=0,
            baseline_gpu_mib=None,
            expected_frames=2,
        )


def test_benchmark_cli_requires_explicit_local_inputs() -> None:
    benchmark = _load_benchmark_module()
    parser = benchmark.build_parser()

    try:
        parser.parse_args([])
    except SystemExit as exc:
        assert exc.code == 2
    else:
        raise AssertionError("benchmark accepted implicit source, executable, or weight paths")
