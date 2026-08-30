# Changelog

All notable changes to Mocara are recorded here.

## 0.3.0 - 2026-08-30

- Added a packaged model manifest with immutable repository revisions, runtime file
  sizes and SHA-256 hashes, cached re-verification, and a bundle digest in provenance.
- Added persistent generation history that restores settings and imports prior artifacts
  without regenerating, with bounded and path-contained provenance loading.
- Added up to 16 timed prompt segments with explicit transition frames across the Slate,
  C++ HTTP, sidecar, model-call, history, and provenance contracts.
- Added a guarded, path-free backend capability catalog.
- Added a bounded `kimodo.cpp` probe and benchmark harness while retaining the verified
  NVIDIA Python/CUDA backend as the only production path.
- Added architecture decisions for verifiable authoring and the native promotion gate.

### Native experiment status

The audited `kimodo.cpp` revision remains blocked from production. Its clean Windows
configuration reaches MSVC compilation but fails upstream portability checks, and it
does not yet preserve Mocara's general constraints, SOMA 77-joint presentation output,
BVH contract, parity evidence, or accepted latency/VRAM/licensing gates.

## 0.2.0 - 2026-08-24

First public source release.

- Added a self-contained Unreal Engine 5.8 editor plugin with its local WSL sidecar.
- Added text-to-motion generation with Kimodo-SOMA-RP-v1.1.
- Added deterministic seeds, independent guidance controls, one-to-four sequential candidates, and provenance JSON.
- Added BVH import, UE5 mannequin and MetaHuman body retargeting, preview character resolution, and generated-asset isolation by target identity.
- Added Animation Lab pose keys, easing intervals, local AutoPose, constraint regeneration, and a two-hand grip preset.
- Added ownership-checked sidecar startup and shutdown, loopback binding, and request validation.
- Added portable Python contract tests and Unreal automation tests.

### Known limitation

Automated source, runtime, build, import, and retarget checks do not replace a human viewport pass for every MetaHuman body and clothing combination.
