# Changelog

All notable changes to Mocara are recorded here.

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
