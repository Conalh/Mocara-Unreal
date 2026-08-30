from pathlib import Path
import json
import os
import sys
import threading

from fastapi.testclient import TestClient
import pytest
from pydantic import ValidationError


sys.path.insert(
    0,
    str(Path(__file__).resolve().parents[1] / "Sidecar"),
)

from mocara_sidecar.server import (
    CLIENT_HEADER,
    DEFAULT_MODEL,
    GeneratedArtifact,
    GenerateRequest,
    KimodoRuntime,
    PromptSegment,
    _Job,
    _build_prompt_timeline,
    _candidate_seed,
    _write_provenance,
    create_app,
)

#: Every non-/health endpoint requires this header to be present; the value is arbitrary.
HEADERS = {CLIENT_HEADER: "pytest"}


def test_single_prompt_keeps_punctuation_and_total_duration() -> None:
    request = GenerateRequest(prompt="runs forward. jumps over a rail.", duration=3.0)

    texts, frame_counts = _build_prompt_timeline(request, fps=30.0)

    assert texts == ["runs forward. jumps over a rail."]
    assert frame_counts == [90]


def test_multi_prompt_keeps_exact_text_and_per_segment_duration() -> None:
    request = GenerateRequest(
        prompt="runs forward.",
        duration=3.0,
        segments=[
            PromptSegment(prompt="runs forward.", duration=1.5),
            PromptSegment(prompt="vaults a rail, landing low.", duration=2.0),
            PromptSegment(prompt="recovers into a sprint.", duration=1.0),
        ],
        transition_frames=7,
    )

    texts, frame_counts = _build_prompt_timeline(request, fps=30.0)

    assert texts == [
        "runs forward.",
        "vaults a rail, landing low.",
        "recovers into a sprint.",
    ]
    assert frame_counts == [45, 60, 30]
    assert request.duration == 4.5
    assert request.transition_frames == 7


def test_multi_prompt_provenance_records_the_exact_timeline(tmp_path: Path) -> None:
    request = GenerateRequest(
        prompt="runs forward.",
        seed=44,
        segments=[
            {"prompt": "runs forward.", "duration": 1.5},
            {"prompt": "stops in a low guard.", "duration": 1.0},
        ],
        transition_frames=6,
    )
    artifact = GeneratedArtifact(
        0,
        44,
        tmp_path / "mocara_job_c01.bvh",
        tmp_path / "mocara_job_c01.npz",
    )

    path = _write_provenance(
        tmp_path / "mocara_job.json",
        request,
        [artifact],
        resolved_model="kimodo-soma-rp-v1.1",
        text_encoder_precision="float32",
    )

    saved = json.loads(path.read_text(encoding="utf-8"))
    assert saved["duration"] == 2.5
    assert saved["segments"] == [
        {"prompt": "runs forward.", "duration": 1.5},
        {"prompt": "stops in a low guard.", "duration": 1.0},
    ]
    assert saved["transition_frames"] == 6


@pytest.mark.parametrize(
    "kwargs",
    [
        {
            "segments": [
                {"prompt": "walk", "duration": 20.0},
                {"prompt": "run", "duration": 11.0},
            ]
        },
        {"segments": [{"prompt": "walk", "duration": 1.0}] * 17},
        {"segments": [{"prompt": "different first prompt", "duration": 1.0}]},
        {"transition_frames": 16},
    ],
)
def test_multi_prompt_contract_rejects_ambiguous_or_unbounded_sequences(
    kwargs: dict[str, object],
) -> None:
    with pytest.raises(ValidationError):
        GenerateRequest(prompt="walk", **kwargs)


def test_generate_request_accepts_reproducible_generation_controls() -> None:
    request = GenerateRequest(
        prompt="A person swings a two-handed sword.",
        seed=48271,
        text_guidance=2.5,
        constraint_guidance=3.0,
        candidate_count=3,
        constraint_preset="two-handed-grip",
    )

    assert request.seed == 48271
    assert request.text_guidance == 2.5
    assert request.constraint_guidance == 3.0
    assert request.candidate_count == 3
    assert request.constraint_preset == "two-handed-grip"


def test_candidate_seeds_are_individually_reproducible() -> None:
    assert [_candidate_seed(2_147_483_646, index) for index in range(4)] == [
        2_147_483_646,
        2_147_483_647,
        0,
        1,
    ]


def test_runtime_assigns_a_seed_when_an_older_client_omits_it() -> None:
    runtime = KimodoRuntime()
    runtime.generate = lambda job: setattr(job, "status", "done")  # type: ignore[method-assign]

    job = runtime.start_job(GenerateRequest(prompt="walk"))

    assert job is not None
    assert job.request.seed is not None
    assert 0 <= job.request.seed <= 2_147_483_647
    assert runtime.wait_until_idle(timeout=2)


@pytest.mark.parametrize(
    "field,value",
    [
        ("seed", -1),
        ("text_guidance", 10.1),
        ("constraint_guidance", -0.1),
        ("candidate_count", 0),
        ("candidate_count", 5),
    ],
)
def test_generate_request_bounds_expensive_generation_controls(field: str, value: object) -> None:
    with pytest.raises(ValidationError):
        GenerateRequest(prompt="walk", **{field: value})


def test_generate_request_rejects_unbounded_constraint_payload() -> None:
    oversized = [{"type": "fullbody", "padding": "x" * (2 * 1024 * 1024)}]

    with pytest.raises(ValidationError):
        GenerateRequest(prompt="walk", constraints=oversized)


def test_provenance_file_records_exact_prompt_controls_and_artifacts(tmp_path: Path) -> None:
    request = GenerateRequest(
        prompt="A person swings a two-handed sword.",
        duration=4.0,
        seed=7103,
        text_guidance=2.75,
        constraint_guidance=3.25,
        candidate_count=2,
        constraint_preset="two-handed-grip",
        constraints=[{"type": "root2d", "frame_indices": [0], "smooth_root_2d": [[0, 0]]}],
        in_place=True,
    )
    artifacts = [
        GeneratedArtifact(
            candidate_index=index,
            seed=7103 + index,
            bvh_path=tmp_path / f"mocara_job_c{index + 1:02d}.bvh",
            npz_path=tmp_path / f"mocara_job_c{index + 1:02d}.npz",
        )
        for index in range(2)
    ]

    provenance_path = _write_provenance(
        tmp_path / "mocara_job.json",
        request,
        artifacts,
        resolved_model="kimodo-soma-rp-v1.1",
        text_encoder_precision="float32",
    )

    saved = json.loads(provenance_path.read_text(encoding="utf-8"))
    assert saved["prompt"] == request.prompt
    assert saved["seed"] == request.seed
    assert saved["cfg"] == {"type": "separated", "text": 2.75, "constraint": 3.25}
    assert saved["candidate_count"] == 2
    assert saved["constraint_preset"] == "two-handed-grip"
    assert saved["text_encoder_precision"] == "float32"
    assert saved["backend"] == "nvidia-kimodo-python"
    assert saved["model_bundle"] is None
    assert saved["segments"] == []
    assert saved["transition_frames"] == 5
    assert saved["in_place"] is True
    assert saved["bvh_standard_tpose"] is True
    assert saved["constraints"] == request.constraints
    assert [artifact["candidate_index"] for artifact in saved["artifacts"]] == [0, 1]
    assert [artifact["seed"] for artifact in saved["artifacts"]] == [7103, 7104]


def _persist_history_job(
    output_dir: Path,
    job_id: str,
    prompt: str,
    created_at: str,
    seed: int,
) -> Path:
    stem = output_dir / f"mocara_{job_id}_c01"
    bvh_path = stem.with_suffix(".bvh")
    npz_path = stem.with_suffix(".npz")
    bvh_path.write_text("HIERARCHY\n", encoding="utf-8")
    npz_path.write_bytes(b"NPZ")
    artifact = GeneratedArtifact(0, seed, bvh_path, npz_path)
    return _write_provenance(
        output_dir / f"mocara_{job_id}.json",
        GenerateRequest(prompt=prompt, duration=2.0, seed=seed),
        [artifact],
        resolved_model="kimodo-soma-rp-v1.1",
        text_encoder_precision="float32",
        job_id=job_id,
        created_at=created_at,
        fps=30.0,
        num_frames=60,
    )


def test_persistent_history_returns_recent_valid_jobs_without_regenerating(tmp_path: Path) -> None:
    older = _persist_history_job(
        tmp_path, "111111111111", "walk forward", "2026-08-30T10:00:00Z", 10
    )
    newer = _persist_history_job(
        tmp_path, "222222222222", "vault a rail", "2026-08-30T11:00:00Z", 20
    )
    os.utime(older, (1, 1))
    os.utime(newer, (2, 2))
    runtime = KimodoRuntime(output_dir=tmp_path)
    client = TestClient(create_app(runtime=runtime, warmup=False))

    response = client.get("/history?limit=1", headers=HEADERS)

    assert response.status_code == 200
    jobs = response.json()["jobs"]
    assert [job["job_id"] for job in jobs] == ["222222222222"]
    assert jobs[0]["created_at"] == "2026-08-30T11:00:00Z"
    assert jobs[0]["status"] == "done"
    assert jobs[0]["provenance"]["prompt"] == "vault a rail"
    assert jobs[0]["provenance"]["seed"] == 20
    assert jobs[0]["fps"] == 30.0
    assert jobs[0]["num_frames"] == 60
    assert jobs[0]["artifacts"][0]["bvh_path"].endswith("mocara_222222222222_c01.bvh")


def test_persistent_history_skips_malformed_and_out_of_directory_artifacts(tmp_path: Path) -> None:
    (tmp_path / "mocara_brokenbroken.json").write_text("not json", encoding="utf-8")
    outside_bvh = tmp_path.parent / "outside.bvh"
    outside_npz = tmp_path.parent / "outside.npz"
    outside_bvh.write_text("HIERARCHY\n", encoding="utf-8")
    outside_npz.write_bytes(b"NPZ")
    _write_provenance(
        tmp_path / "mocara_333333333333.json",
        GenerateRequest(prompt="escape", seed=30),
        [GeneratedArtifact(0, 30, outside_bvh, outside_npz)],
        resolved_model="kimodo-soma-rp-v1.1",
        text_encoder_precision="float32",
        job_id="333333333333",
        created_at="2026-08-30T12:00:00Z",
        fps=30.0,
        num_frames=90,
    )
    runtime = KimodoRuntime(output_dir=tmp_path)

    response = TestClient(create_app(runtime=runtime, warmup=False)).get(
        "/history", headers=HEADERS
    )

    assert response.status_code == 200
    assert response.json() == {"jobs": []}


def test_persistent_history_skips_a_nonnumeric_fps_without_failing_request(
    tmp_path: Path,
) -> None:
    provenance_path = _persist_history_job(
        tmp_path, "444444444444", "walk", "2026-08-30T13:00:00Z", 40
    )
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
    provenance["fps"] = {"not": "a number"}
    provenance_path.write_text(json.dumps(provenance), encoding="utf-8")

    response = TestClient(
        create_app(runtime=KimodoRuntime(output_dir=tmp_path), warmup=False)
    ).get("/history", headers=HEADERS)

    assert response.status_code == 200
    assert response.json() == {"jobs": []}


def test_persistent_history_limit_is_bounded(tmp_path: Path) -> None:
    runtime = KimodoRuntime(output_dir=tmp_path)
    response = TestClient(create_app(runtime=runtime, warmup=False)).get(
        "/history?limit=51", headers=HEADERS
    )

    assert response.status_code == 422


def test_completed_job_status_keeps_legacy_paths_and_lists_all_candidates(tmp_path: Path) -> None:
    runtime = KimodoRuntime()
    request = GenerateRequest(prompt="A person swings a sword.", seed=9, candidate_count=2)
    job = _Job("candidatejob", request)
    job.status = "done"
    job.artifacts = [
        GeneratedArtifact(index, 9 + index, tmp_path / f"c{index}.bvh", tmp_path / f"c{index}.npz")
        for index in range(2)
    ]
    job.bvh_path = job.artifacts[0].bvh_path
    job.npz_path = job.artifacts[0].npz_path
    job.completed_candidates = 2
    runtime.jobs[job.job_id] = job

    response = TestClient(create_app(runtime=runtime, warmup=False)).get(
        f"/jobs/{job.job_id}", headers=HEADERS
    )

    assert response.status_code == 200
    state = response.json()
    assert state["bvh_path"] == str(job.artifacts[0].bvh_path)
    assert state["npz_path"] == str(job.artifacts[0].npz_path)
    assert [artifact["candidate_index"] for artifact in state["artifacts"]] == [0, 1]
    assert [artifact["seed"] for artifact in state["artifacts"]] == [9, 10]
    assert state["completed_candidates"] == 2
    assert state["provenance"]["prompt"] == request.prompt
    assert state["provenance"]["seed"] == 9


def test_generate_endpoint_rejects_overlapping_job() -> None:
    runtime = KimodoRuntime()
    started = threading.Event()
    release = threading.Event()

    def blocking_generate(job: _Job) -> None:
        job.status = "running"
        started.set()
        release.wait(timeout=5)
        job.status = "done"

    runtime.generate = blocking_generate  # type: ignore[method-assign]
    client = TestClient(create_app(runtime=runtime, warmup=False))

    first = client.post("/generate", json={"prompt": "walk"}, headers=HEADERS)
    assert first.status_code == 200
    assert started.wait(timeout=2)

    second = client.post("/generate", json={"prompt": "jump"}, headers=HEADERS)
    assert second.status_code == 409

    release.set()
    assert runtime.wait_until_idle(timeout=2)

    third = client.post("/generate", json={"prompt": "jump"}, headers=HEADERS)
    assert third.status_code == 200
    release.set()
    assert runtime.wait_until_idle(timeout=2)


def test_runtime_retains_only_bounded_finished_job_history() -> None:
    runtime = KimodoRuntime(max_retained_jobs=3)
    for index in range(5):
        job = _Job(str(index), GenerateRequest(prompt="walk"))
        job.status = "done"
        runtime.jobs[job.job_id] = job

    runtime.prune_finished_jobs()

    assert list(runtime.jobs) == ["2", "3", "4"]


def test_completed_jobs_are_pruned_without_waiting_for_another_request() -> None:
    runtime = KimodoRuntime(max_retained_jobs=3)

    def complete_immediately(job: _Job) -> None:
        job.status = "done"

    runtime.generate = complete_immediately  # type: ignore[method-assign]
    for _ in range(5):
        assert runtime.start_job(GenerateRequest(prompt="walk")) is not None
        assert runtime.wait_until_idle(timeout=2)

    assert len(runtime.jobs) == 3


@pytest.mark.parametrize(
    "script",
    [
        "Scripts/run_sidecar.sh",
    ],
)
def test_sidecar_scripts_bind_only_to_loopback(script: str) -> None:
    repo_root = Path(__file__).resolve().parents[1]
    contents = (repo_root / script).read_text(encoding="utf-8")

    assert "--host 127.0.0.1" in contents
    assert "--host 0.0.0.0" not in contents


def test_sidecar_port_is_configurable_and_defaults_to_8765() -> None:
    repo_root = Path(__file__).resolve().parents[1]
    script = repo_root / "Scripts" / "run_sidecar.sh"
    contents = script.read_text(encoding="utf-8")

    # The editor forwards MOCARA_PORT from its SidecarUrl setting; the script must use
    # it rather than the literal it used to hardcode.
    assert 'PORT="${MOCARA_PORT:-8765}"' in contents
    assert '--port "$PORT"' in contents
    assert "--port 8765" not in contents


def test_sidecar_uses_fp32_text_embeddings_by_default() -> None:
    repo_root = Path(__file__).resolve().parents[1]
    script = repo_root / "Scripts" / "run_sidecar.sh"
    contents = script.read_text(encoding="utf-8")

    assert 'TEXT_ENCODER_FP32="${TEXT_ENCODER_FP32:-1}"' in contents
    assert "export TEXT_ENCODER_FP32" in contents


@pytest.mark.parametrize(
    ("url", "expected"),
    [
        ("http://127.0.0.1:8765", "8765"),
        ("http://127.0.0.1:9000/", "9000"),
        ("http://[::1]:8765", "8765"),
        ("http://localhost", ""),
    ],
)
def test_launcher_port_parsing_covers_the_shapes_sidecarurl_can_take(url: str, expected: str) -> None:
    """The C++ helper splits from the right and strips the path; mirror its contract."""
    host_port = url.split("://", 1)[-1].split("/", 1)[0]
    head, sep, tail = host_port.rpartition(":")
    assert (tail if sep and tail.isdigit() else "") == expected


def _idle_app() -> TestClient:
    runtime = KimodoRuntime()
    runtime.generate = lambda job: setattr(job, "status", "done")  # type: ignore[method-assign]
    return TestClient(create_app(runtime=runtime, warmup=False))


@pytest.mark.parametrize(
    ("method", "path"),
    [
        ("post", "/generate"),
        ("get", "/jobs/deadbeef"),
        ("get", "/history"),
        ("post", "/shutdown"),
    ],
)
def test_mutating_endpoints_refuse_requests_without_the_client_header(method: str, path: str) -> None:
    """A browser cannot set a custom header cross-origin without a preflight this app
    never answers, so requiring one keeps a visited web page from driving the sidecar."""
    client = _idle_app()

    kwargs = {"json": {"prompt": "walk"}} if method == "post" else {}
    assert getattr(client, method)(path, **kwargs).status_code == 403


def test_health_stays_open_because_it_is_only_a_liveness_probe() -> None:
    response = _idle_app().get("/health")

    assert response.status_code == 200
    assert response.json()["text_encoder_precision"] == "float32"
    assert response.json()["backend"] == "nvidia-kimodo-python"
    assert response.json()["model_integrity"] == "pending"


def test_generate_refuses_a_model_the_warm_sidecar_is_not_serving() -> None:
    """The model field used to be validated and then silently ignored."""
    client = _idle_app()

    mismatched = client.post(
        "/generate", json={"prompt": "walk", "model": "some-other-model"}, headers=HEADERS
    )
    assert mismatched.status_code == 409
    assert DEFAULT_MODEL in mismatched.json()["detail"]

    matching = client.post(
        "/generate", json={"prompt": "walk", "model": DEFAULT_MODEL}, headers=HEADERS
    )
    assert matching.status_code == 200


def test_generate_accepts_the_resolved_model_name_reported_by_health() -> None:
    runtime = KimodoRuntime()
    runtime.resolved_name = DEFAULT_MODEL.lower()
    runtime.generate = lambda job: setattr(job, "status", "done")  # type: ignore[method-assign]
    client = TestClient(create_app(runtime=runtime, warmup=False))

    response = client.post(
        "/generate",
        json={"prompt": "walk", "model": runtime.resolved_name},
        headers=HEADERS,
    )

    assert response.status_code == 200
    assert runtime.wait_until_idle(timeout=2)
