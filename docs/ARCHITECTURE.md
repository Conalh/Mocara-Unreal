# Mocara architecture

This document describes the concrete architecture of Mocara 0.2.0. It is written so a collaborator can understand the system without access to its original host project.

## System purpose

Mocara turns a text description and optional pose constraints into an Unreal `UAnimSequence`. It is an editor-authoring system, not a runtime gameplay service:

```text
text + controls + pose constraints
             |
             v
LLM2Vec text conditioning (Meta Llama 3 8B base)
             |
             v
Kimodo diffusion motion generation on a SOMA skeleton
             |
             v
BVH + NPZ + provenance JSON
             |
             v
Unreal import -> IK Rig retarget -> preview/edit -> UAnimSequence
```

The distinction between the first two AI stages matters. Llama/LLM2Vec converts language into numerical conditioning. Kimodo is the generative model that synthesizes the skeletal motion.

## Deployment topology

Mocara spans two operating systems on one workstation.

### Windows process

`UnrealEditor.exe` loads the editor-only `MocaraEditor` C++ module. That module owns:

- the Slate window and Animation Lab;
- settings, validation, and user-visible state;
- WSL setup and sidecar process lifecycle;
- asynchronous HTTP submission and polling;
- BVH import and Unreal asset creation;
- IK Rig chain construction and retargeting;
- preview character discovery and viewport controls;
- local pose edits, constraint construction, and provenance display.

### WSL2 process

Ubuntu runs Uvicorn and `mocara_sidecar.server`. The sidecar owns:

- request schema validation and job state;
- loading Kimodo and the LLM2Vec text encoder;
- keeping the models warm on the NVIDIA GPU;
- serializing generation so peak VRAM stays bounded;
- converting prompt, seed, guidance, and constraints into a Kimodo call;
- writing BVH, NPZ, and provenance JSON;
- returning Windows-readable artifact paths.

### Filesystem exchange

The HTTP response carries metadata and paths, not large animation payloads. WSL writes generated files through `/mnt/<drive>/...` into the host project's `Saved/Kimodo` area. The sidecar converts WSL paths into normalized Windows drive paths before Unreal consumes them.

This split keeps ML dependencies out of the Unreal process and avoids serializing large skeletal arrays through JSON.

## Layer boundaries

### 1. Unreal presentation and session state

`SMocaraWindow`, `SMocaraViewport`, and `SMocaraTimeline` form the Slate authoring surface. They collect prompts and controls, render job status, manage candidate selection, display imported clips, and edit pose-key intervals.

The UI does not load Python or model weights. It calls typed C++ services.

### 2. Settings and target profiles

`UMocaraSettings` stores the loopback URL, WSL distribution, optional sidecar override, target mesh, preview character class, generated asset destinations, and retarget controls.

`FMocaraTargetProfile` and the bone map isolate skeleton-specific knowledge from the generic importer and UI. Built-in profiles cover the SOMA source, UE5 mannequin conventions, and MetaHuman body conventions. Supporting another humanoid should normally add a validated target profile rather than scattering bone-name conditionals through the system.

### 3. Sidecar lifecycle bridge

`FMocaraSidecarLauncher` resolves the installed plugin root through Unreal's plugin manager. It exports that root as `MOCARA_ROOT`, converts paths to WSL form, and streams packaged Bash scripts through a CR-stripping pipeline.

Streaming is deliberate: Windows checkouts can carry CRLF line endings, and a packaged plugin should never be modified in place just to run a script.

The launcher has four operations:

- `doctor.sh` inspects prerequisites without installing them;
- `setup_kimodo.sh` provisions the pinned environment;
- `run_sidecar.sh` starts the warm Uvicorn service;
- `stop_sidecar.sh` validates PID plus ownership token before stopping it.

The editor exports token, port, plugin root, and setup overrides explicitly. Scripts must not infer a developer checkout path.

### 4. Local HTTP contract

`FMocaraKimodoClient` talks to the sidecar asynchronously. The stable surface is:

- `GET /health` for liveness and loaded-model state;
- `POST /generate` to submit one bounded job;
- `GET /jobs/{job_id}` to poll progress and retrieve artifacts;
- `POST /shutdown` for editor-owned teardown.

Every endpoint except `/health` requires `X-Mocara-Client`. The sidecar rejects overlapping generation with `409`, rejects a request for a model different from the warm model, bounds prompt and constraint sizes, and retains only bounded completed-job history.

The client header prevents a normal cross-origin webpage from issuing privileged requests because the sidecar does not allow the required preflight. It is not authentication against another local process.

### 5. Text conditioning and motion generation

The default model is `Kimodo-SOMA-RP-v1.1`. Kimodo's LLM2Vec component adapts Meta Llama 3 8B into a bidirectional text encoder and emits the conditioning representation used by the diffusion motion model.

Generation inputs include:

- prompt and duration;
- reproducible base seed;
- text and constraint classifier-free guidance values;
- one to four candidate requests;
- in-place choice;
- optional full-body, root, or end-effector constraints;
- a constraint-preset identity for provenance.

Candidates are generated sequentially with deterministic derived seeds. This gives repeatable alternatives without multiplying peak VRAM.

The sidecar remains model-aware. If the client asks for a different model, it returns an explicit conflict instead of silently using whatever is already resident.

### 6. Interchange contract

Each successful candidate produces:

- **BVH:** hierarchical joint rotations and root translation using the SOMA skeleton;
- **NPZ:** Kimodo-native numerical output retained for downstream analysis or regeneration workflows;
- **provenance JSON:** exact prompt, resolved model, seed, guidance, candidate index, constraints, precision, settings, and artifact paths.

`Resources/somaskel77_standard_tpose.bvh` gives the importer a pinned reference pose that matches the generated SOMA hierarchy.

BVH is the stable bridge because Unreal can import it without linking the editor module to Python, PyTorch, or Kimodo internals.

### 7. Unreal import and retarget

`FMocaraBvhImporter` parses hierarchy, channels, frame timing, local transforms, and root motion, then builds transient or saved Unreal animation data.

`FMocaraRetargeter` constructs or resolves IK rigs and an IK retargeter for the source/target pair. Target identity is included in generated asset names so changing from a mannequin to a MetaHuman cannot silently reuse incompatible retarget assets.

The pipeline returns structured save status to the UI. A failed package save is user-visible instead of being reported as a successful generation.

### 8. Authoring and constraint feedback

Animation Lab separates broad generation from deterministic local editing:

- Pose keys store exact skeleton bone identifiers plus friendly labels.
- Ease In, Hold, and Ease Out define the influence interval.
- AutoPose applies keys locally to the loaded clip.
- Constraint regeneration converts keyed poses into bounded Kimodo constraints and starts a new generation.
- Two-Hand Grip samples the existing motion, preserves its moving midpoint and hand spacing, and expresses a relationship between both wrists rather than assuming a particular prop.

This feedback loop is the core authoring pattern:

```text
generate broad motion -> inspect -> key exact corrections ->
apply locally or regenerate with constraints -> compare candidates -> save
```

## Repository layout

```text
Mocara.uplugin                 Unreal plugin descriptor
Config/FilterPlugin.ini       Non-code files included by BuildPlugin
Resources/                    Pinned skeleton reference data
Scripts/                      WSL doctor/setup/run/stop plus manual PowerShell launcher
Sidecar/                      Installable FastAPI Python package
Source/MocaraEditor/          Editor-only Unreal C++ module and automation tests
Tests/                        Portable Python and script contract tests
docs/                         Architecture and contributor-facing decisions
```

The repository root is also the installable plugin root. There is no nested host Unreal project.

## Extension seams

Additions should attach at an existing boundary:

- new skeleton family -> target profile, bone map, and retarget validation;
- new motion model -> sidecar runtime adapter and explicit model contract;
- new constraint tool -> authoring state plus serialized constraint schema;
- new interchange format -> sidecar artifact type plus Unreal importer;
- remote or multi-user generation -> a new authenticated service boundary, not exposure of the loopback API;
- runtime-game use -> a separate runtime module and packaging/security design.

Keep model-specific Python out of the Unreal module and Unreal asset semantics out of the model service.

## Failure and recovery model

- Doctor is read-only and reports missing prerequisites.
- Setup is pinned, rerunnable, and safe to interrupt.
- Sidecar startup is health-polled because model load can take tens of seconds.
- HTTP calls stay asynchronous except the deliberately bounded editor-teardown path.
- One generation owns the runtime at a time; overlap receives `409`.
- PID cleanup is ownership checked and treats stale files as recoverable state.
- Generated assets include target identity and report save failures.
- Exact inputs are retained in provenance for reproduction and debugging.

## Verification layers

No single green check proves the full product. Mocara uses layered evidence:

1. Python unit and contract tests for validation, job lifecycle, scripts, paths, provenance, and public-package boundaries.
2. Unreal automation tests for C++ clients, pose editing, target profiles, preview resolution, retarget contracts, and viewport math.
3. Unreal C++ compilation against the target engine.
4. `BuildPlugin` from a clean source checkout.
5. Fresh host-project installation and import/retarget execution.
6. Human viewport acceptance for deformation, facing, foot behavior, and clothing interaction.

The final step cannot be replaced by headless tests for a new MetaHuman body or clothing combination.
