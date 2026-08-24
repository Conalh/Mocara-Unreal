# Build an app with this architecture

The block below is a standalone prompt for a coding agent. It assumes the agent has no access to Mocara, its repository, or prior conversation.

## Copy-paste agent brief

```text
Build an Unreal Engine 5.8 editor plugin that turns natural-language motion prompts into editable character animation. Use this architecture and preserve its boundaries.

PRODUCT OUTCOME

A user opens an editor tab, types a body-motion prompt, chooses duration/seed/guidance, generates one or more candidates, previews the result, edits exact skeletal poses on a timeline, optionally regenerates with those poses as constraints, retargets the chosen motion to a UE5 mannequin or MetaHuman body, and saves a UAnimSequence in the host project.

DEPLOYMENT

- Windows owns Unreal Editor, the C++ plugin, Slate UI, asset import, IK Rig retargeting, preview, local pose editing, and saved Unreal assets.
- WSL2 Ubuntu owns Python, CUDA/PyTorch, model downloads, LLM2Vec text conditioning, and Kimodo motion generation.
- A local FastAPI/Uvicorn sidecar connects the two processes over HTTP bound only to 127.0.0.1.
- Exchange generated animation through files in the host project's Saved directory: BVH for Unreal import, NPZ for numerical/native output, and JSON for provenance. Return normalized Windows paths in job results.

AI PIPELINE

- Use Meta-Llama-3-8B-Instruct through LLM2Vec as a text-conditioning encoder. It converts prompt language into vectors; it does not generate animation.
- Use Kimodo-SOMA-RP-v1.1 as the diffusion motion generator. It consumes text conditioning, duration, seed, classifier-free guidance, and optional skeletal constraints.
- Keep both models warm in the sidecar so they do not reload for each request.
- Generate candidate variations sequentially from deterministic derived seeds to bound VRAM.
- Do not embed model weights in the plugin or repository. Download them into the user's cache after they accept the applicable licenses.

UNREAL PLUGIN LAYERS

1. An editor-only C++ module with a Slate tab and Animation Lab.
2. Project settings for sidecar URL, WSL distro, optional sidecar root, target mesh, preview character class, output paths, auto-start, auto-save, and retarget controls.
3. A sidecar launcher that resolves the installed plugin root, converts Windows paths to WSL paths, forwards explicit environment variables, streams LF-normalized Bash scripts without modifying installed files, health-polls startup, and stops only a PID with a matching ownership token.
4. An asynchronous HTTP client for health, generate, job polling, and shutdown.
5. A BVH importer that validates the hierarchy and channels, creates source animation data, and makes asset-save failures visible.
6. An IK Rig/IK Retargeter layer with target profiles for the SOMA source, UE5 mannequin, and MetaHuman body conventions. Include target identity in generated rig, retargeter, and animation names so incompatible assets cannot be reused silently.
7. A preview resolver that can use an explicitly configured assembled character or discover a compatible MetaHuman Blueprint, while falling back to mannequin behavior when no target is selected.
8. A timeline/viewport authoring layer with exact bone identifiers, friendly display labels, pose keys, Ease In/Hold/Ease Out intervals, undo transactions, local AutoPose, and constraint regeneration.

LOCAL API

- GET /health: open liveness and loaded-model information.
- POST /generate: validate and submit one bounded job.
- GET /jobs/{id}: status, progress, candidate artifacts, and provenance.
- POST /shutdown: editor-owned teardown.
- Require an X-Mocara-Client header everywhere except /health to block ordinary cross-origin browser requests. Document that it is not authentication against local processes.
- Reject overlapping jobs with HTTP 409.
- Reject a requested model that differs from the warm model instead of silently substituting it.
- Bound prompt length, duration, diffusion steps, candidate count, constraint payload size, retained job history, and filesystem destinations.

GENERATION AND AUTHORING DATA

Record prompt, resolved model, seed, candidate seed/index, duration, text guidance, constraint guidance, text-encoder precision, in-place choice, constraint preset, exact constraints, and artifact paths in provenance JSON.

Represent a pose key as an exact source-skeleton bone plus desired transform and an influence interval. Convert keys into Kimodo constraints when regenerating. Also provide a deterministic local path that applies the same keys to the current clip without a model call.

For a two-hand grip constraint, sample the selected motion, preserve the existing moving hand midpoint and inter-hand spacing, and constrain both wrists relative to that shared moving frame. Do not assume a sword, rifle, or tool-specific grip distance.

REPOSITORY AND PACKAGING

- Make the repository root the installable plugin root containing the .uplugin, Config, Resources, Scripts, Sidecar, Source, Tests, and docs.
- Make the plugin self-contained for Unreal BuildPlugin packaging.
- Pin the Kimodo source revision and environment installer version.
- Force Bash scripts to LF through .gitattributes and also stream them through a CR-stripping launcher for packaged robustness.
- Exclude host project files, Epic assets, generated animations, credentials, caches, downloaded source trees, binaries, and model weights.
- Use Apache-2.0 for original source only after confirming all bundled resource notices. Keep model and engine licenses separate and explicit.

SECURITY MODEL

- Loopback only. Never expose this API to a network under the local trust design.
- Treat prompts, JSON, returned paths, BVH, NPZ, and model output as untrusted at their boundaries.
- Use asynchronous Unreal HTTP outside a bounded teardown path.
- Never kill by process-name scan; validate PID and ownership token.
- Never write tokens to source, logs, command output, or project content.
- A remote or multi-user version requires a new authenticated, authorized, isolated service design.

IMPLEMENTATION ORDER

1. Define API schemas, settings contracts, artifact paths, and threat boundary.
2. Implement a fake sidecar and Unreal async client with tests.
3. Implement WSL doctor/setup/run/stop scripts and ownership-safe lifecycle tests.
4. Load LLM2Vec + Llama text conditioning and Kimodo in a warm sidecar.
5. Produce BVH/NPZ/provenance and validate paths and bounds.
6. Import the SOMA BVH into Unreal and verify frame timing, root motion, and reference pose.
7. Add target profiles, IK rigs, retargeting, and asset naming isolation.
8. Add preview, timeline pose editing, local application, and constraint regeneration.
9. Add deterministic variations and candidate selection.
10. Prove source tests, Unreal automation, C++ build, clean BuildPlugin packaging, fresh-host install, and human viewport acceptance.

ACCEPTANCE

- A clean clone packages as a UE 5.8 Win64 plugin without relying on a developer path.
- Setup from a supported Windows/WSL/NVIDIA machine reaches a healthy warm sidecar using a user-provided Hugging Face token.
- A prompt creates deterministic candidate BVH/NPZ/JSON artifacts and a retargeted UAnimSequence.
- Mannequin and MetaHuman body targets cannot cross-reuse stale generated rig assets.
- Exact pose keys can be applied locally or sent back as bounded constraints.
- Shutdown cannot terminate an unrelated process.
- The API is loopback-only and rejects unauthorized browser-shaped requests, oversized inputs, overlapping jobs, and warm-model mismatches.
- No model weights, Epic content, credentials, private project files, or machine-specific paths are present in source or Git history.
- Automated checks are accompanied by a human viewport pass for each newly supported body/clothing combination.

Do not collapse the text encoder and motion generator into one vague "LLM" component. Do not move Unreal asset logic into Python. Do not load PyTorch inside Unreal. Keep the system modular at the process, API, interchange, retarget, and authoring boundaries above.
```

## Why this prompt is specific

It names responsibilities, data formats, trust boundaries, failure behavior, and acceptance evidence. That gives an agent enough architecture to build an equivalent system without requiring Mocara source, while leaving the implementation details open to repository-grounded engineering decisions.
