# Mocara architecture

This document describes the concrete architecture of Mocara 0.3.0. It is written so a collaborator can understand the system without access to its original host project.

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
- immutable model-manifest resolution and runtime file verification;
- loading Kimodo and the LLM2Vec text encoder;
- keeping the models warm on the NVIDIA GPU;
- serializing generation so peak VRAM stays bounded;
- converting prompt, seed, guidance, and constraints into a Kimodo call;
- writing BVH, NPZ, and provenance JSON;
- reconstructing persisted generation history from bounded provenance records;
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
- `GET /history` to restore completed generation records without running the model;
- `GET /backends` for a path-free production/experiment capability catalog;
- `POST /shutdown` for editor-owned teardown.

Every endpoint except `/health` requires `X-Mocara-Client`. The sidecar rejects overlapping generation with `409`, rejects a request for a model different from the warm model, bounds prompt sequences and constraint sizes, and retains only bounded in-memory and persisted job history.

The client header prevents a normal cross-origin webpage from issuing privileged requests because the sidecar does not allow the required preflight. It is not authentication against another local process.

### 5. Verified text conditioning and motion generation

The default model is `Kimodo-SOMA-RP-v1.1`. Kimodo's LLM2Vec component adapts Meta Llama 3 8B into a bidirectional text encoder and emits the conditioning representation used by the diffusion motion model.

`Resources/model-manifest.json` identifies the upstream Kimodo source and every model
repository by immutable commit. It also records the byte size and SHA-256 of the exact
runtime configuration and weight files. Before importing Kimodo, the sidecar downloads
only that declared dependency closure and verifies it. A cache can skip re-reading a
file only when its expected digest, size, modification time, change time, and filesystem
identity still match. The public
bundle record contains repository identities and one aggregate digest, never cache paths.

Generation inputs include:

- one prompt/duration pair or an ordered sequence of up to 16 timed prompt beats;
- an explicit one-to-15-frame transition width between prompt beats;
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
- **provenance JSON:** exact prompt sequence, transition width, resolved model, verified
  bundle digest, backend, seed, guidance, candidate index, constraints, precision,
  output shape, creation time, settings, and artifact paths.

`Resources/somaskel77_standard_tpose.bvh` gives the importer a pinned reference pose that matches the generated SOMA hierarchy.

BVH is the stable bridge because Unreal can import it without linking the editor module to Python, PyTorch, or Kimodo internals.

### 7. Persistent generation history

Provenance files in the configured output directory are the local history system of
record. No database or cloud account is required. The loader reads no more than 512
candidate records per request, caps each JSON file at 1 MiB, validates the schema and
timestamps, and accepts only one-to-four exact `mocara_<job>_cNN` BVH/NPZ pairs. Records
or artifacts that are symlinks, malformed, missing, or outside the output directory are
ignored.

The Unreal client requests at most 50 entries, displays them by UTC creation time, and
restores the prompt timeline and generation settings before importing the chosen saved
artifact. It does not submit a new generation job.

### 8. Unreal import and retarget

`FMocaraBvhImporter` parses hierarchy, channels, frame timing, local transforms, and root motion, then builds transient or saved Unreal animation data.

`FMocaraRetargeter` constructs or resolves IK rigs and an IK retargeter for the source/target pair. Target identity is included in generated asset names so changing from a mannequin to a MetaHuman cannot silently reuse incompatible retarget assets.

The pipeline returns structured save status to the UI. A failed package save is user-visible instead of being reported as a successful generation.

### 9. Authoring and constraint feedback

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

### 10. Native backend experiment boundary

The production runtime is `nvidia-kimodo-python`. `localai-kimodo-cpp` is represented
only by a capability record and an explicit benchmark tool:

- the status probe validates configured files and a clean audited Git revision but never
  executes the checkout;
- HTTP responses expose booleans, revision identity, and failed gate names, never local
  checkout, executable, model, or evidence paths;
- the benchmark bounds iterations, frames, steps, time, captured output, and text-bundle
  entries, then validates raw stream sizes and writes input hashes plus measurements;
- requesting the experiment through `MOCARA_BACKEND` fails before a job is created.

Promotion requires a clean Windows build, correct SOMA 30-to-77 expansion, general
constraint support, approved embedding and motion parity, no latency or peak-VRAM
regression on the same workload, and a human licence review. See
[`decisions/0003-native-backend-experiment.md`](decisions/0003-native-backend-experiment.md).

## Repository layout

```text
Mocara.uplugin                 Unreal plugin descriptor
Config/FilterPlugin.ini       Non-code files included by BuildPlugin
Resources/                    Pinned skeleton reference data and model manifest
Scripts/                      WSL lifecycle tools plus the explicit native benchmark
Sidecar/                      Installable FastAPI Python package
Source/MocaraEditor/          Editor-only Unreal C++ module and automation tests
Tests/                        Portable Python and script contract tests
docs/                         Architecture and contributor-facing decisions
```

The repository root is also the installable plugin root. There is no nested host Unreal project.

## Extension seams

Additions should attach at an existing boundary:

- new skeleton family -> target profile, bone map, and retarget validation;
- new motion backend -> sidecar adapter, capability record, explicit model/interchange
  contract, and a reviewed promotion decision;
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
- Completed generation records survive process restarts; unsafe or stale records fail
  closed instead of returning arbitrary filesystem paths.
- An experimental backend configuration cannot silently replace production generation.

## Verification layers

No single green check proves the full product. Mocara uses layered evidence:

1. Python unit and contract tests for validation, model integrity, job lifecycle, history,
   backend gates, benchmark bounds, scripts, paths, provenance, and public-package boundaries.
2. Unreal automation tests for C++ clients, prompt sequences, history parsing, pose
   editing, target profiles, preview resolution, retarget contracts, and viewport math.
3. Unreal C++ compilation against the target engine.
4. `BuildPlugin` from a clean source checkout.
5. Fresh host-project installation and import/retarget execution.
6. Human viewport acceptance for deformation, facing, foot behavior, and clothing interaction.

The final step cannot be replaced by headless tests for a new MetaHuman body or clothing combination.
