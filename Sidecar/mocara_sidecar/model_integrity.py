"""Pinned model-bundle download and integrity verification for Mocara."""

from __future__ import annotations

import hashlib
import importlib
import json
import os
import re
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
from typing import Any, Callable, Optional


_HEX40 = re.compile(r"^[0-9a-f]{40}$")
_HEX64 = re.compile(r"^[0-9a-f]{64}$")
_REPO_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,95}/[A-Za-z0-9][A-Za-z0-9._-]{0,95}$")
_MAX_MANIFEST_BYTES = 256 * 1024
_MAX_CACHE_BYTES = 2 * 1024 * 1024


class ModelManifestError(RuntimeError):
    """The shipped model contract or a downloaded artifact failed validation."""


@dataclass(frozen=True)
class VerifiedModelBundle:
    backend_id: str
    snapshots: dict[str, Path]
    public_record: dict[str, Any]

    def snapshot_for(self, role: str) -> Path:
        try:
            return self.snapshots[role]
        except KeyError as exc:
            raise ModelManifestError(f"model manifest has no repository role {role!r}") from exc


def default_manifest_path() -> Path:
    return Path(__file__).resolve().parents[2] / "Resources" / "model-manifest.json"


def default_verification_cache_path() -> Path:
    return Path.home() / ".cache" / "mocara" / "model-verification-v1.json"


def _require_string(value: Any, label: str, maximum: int = 512) -> str:
    if not isinstance(value, str) or not value or len(value) > maximum:
        raise ModelManifestError(f"{label} must be a non-empty string")
    return value


def _load_json_object(path: Path, maximum_bytes: int, label: str) -> dict[str, Any]:
    try:
        if path.stat().st_size > maximum_bytes:
            raise ModelManifestError(f"{label} exceeds {maximum_bytes} bytes")
        parsed = json.loads(path.read_text(encoding="utf-8"))
    except ModelManifestError:
        raise
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        raise ModelManifestError(f"unable to read {label}: {exc}") from exc
    if not isinstance(parsed, dict):
        raise ModelManifestError(f"{label} must contain a JSON object")
    return parsed


def _safe_relative_file(value: Any, label: str) -> str:
    text = _require_string(value, label, 512)
    path = PurePosixPath(text)
    if "\\" in text or path.is_absolute() or any(part in {"", ".", ".."} for part in path.parts):
        raise ModelManifestError(f"{label} must be a safe relative path")
    return path.as_posix()


def _validate_manifest(raw: dict[str, Any]) -> dict[str, Any]:
    if raw.get("schema_version") != 1:
        raise ModelManifestError("model manifest schema_version must be 1")
    bundle_id = _require_string(raw.get("bundle_id"), "bundle_id", 128)
    backend = raw.get("backend")
    if not isinstance(backend, dict):
        raise ModelManifestError("backend must be an object")
    backend_id = _require_string(backend.get("id"), "backend.id", 128)
    source_url = _require_string(backend.get("source_url"), "backend.source_url", 512)
    source_revision = _require_string(backend.get("source_revision"), "backend.source_revision", 40)
    if not _HEX40.fullmatch(source_revision):
        raise ModelManifestError("backend.source_revision must be an immutable 40-character Git SHA")
    backend_license = _require_string(backend.get("license"), "backend.license", 128)

    repositories = raw.get("repositories")
    if not isinstance(repositories, list) or not 1 <= len(repositories) <= 8:
        raise ModelManifestError("repositories must contain between 1 and 8 entries")
    roles: set[str] = set()
    validated_repositories: list[dict[str, Any]] = []
    for index, repository in enumerate(repositories):
        if not isinstance(repository, dict):
            raise ModelManifestError(f"repositories[{index}] must be an object")
        role = _require_string(repository.get("role"), f"repositories[{index}].role", 64)
        if role in roles:
            raise ModelManifestError(f"repository role {role!r} is duplicated")
        roles.add(role)
        repo_id = _require_string(repository.get("repo_id"), f"repositories[{index}].repo_id", 192)
        if not _REPO_ID.fullmatch(repo_id):
            raise ModelManifestError(f"repositories[{index}].repo_id is invalid")
        revision = _require_string(repository.get("revision"), f"repositories[{index}].revision", 40)
        if not _HEX40.fullmatch(revision):
            raise ModelManifestError(
                f"repositories[{index}].revision must be an immutable 40-character Git SHA"
            )
        license_id = _require_string(repository.get("license"), f"repositories[{index}].license", 128)
        required_files = repository.get("required_files")
        if not isinstance(required_files, list) or not 1 <= len(required_files) <= 64:
            raise ModelManifestError(
                f"repositories[{index}].required_files must contain between 1 and 64 entries"
            )
        seen_files: set[str] = set()
        validated_files: list[dict[str, Any]] = []
        for file_index, required in enumerate(required_files):
            if not isinstance(required, dict):
                raise ModelManifestError(
                    f"repositories[{index}].required_files[{file_index}] must be an object"
                )
            file_path = _safe_relative_file(
                required.get("path"),
                f"repositories[{index}].required_files[{file_index}].path",
            )
            if file_path in seen_files:
                raise ModelManifestError(f"required file {file_path!r} is duplicated")
            seen_files.add(file_path)
            size = required.get("size")
            if not isinstance(size, int) or isinstance(size, bool) or size <= 0:
                raise ModelManifestError(f"required file {file_path!r} has an invalid size")
            sha256 = _require_string(required.get("sha256"), f"required file {file_path!r} sha256", 64)
            if not _HEX64.fullmatch(sha256):
                raise ModelManifestError(f"required file {file_path!r} has an invalid sha256")
            kind = required.get("kind", "weight")
            if kind not in {"weight", "runtime"}:
                raise ModelManifestError(f"required file {file_path!r} has an invalid kind")
            validated_files.append(
                {"path": file_path, "size": size, "sha256": sha256, "kind": kind}
            )
        validated_repositories.append(
            {
                "role": role,
                "repo_id": repo_id,
                "revision": revision,
                "license": license_id,
                "required_files": validated_files,
            }
        )
    return {
        "schema_version": 1,
        "bundle_id": bundle_id,
        "backend": {
            "id": backend_id,
            "source_url": source_url,
            "source_revision": source_revision,
            "license": backend_license,
        },
        "repositories": validated_repositories,
    }


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _read_verification_cache(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    try:
        raw = _load_json_object(path, _MAX_CACHE_BYTES, "model verification cache")
    except ModelManifestError:
        return {}
    entries = raw.get("entries")
    return entries if isinstance(entries, dict) else {}


def _write_verification_cache(path: Path, entries: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + f".{os.getpid()}.tmp")
    temporary.write_text(
        json.dumps({"schema_version": 1, "entries": entries}, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    try:
        temporary.chmod(0o600)
    except OSError:
        pass
    temporary.replace(path)


def prepare_verified_bundle(
    manifest_path: Path,
    *,
    verification_cache_path: Optional[Path] = None,
    snapshot_download_fn: Optional[Callable[..., str]] = None,
) -> VerifiedModelBundle:
    """Download immutable snapshots and verify every declared weight exactly once."""
    manifest_path = Path(manifest_path)
    manifest = _validate_manifest(
        _load_json_object(manifest_path, _MAX_MANIFEST_BYTES, "model manifest")
    )
    if snapshot_download_fn is None:
        from huggingface_hub import snapshot_download

        snapshot_download_fn = snapshot_download
    cache_path = Path(verification_cache_path or default_verification_cache_path())
    verification_cache = _read_verification_cache(cache_path)
    snapshots: dict[str, Path] = {}
    public_repositories: list[dict[str, Any]] = []

    for repository in manifest["repositories"]:
        download_kwargs: dict[str, Any] = {
            "repo_id": repository["repo_id"],
            "revision": repository["revision"],
            # A pinned runtime intentionally downloads only its declared dependency
            # closure. This also lets offline verification accept a cache that omits
            # model-card and training-only files.
            "allow_patterns": [item["path"] for item in repository["required_files"]],
        }
        if os.environ.get("MOCARA_MODEL_LOCAL_ONLY", "").strip().lower() in {"1", "true", "yes", "on"}:
            download_kwargs["local_files_only"] = True
        try:
            snapshot = Path(snapshot_download_fn(**download_kwargs)).resolve()
        except Exception as exc:
            raise ModelManifestError(
                f"unable to resolve {repository['repo_id']} at {repository['revision']}: {exc}"
            ) from exc
        if not snapshot.is_dir():
            raise ModelManifestError(f"downloaded snapshot is not a directory: {snapshot}")
        snapshots[repository["role"]] = snapshot

        public_weights: list[dict[str, Any]] = []
        for required in repository["required_files"]:
            artifact = snapshot.joinpath(*PurePosixPath(required["path"]).parts)
            try:
                stat = artifact.stat()
            except OSError as exc:
                raise ModelManifestError(
                    f"required model file is missing: {repository['repo_id']}/{required['path']}"
                ) from exc
            if not artifact.is_file() or stat.st_size != required["size"]:
                raise ModelManifestError(
                    f"size mismatch for {repository['repo_id']}/{required['path']}: "
                    f"expected {required['size']}, got {stat.st_size}"
                )
            cache_key = (
                f"{repository['repo_id']}@{repository['revision']}:"
                f"{required['path']}:{required['sha256']}"
            )
            cached = verification_cache.get(cache_key)
            cache_matches = (
                isinstance(cached, dict)
                and cached.get("size") == stat.st_size
                and cached.get("mtime_ns") == stat.st_mtime_ns
                and cached.get("ctime_ns") == stat.st_ctime_ns
                and cached.get("inode") == stat.st_ino
                and cached.get("sha256") == required["sha256"]
            )
            actual_hash = required["sha256"] if cache_matches else _sha256_file(artifact)
            if actual_hash != required["sha256"]:
                raise ModelManifestError(
                    f"sha256 mismatch for {repository['repo_id']}/{required['path']}: "
                    f"expected {required['sha256']}, got {actual_hash}"
                )
            verification_cache[cache_key] = {
                "size": stat.st_size,
                "mtime_ns": stat.st_mtime_ns,
                "ctime_ns": stat.st_ctime_ns,
                "inode": stat.st_ino,
                "sha256": actual_hash,
            }
            public_weights.append(
                {
                    "path": required["path"],
                    "size": required["size"],
                    "sha256": required["sha256"],
                    "kind": required["kind"],
                }
            )
        public_files = public_weights
        public_repositories.append(
            {
                "role": repository["role"],
                "repo_id": repository["repo_id"],
                "revision": repository["revision"],
                "license": repository["license"],
                "files": public_files,
                "weights": [
                    {key: value for key, value in item.items() if key != "kind"}
                    for item in public_files
                    if item["kind"] == "weight"
                ],
            }
        )

    _write_verification_cache(cache_path, verification_cache)
    bundle_identity = {
        "schema_version": manifest["schema_version"],
        "bundle_id": manifest["bundle_id"],
        "backend": manifest["backend"],
        "repositories": public_repositories,
    }
    bundle_sha256 = hashlib.sha256(
        json.dumps(bundle_identity, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    public_record = {
        **bundle_identity,
        "bundle_sha256": bundle_sha256,
        "status": "verified",
    }
    return VerifiedModelBundle(
        backend_id=manifest["backend"]["id"],
        snapshots=snapshots,
        public_record=public_record,
    )


def materialize_text_adapter_overlay(
    source_snapshot: Path,
    foundation_snapshot: Path,
    overlays_root: Path,
) -> Path:
    """Create a local adapter view whose base model is an exact pinned snapshot."""
    source_snapshot = Path(source_snapshot).resolve()
    foundation_snapshot = Path(foundation_snapshot).resolve()
    if not source_snapshot.is_dir() or not foundation_snapshot.is_dir():
        raise ModelManifestError("text adapter and foundation snapshots must be directories")
    overlay_key = hashlib.sha256(
        f"{source_snapshot}\0{foundation_snapshot}".encode("utf-8")
    ).hexdigest()[:20]
    overlay = Path(overlays_root).resolve() / overlay_key
    overlay.mkdir(parents=True, exist_ok=True)
    adapter_config_found = False
    for source in source_snapshot.rglob("*"):
        if source.is_dir():
            continue
        relative = source.relative_to(source_snapshot)
        destination = overlay / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if relative.as_posix() == "adapter_config.json":
            adapter_config_found = True
            config = _load_json_object(source, _MAX_MANIFEST_BYTES, "text adapter config")
            config["base_model_name_or_path"] = str(foundation_snapshot)
            temporary = destination.with_suffix(destination.suffix + ".tmp")
            temporary.write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
            temporary.replace(destination)
            continue
        if destination.exists() or destination.is_symlink():
            continue
        try:
            os.link(source.resolve(), destination)
        except OSError:
            destination.symlink_to(source.resolve())
    if not adapter_config_found:
        raise ModelManifestError("text adapter snapshot has no adapter_config.json")
    return overlay


def configure_kimodo_loader(
    bundle: VerifiedModelBundle,
    *,
    overlays_root: Optional[Path] = None,
) -> None:
    """Point the pinned upstream Kimodo loader at verified local snapshots."""
    motion_snapshot = bundle.snapshot_for("motion")
    text_base_snapshot = bundle.snapshot_for("text-base-adapter")
    text_supervised_snapshot = bundle.snapshot_for("text-supervised-adapter")
    text_foundation_snapshot = bundle.snapshot_for("text-foundation")
    overlay = materialize_text_adapter_overlay(
        text_base_snapshot,
        text_foundation_snapshot,
        overlays_root or (Path.home() / ".cache" / "mocara" / "text-adapter-overlays"),
    )
    loader = importlib.import_module("kimodo.model.load_model")
    loader._resolve_hf_model_path = lambda _modelname: motion_snapshot
    preset = loader.TEXT_ENCODER_PRESETS["llm2vec"]["kwargs"]
    preset["base_model_name_or_path"] = str(overlay)
    preset["peft_model_name_or_path"] = str(text_supervised_snapshot)
