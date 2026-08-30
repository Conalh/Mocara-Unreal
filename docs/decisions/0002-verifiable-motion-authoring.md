# ADR 0002: Make motion authoring verifiable and recoverable

- Status: Accepted
- Date: 2026-08-30
- Scope: P1a verified models, P1b persisted history, P1c prompt sequences

## Context

A model name alone does not identify the code, configuration, adapters, and weight
files that produced a motion. The previous editor also kept completed jobs only in
memory and represented a generation as one prompt over one duration. That made model
drift difficult to diagnose, made useful clips disappear from the UI after a restart,
and forced several motion beats into ambiguous prose.

## Decision

Mocara treats a generation as a durable, provenance-backed authoring record:

1. The packaged `Resources/model-manifest.json` pins the upstream Kimodo source,
   model, text adapter, base adapter, and foundation model to exact revisions and
   records SHA-256 plus byte size for every runtime file.
2. The sidecar verifies that dependency closure before importing Kimodo. It performs
   a full file hash on first use and reuses only a cache entry that still matches the
   file's size, modification time, change time, filesystem identity, and expected digest.
3. Provenance records the verified bundle digest, backend, exact generation controls,
   prompt segments, transition width, artifacts, output shape, and UTC creation time.
4. `GET /history` reconstructs completed jobs from provenance without generating
   again. The endpoint is client-header guarded, returns at most 50 jobs, reads at
   most 512 candidate files, rejects oversized or malformed JSON, and only admits
   non-symlink artifacts with the exact expected names inside the configured output
   directory.
5. `POST /generate` keeps its single-prompt fields for compatible clients and adds an
   optional ordered sequence of at most 16 prompt/duration segments. Total duration
   remains capped at 30 seconds and total prompt text at 4,000 characters.

The generation output directory and its provenance files remain the history system of
record. A database is not introduced for this local, single-user editor workflow.

## Consequences

- A result can be traced to exact executable inputs rather than a moving repository
  name or model alias.
- First verification of the current bundle reads every declared runtime file; later
  starts are fast when the cache metadata still proves the same files were checked.
- The editor can restore and import prior generations after either process restarts.
- Prompt sequencing is explicit and reproducible while old clients remain valid.
- History intentionally covers generated artifacts, not unsaved interactive pose keys.
- Deleting or moving a generated artifact makes its history entry ineligible instead
  of returning an unsafe or stale path.
