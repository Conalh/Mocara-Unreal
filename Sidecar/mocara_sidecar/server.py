"""Warm Kimodo HTTP sidecar for the Mocara Unreal plugin."""

from __future__ import annotations

import json
import os
import secrets
import threading
import traceback
import uuid
from contextlib import asynccontextmanager
from dataclasses import dataclass
from pathlib import Path
from typing import Annotated, Any, Literal, Optional

os.environ.setdefault("TEXT_ENCODER_DEVICE", "cpu")

import warnings

warnings.filterwarnings(
    "ignore",
    message=r"Already found a `peft_config` attribute in the model",
)
warnings.filterwarnings(
    "ignore",
    message=r"To copy construct from a tensor",
)

from fastapi import Depends, FastAPI, Header, HTTPException
from pydantic import BaseModel, ConfigDict, Field, field_validator

#: Every mutating endpoint requires this header to be present. The *value* is not a
#: secret and is not checked -- presence alone is the control.
#:
#: What that buys: a browser cannot set a custom header on a cross-origin request without
#: first passing a CORS preflight, and this app answers no preflight. So a page the user
#: happens to visit while the editor is open cannot POST /shutdown or /generate, which it
#: otherwise could -- a bodyless POST is a "simple request" and needs no preflight at all.
#:
#: What it does not buy: any defence against other processes on this machine. They can
#: send the header trivially. That is accepted -- the sidecar is loopback-only, and a
#: local process can already read the environment and files it would need anyway.
#:
#: A real shared secret was considered and rejected: the editor would have to hand the
#: token to the sidecar at launch, and a sidecar that outlived an editor crash would then
#: reject the next session with 401 while still looking healthy.
CLIENT_HEADER = "X-Mocara-Client"

DEFAULT_OUTPUT_DIR = Path(
    os.environ.get(
        "MOCARA_OUTPUT_DIR",
        str(Path.home() / ".cache" / "mocara" / "generated"),
    )
)
DEFAULT_MODEL = os.environ.get("MOCARA_MODEL", "Kimodo-SOMA-RP-v1.1")
MAX_CONSTRAINT_PAYLOAD_BYTES = 2 * 1024 * 1024


def _env_flag(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() not in {"0", "false", "no", "off"}


TEXT_ENCODER_FP32 = _env_flag("TEXT_ENCODER_FP32", True)


class GenerateRequest(BaseModel):
    model_config = ConfigDict(str_strip_whitespace=True)

    prompt: str = Field(min_length=1, max_length=4000)
    duration: float = Field(default=3.0, ge=0.5, le=30)
    seed: Optional[int] = Field(default=None, ge=0, le=2_147_483_647)
    model: str = Field(default=DEFAULT_MODEL, min_length=1, max_length=128)
    diffusion_steps: int = Field(default=100, ge=1, le=500)
    constraints: Optional[list[dict[str, Any]]] = Field(default=None, max_length=256)
    text_guidance: float = Field(default=2.0, ge=0, le=10)
    constraint_guidance: float = Field(default=2.0, ge=0, le=10)
    candidate_count: int = Field(default=1, ge=1, le=4)
    constraint_preset: Optional[Literal["two-handed-grip"]] = None
    in_place: bool = False
    bvh_standard_tpose: bool = True

    @field_validator("constraints")
    @classmethod
    def validate_constraints(cls, constraints: Optional[list[dict[str, Any]]]):
        if constraints is None:
            return constraints
        encoded = json.dumps(constraints, separators=(",", ":")).encode("utf-8")
        if len(encoded) > MAX_CONSTRAINT_PAYLOAD_BYTES:
            raise ValueError(
                f"constraints exceed the {MAX_CONSTRAINT_PAYLOAD_BYTES}-byte request limit"
            )
        supported = {
            "root2d",
            "fullbody",
            "left-hand",
            "right-hand",
            "left-foot",
            "right-foot",
            "end-effector",
        }
        for constraint in constraints:
            if constraint.get("type") not in supported:
                raise ValueError("constraint type is missing or unsupported")
        return constraints


class GenerateAccepted(BaseModel):
    job_id: str
    status: str = "queued"


class CandidateArtifact(BaseModel):
    candidate_index: int
    seed: int
    bvh_path: str
    npz_path: str
    windows_bvh_path: str
    windows_npz_path: str


class GenerationProvenance(BaseModel):
    prompt: str
    duration: float
    seed: Optional[int]
    model: str
    diffusion_steps: int
    cfg_type: str = "separated"
    text_guidance: float
    constraint_guidance: float
    candidate_count: int
    constraint_preset: Optional[str]
    constraint_count: int
    text_encoder_precision: str
    in_place: bool
    bvh_standard_tpose: bool


class JobStatus(BaseModel):
    job_id: str
    status: str
    error: Optional[str] = None
    bvh_path: Optional[str] = None
    npz_path: Optional[str] = None
    windows_bvh_path: Optional[str] = None
    windows_npz_path: Optional[str] = None
    fps: Optional[float] = None
    num_frames: Optional[int] = None
    completed_candidates: int = 0
    artifacts: list[CandidateArtifact] = Field(default_factory=list)
    provenance: Optional[GenerationProvenance] = None
    provenance_path: Optional[str] = None
    windows_provenance_path: Optional[str] = None


class HealthResponse(BaseModel):
    ready: bool
    loading: bool
    cuda: bool
    device_name: Optional[str] = None
    model: Optional[str] = None
    text_encoder_device: str = Field(default_factory=lambda: os.environ.get("TEXT_ENCODER_DEVICE", "cpu"))
    text_encoder_precision: str = Field(
        default_factory=lambda: "float32" if TEXT_ENCODER_FP32 else "bfloat16"
    )
    error: Optional[str] = None


@dataclass(frozen=True)
class GeneratedArtifact:
    candidate_index: int
    seed: int
    bvh_path: Path
    npz_path: Path


class _Job:
    def __init__(self, job_id: str, request: GenerateRequest) -> None:
        self.job_id = job_id
        self.request = request
        self.status = "queued"
        self.error: Optional[str] = None
        self.bvh_path: Optional[Path] = None
        self.npz_path: Optional[Path] = None
        self.fps: Optional[float] = None
        self.num_frames: Optional[int] = None
        self.completed_candidates = 0
        self.artifacts: list[GeneratedArtifact] = []
        self.provenance_path: Optional[Path] = None


def _wsl_to_windows(path: Path) -> str:
    # Parse the WSL spelling explicitly instead of asking the host's pathlib to
    # resolve it. That keeps this conversion deterministic in Windows-side tests.
    normalized = str(path).replace("\\", "/")
    parts = normalized.split("/")
    if len(parts) >= 4 and parts[0] == "" and parts[1] == "mnt" and len(parts[2]) == 1:
        drive = parts[2].upper()
        return f"{drive}:/{'/'.join(parts[3:])}"
    return normalized


class KimodoRuntime:
    def __init__(self, max_retained_jobs: int = 128, text_encoder_fp32: bool = TEXT_ENCODER_FP32) -> None:
        self.lock = threading.Lock()
        self.model_load_lock = threading.Lock()
        self.jobs: dict[str, _Job] = {}
        self.active_job_id: Optional[str] = None
        self.max_retained_jobs = max(1, max_retained_jobs)
        self.idle_event = threading.Event()
        self.idle_event.set()
        self.model = None
        self.resolved_name: Optional[str] = None
        self.loading = False
        self.load_error: Optional[str] = None
        self.cuda = False
        self.device_name: Optional[str] = None
        self.device = "cpu"
        self.text_encoder_fp32 = text_encoder_fp32

    @property
    def text_encoder_precision(self) -> str:
        return "float32" if self.text_encoder_fp32 else "bfloat16"

    def ensure_loaded(self) -> None:
        # Warmup and the first Generate request can arrive together. Only one of
        # them may materialize the shared GPU model; the other waits and reuses it.
        with self.model_load_lock:
            with self.lock:
                if self.model is not None:
                    return
                if self.load_error:
                    raise RuntimeError(self.load_error)
                self.loading = True
            try:
                import torch
                from kimodo import load_model

                self.cuda = bool(torch.cuda.is_available())
                self.device = "cuda:0" if self.cuda else "cpu"
                if self.cuda:
                    self.device_name = torch.cuda.get_device_name(0)
                model, resolved = load_model(
                    DEFAULT_MODEL,
                    device=self.device,
                    default_family="Kimodo",
                    return_resolved_name=True,
                    text_encoder_fp32=self.text_encoder_fp32,
                )
                with self.lock:
                    self.model = model
                    self.resolved_name = resolved
                    self.loading = False
            except Exception as exc:
                with self.lock:
                    self.loading = False
                    self.load_error = f"{type(exc).__name__}: {exc}"
                raise

    def start_job(self, request: GenerateRequest) -> Optional[_Job]:
        with self.lock:
            if self.active_job_id is not None:
                active = self.jobs.get(self.active_job_id)
                if active is not None and active.status in {"queued", "running"}:
                    return None
                self.active_job_id = None
            self._prune_finished_jobs_locked()
            if request.seed is None:
                # Older clients omitted the seed. Give every accepted job a random base
                # seed anyway so its result is reproducible from provenance.
                request.seed = secrets.randbelow(2_147_483_648)
            job_id = uuid.uuid4().hex[:12]
            job = _Job(job_id, request)
            self.jobs[job_id] = job
            self.active_job_id = job_id
            self.idle_event.clear()

        thread = threading.Thread(target=self._run_job, args=(job,), name=f"kimodo-{job_id}", daemon=True)
        thread.start()
        return job

    def _run_job(self, job: _Job) -> None:
        try:
            self.generate(job)
        finally:
            with self.lock:
                if self.active_job_id == job.job_id:
                    self.active_job_id = None
                self._prune_finished_jobs_locked()
                self.idle_event.set()

    def get_job(self, job_id: str) -> Optional[_Job]:
        with self.lock:
            return self.jobs.get(job_id)

    def wait_until_idle(self, timeout: float) -> bool:
        return self.idle_event.wait(timeout)

    def prune_finished_jobs(self) -> None:
        with self.lock:
            self._prune_finished_jobs_locked()

    def _prune_finished_jobs_locked(self) -> None:
        finished = [
            job_id for job_id, job in self.jobs.items()
            if job.status not in {"queued", "running"}
        ]
        remove_count = max(0, len(self.jobs) - self.max_retained_jobs)
        for job_id in finished[:remove_count]:
            self.jobs.pop(job_id, None)

    def generate(self, job: _Job) -> None:
        job.status = "running"
        try:
            self.ensure_loaded()
            import torch
            from kimodo.constraints import load_constraints_lst
            from kimodo.exports.bvh import save_motion_bvh
            from kimodo.exports.motion_io import save_kimodo_npz
            from kimodo.skeleton import SOMASkeleton30, global_rots_to_local_rots
            from kimodo.tools import seed_everything

            req = job.request
            model = self.model
            fps = float(model.fps)
            texts, num_frames = _build_prompt_timeline(req, fps)

            constraint_lst = []
            if req.constraints:
                constraint_lst = load_constraints_lst(req.constraints, model.skeleton)

            DEFAULT_OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
            skeleton = model.skeleton
            if isinstance(skeleton, SOMASkeleton30):
                skeleton = skeleton.somaskel77.to(self.device)

            artifacts: list[GeneratedArtifact] = []
            for candidate_index in range(req.candidate_count):
                candidate_seed = _candidate_seed(req.seed, candidate_index)
                seed_everything(candidate_seed)
                # Generate candidates sequentially. This keeps peak VRAM equal to the
                # proven single-sample path on 16 GB cards, while each artifact remains
                # independently reproducible from its own recorded seed.
                output = model(
                    texts,
                    num_frames,
                    constraint_lst=constraint_lst,
                    cfg_weight=[req.text_guidance, req.constraint_guidance],
                    cfg_type="separated",
                    num_denoising_steps=req.diffusion_steps,
                    num_samples=1,
                    multi_prompt=True,
                    num_transition_frames=5,
                    post_processing=True,
                    return_numpy=True,
                )
                stem = DEFAULT_OUTPUT_DIR / f"mocara_{job.job_id}_c{candidate_index + 1:02d}"
                npz_path = stem.with_suffix(".npz")
                bvh_path = stem.with_suffix(".bvh")
                single = {
                    key: (
                        value[0]
                        if hasattr(value, "shape")
                        and len(getattr(value, "shape", ())) > 0
                        and value.shape[0] == 1
                        else value
                    )
                    for key, value in output.items()
                }
                save_kimodo_npz(str(npz_path), single)

                joints_pos = torch.from_numpy(output["posed_joints"][0]).to(self.device)
                joints_rot = torch.from_numpy(output["global_rot_mats"][0]).to(self.device)
                local_rot_mats = global_rots_to_local_rots(joints_rot, skeleton)
                root_positions = joints_pos[:, skeleton.root_idx, :]
                if req.in_place:
                    root_positions = root_positions.clone()
                    root_positions[:, 0] = 0
                    root_positions[:, 2] = 0

                save_motion_bvh(
                    str(bvh_path),
                    local_rot_mats,
                    root_positions,
                    skeleton=skeleton,
                    fps=fps,
                    standard_tpose=req.bvh_standard_tpose,
                )
                artifacts.append(
                    GeneratedArtifact(
                        candidate_index=candidate_index,
                        seed=candidate_seed,
                        bvh_path=bvh_path,
                        npz_path=npz_path,
                    )
                )
                job.completed_candidates = len(artifacts)

            job.artifacts = artifacts
            job.npz_path = artifacts[0].npz_path
            job.bvh_path = artifacts[0].bvh_path
            job.provenance_path = _write_provenance(
                DEFAULT_OUTPUT_DIR / f"mocara_{job.job_id}.json",
                req,
                artifacts,
                resolved_model=self.resolved_name or req.model,
                text_encoder_precision=self.text_encoder_precision,
            )
            job.fps = fps
            job.num_frames = int(output["posed_joints"].shape[1])
            job.status = "done"
        except Exception:
            job.status = "error"
            job.error = traceback.format_exc()


RUNTIME = KimodoRuntime()


def _build_prompt_timeline(request: GenerateRequest, fps: float) -> tuple[list[str], list[int]]:
    """Build the single prompt segment represented by the current editor UI."""
    return [request.prompt], [max(1, int(request.duration * fps))]


def _candidate_seed(base_seed: int, candidate_index: int) -> int:
    return (base_seed + candidate_index) % 2_147_483_648


def _provenance_for(
    request: GenerateRequest,
    resolved_model: str,
    text_encoder_precision: str,
) -> GenerationProvenance:
    return GenerationProvenance(
        prompt=request.prompt,
        duration=request.duration,
        seed=request.seed,
        model=resolved_model,
        diffusion_steps=request.diffusion_steps,
        text_guidance=request.text_guidance,
        constraint_guidance=request.constraint_guidance,
        candidate_count=request.candidate_count,
        constraint_preset=request.constraint_preset,
        constraint_count=len(request.constraints or []),
        text_encoder_precision=text_encoder_precision,
        in_place=request.in_place,
        bvh_standard_tpose=request.bvh_standard_tpose,
    )


def _write_provenance(
    path: Path,
    request: GenerateRequest,
    artifacts: list[GeneratedArtifact],
    resolved_model: str,
    text_encoder_precision: str,
) -> Path:
    provenance = _provenance_for(request, resolved_model, text_encoder_precision).model_dump()
    provenance["cfg"] = {
        "type": provenance.pop("cfg_type"),
        "text": provenance.pop("text_guidance"),
        "constraint": provenance.pop("constraint_guidance"),
    }
    provenance["artifacts"] = [
        {
            "candidate_index": artifact.candidate_index,
            "seed": artifact.seed,
            "bvh_path": str(artifact.bvh_path),
            "npz_path": str(artifact.npz_path),
            "windows_bvh_path": _wsl_to_windows(artifact.bvh_path),
            "windows_npz_path": _wsl_to_windows(artifact.npz_path),
        }
        for artifact in artifacts
    ]
    provenance["constraints"] = request.constraints or []
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(provenance, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)
    return path


def create_app(runtime: Optional[KimodoRuntime] = None, warmup: bool = True) -> FastAPI:
    active_runtime = runtime or RUNTIME

    def require_client_header(
        client: Annotated[Optional[str], Header(alias=CLIENT_HEADER)] = None,
    ) -> None:
        if not client:
            raise HTTPException(status_code=403, detail=f"{CLIENT_HEADER} header is required")

    guarded = [Depends(require_client_header)]

    @asynccontextmanager
    async def lifespan(_: FastAPI):
        if warmup:
            thread = threading.Thread(
                target=_safe_warmup,
                args=(active_runtime,),
                name="kimodo-warmup",
                daemon=True,
            )
            thread.start()
        yield

    app = FastAPI(title="Mocara Kimodo Sidecar", version="0.2.0", lifespan=lifespan)

    @app.get("/health", response_model=HealthResponse)
    def health() -> HealthResponse:
        return HealthResponse(
            ready=active_runtime.model is not None,
            loading=active_runtime.loading,
            cuda=active_runtime.cuda,
            device_name=active_runtime.device_name,
            model=active_runtime.resolved_name,
            text_encoder_precision=active_runtime.text_encoder_precision,
            error=active_runtime.load_error,
        )

    @app.post("/generate", response_model=GenerateAccepted, dependencies=guarded)
    def generate(req: GenerateRequest) -> GenerateAccepted:
        served_names = {DEFAULT_MODEL.casefold()}
        if active_runtime.resolved_name:
            served_names.add(active_runtime.resolved_name.casefold())
        if req.model.casefold() not in served_names:
            # The model is loaded once and kept warm; honouring a per-request model
            # would mean evicting it from the GPU. Refuse instead of silently
            # generating with something other than what was asked for.
            served_model = active_runtime.resolved_name or DEFAULT_MODEL
            raise HTTPException(
                status_code=409,
                detail=f"sidecar is serving {served_model!r}; restart it with MOCARA_MODEL to change",
            )
        job = active_runtime.start_job(req)
        if job is None:
            raise HTTPException(status_code=409, detail="a generation job is already running")
        return GenerateAccepted(job_id=job.job_id, status=job.status)

    @app.get("/jobs/{job_id}", response_model=JobStatus, dependencies=guarded)
    def job_status(job_id: str) -> JobStatus:
        job = active_runtime.get_job(job_id)
        if job is None:
            raise HTTPException(status_code=404, detail="unknown job")
        artifacts = [
            CandidateArtifact(
                candidate_index=artifact.candidate_index,
                seed=artifact.seed,
                bvh_path=str(artifact.bvh_path),
                npz_path=str(artifact.npz_path),
                windows_bvh_path=_wsl_to_windows(artifact.bvh_path),
                windows_npz_path=_wsl_to_windows(artifact.npz_path),
            )
            for artifact in job.artifacts
        ]
        return JobStatus(
            job_id=job.job_id,
            status=job.status,
            error=job.error,
            bvh_path=str(job.bvh_path) if job.bvh_path else None,
            npz_path=str(job.npz_path) if job.npz_path else None,
            windows_bvh_path=_wsl_to_windows(job.bvh_path) if job.bvh_path else None,
            windows_npz_path=_wsl_to_windows(job.npz_path) if job.npz_path else None,
            fps=job.fps,
            num_frames=job.num_frames,
            completed_candidates=job.completed_candidates,
            artifacts=artifacts,
            provenance=_provenance_for(
                job.request,
                active_runtime.resolved_name or job.request.model,
                active_runtime.text_encoder_precision,
            ),
            provenance_path=str(job.provenance_path) if job.provenance_path else None,
            windows_provenance_path=(
                _wsl_to_windows(job.provenance_path) if job.provenance_path else None
            ),
        )

    @app.post("/shutdown", dependencies=guarded)
    def shutdown() -> dict[str, bool]:
        def _die() -> None:
            import signal
            import time

            time.sleep(0.2)
            os.kill(os.getpid(), signal.SIGTERM)

        threading.Thread(target=_die, name="mocara-shutdown", daemon=True).start()
        return {"ok": True}

    return app


def _safe_warmup(runtime: KimodoRuntime) -> None:
    try:
        runtime.ensure_loaded()
    except Exception:
        traceback.print_exc()


app = create_app()
