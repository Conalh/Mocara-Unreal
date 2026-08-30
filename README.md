<p align="center">
  <img src="docs/images/mocara-banner.png" alt="Mocara turns performance direction into editable skeletal motion" width="100%">
</p>

<h1 align="center">Mocara</h1>

<p align="center">
  <strong>Text to motion. Motion to an Unreal asset.</strong><br>
  Local character-motion generation, retargeting, and pose-directed refinement inside Unreal Engine 5.8.
</p>

<p align="center">
  <a href="https://github.com/Conalh/Mocara-Unreal/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/Conalh/Mocara-Unreal/actions/workflows/ci.yml/badge.svg"></a>
  <img alt="Beta 0.2.0" src="https://img.shields.io/badge/status-beta%200.2.0-8A63D2">
  <img alt="Unreal Engine 5.8" src="https://img.shields.io/badge/Unreal%20Engine-5.8-0E1128?logo=unrealengine&logoColor=white">
  <img alt="Windows and WSL2" src="https://img.shields.io/badge/platform-Windows%20%2B%20WSL2-0078D4?logo=windows11&logoColor=white">
  <img alt="NVIDIA CUDA" src="https://img.shields.io/badge/compute-NVIDIA%20CUDA-76B900?logo=nvidia&logoColor=white">
  <a href="LICENSE"><img alt="Apache License 2.0" src="https://img.shields.io/badge/license-Apache%202.0-blue.svg"></a>
</p>

<p align="center">
  <a href="#see-it-in-the-editor">See it</a> ·
  <a href="#how-mocara-works">How it works</a> ·
  <a href="#quick-start">Install</a> ·
  <a href="#animation-lab">Author motion</a> ·
  <a href="#development">Develop</a> ·
  <a href="docs/ARCHITECTURE.md">Architecture</a>
</p>

Mocara is a beta Unreal Engine editor plugin that turns a performance prompt into skeletal motion, imports the result, retargets it onto a UE5 mannequin or MetaHuman body, and gives the animator a focused in-editor lab for exact pose corrections.

> [!IMPORTANT]
> Mocara 0.2.0 currently supports **Windows 10/11 + WSL2 Ubuntu + NVIDIA GPUs**. This repository contains the plugin source—not Kimodo or Llama model weights, downloaded checkpoints, or Epic content.

## What you can do

| Generate and compare | Retarget and preview |
| --- | --- |
| Create body motion from plain-language performance direction with a reproducible seed, independent guidance controls, and one to four sequential candidates. | Import SOMA BVH motion, build or reuse target-specific IK assets, and preview it on UE5 mannequin or assembled MetaHuman bodies. |
| **Direct and refine** | **Reproduce and inspect** |
| Key exact bones, shape Ease In / Hold / Ease Out intervals, apply local AutoPose, or send pose constraints back through Kimodo. | Keep prompt, model, seed, guidance, candidate, precision, constraints, and artifact paths together in provenance JSON. |

The model service stays warm between requests, so Mocara can support an authoring loop rather than treating every generation as a disconnected batch job.

## See it in the editor

[![Mocara Animation Lab with generated motion, viewport controls, AutoPose controls, and the pose timeline](docs/images/animation-lab-overview.png)](docs/images/animation-lab-overview.png)

*Animation Lab keeps generation, preview, camera controls, bone selection, pose timing, and candidate provenance in one Unreal editor tab.*

| Two-hand relationship constraint | Per-bone pose interval |
| --- | --- |
| [![A generated running pose with Mocara's two-hand grip constraint and editable timeline](docs/images/two-hand-grip.png)](docs/images/two-hand-grip.png) | [![A keyed right-upper-arm pose with Ease In, Hold, and Ease Out visible on the timeline](docs/images/pose-constraint-timeline.png)](docs/images/pose-constraint-timeline.png) |
| Preserve a moving midpoint and the clip's existing hand spacing, then regenerate without baking in a sword-, rifle-, or tool-specific distance. | Rotate or move a supported bone, then shape when the correction enters, holds at full influence, and releases. |

## How Mocara works

```mermaid
flowchart LR
    Input["Performance prompt<br/>seed · guidance · duration"]:::input

    subgraph Unreal["Windows · Unreal Editor 5.8"]
        direction TB
        Slate["Mocara Slate tab<br/>prompting + Animation Lab"]:::unreal
        Client["C++ async client<br/>sidecar lifecycle"]:::unreal
        Import["SOMA BVH importer"]:::unreal
        Retarget["IK Rig retargeter<br/>mannequin or MetaHuman body"]:::unreal
        Lab["Preview · pose keys · AutoPose<br/>constraint regeneration"]:::author
        Asset[("UAnimSequence<br/>project asset")]:::output

        Slate --> Client
        Import --> Retarget --> Lab --> Asset
    end

    subgraph Linux["WSL2 · Ubuntu"]
        direction TB
        API["FastAPI sidecar<br/>127.0.0.1:8765"]:::sidecar
        Text["LLM2Vec + Meta Llama 3 8B<br/>text conditioning"]:::model
        Motion["Kimodo-SOMA-RP-v1.1<br/>motion generation"]:::model
        Files[("BVH · NPZ<br/>provenance JSON")]:::artifact

        API --> Text --> Motion --> Files
    end

    Input --> Slate
    Client -->|"bounded HTTP job"| API
    Files -->|"host Saved/Kimodo paths"| Import
    Lab -. "pose constraints + regenerate" .-> Client

    classDef input fill:#182033,stroke:#8a63d2,color:#ffffff,stroke-width:2px
    classDef unreal fill:#111827,stroke:#38bdf8,color:#e5f6ff
    classDef sidecar fill:#13251f,stroke:#34d399,color:#e8fff6
    classDef model fill:#24183a,stroke:#a78bfa,color:#f4efff
    classDef artifact fill:#2b2112,stroke:#f59e0b,color:#fff7e6
    classDef author fill:#192337,stroke:#60a5fa,color:#eff6ff,stroke-width:2px
    classDef output fill:#2a1d18,stroke:#fb923c,color:#fff4ed,stroke-width:2px
```

The language model is **not** the animation generator:

1. **Meta Llama 3 8B + LLM2Vec** encode the prompt into conditioning vectors.
2. **Kimodo-SOMA-RP-v1.1** uses that conditioning, the seed, guidance, and optional pose constraints to synthesize skeletal motion.
3. **Mocara** owns the Windows/WSL bridge, import, IK retargeting, preview, local editing, constraint feedback, and Unreal asset creation.

The generation path is local to one workstation. Setup downloads the required code and gated model assets from their original providers; after they are installed, prompts and generated motion travel only between Unreal and the loopback WSL sidecar.

The complete component boundaries, data contracts, extension seams, and verification layers live in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## The authoring loop

```text
Generate broad motion
        ↓
Inspect it on the target character
        ↓
Key exact body corrections
        ↓
Apply locally ───────── or ───────── Regenerate with constraints
        ↓                                      ↓
Compare candidates and provenance
        ↓
Save the chosen UAnimSequence
```

This split keeps creative iteration fast:

- **Local AutoPose** deterministically bakes keyed corrections onto the loaded clip without another model request.
- **Constraint regeneration** turns keyed body, root, or end-effector targets into a new Kimodo request when the motion itself should adapt.
- **Candidate comparison** derives deterministic variation seeds and generates sequentially, keeping peak VRAM at the proven single-motion level.
- **Two-Hand Grip** describes a relationship between both wrists while preserving the selected clip's broad motion.

## Quick start

### Requirements

- Windows 10 or 11 with WSL2 and an Ubuntu distribution.
- Unreal Engine 5.8 and its Windows C++ build toolchain.
- An NVIDIA CUDA-capable GPU with a current Windows driver.
- Git available inside WSL.
- A Hugging Face account with gated access to:
  - [`meta-llama/Meta-Llama-3-8B-Instruct`](https://huggingface.co/meta-llama/Meta-Llama-3-8B-Instruct)
  - [`nvidia/Kimodo-SOMA-RP-v1.1`](https://huggingface.co/nvidia/Kimodo-SOMA-RP-v1.1)
- A Hugging Face read token kept outside the repository.

### Install the plugin

Clone this repository as `Mocara` inside the host project's `Plugins` directory:

```powershell
git clone https://github.com/Conalh/Mocara-Unreal.git <ProjectRoot>\Plugins\Mocara
```

Regenerate the host project's IDE files if needed, build its editor target, and open Unreal. Enable **Mocara**, **IK Rig**, and **Control Rig** if prompted, then restart the editor.

Open **Window → Mocara** and run the preflight before installing anything:

```text
Mocara.Doctor
```

When the reported prerequisites are ready, provision or repair the pinned WSL environment:

```text
Mocara.Setup
```

Setup installs `uv`, Python 3.10, a virtual environment, a GPU-matched PyTorch build, and a pinned Kimodo revision. It is rerunnable and safe to interrupt.

<details>
<summary><strong>Hugging Face token lookup</strong></summary>

Mocara looks for a token in this order:

1. `HF_TOKEN` in the Windows environment.
2. `HKCU\Environment\HF_TOKEN` in the Windows registry.
3. `%USERPROFILE%\.cache\huggingface\token`.

The plugin forwards the resolved token to WSL for setup and runtime access. Never commit a token, `.env` file, Hugging Face cache, or downloaded checkpoint.

</details>

### Generate your first clip

1. Open **Window → Mocara**.
2. Enter a visible body action, duration, and generation controls.
3. Press **Generate**. The first request in a session may wait while the models load.
4. Select a candidate and press **Load**.
5. Preview, key corrections, apply AutoPose or regenerate with constraints, then save the chosen animation.

Generated interchange files default to the host project's `Saved/Kimodo` directory. Unreal assets default to `/Game/Mocara/Generated` and `/Game/Mocara/Retarget`.

## Writing effective motion prompts

Describe what the body does, when it does it, and where its weight or attention goes. One clear action sequence is more useful than camera language or scene prose.

> A person takes three cautious steps forward, pauses, looks over the left shoulder, then turns the torso and runs forward with urgent, uneven strides.

Useful ingredients:

- **Action:** walk, vault, crouch, reach, turn, brace, swing.
- **Direction:** forward, diagonally left, clockwise, toward the floor.
- **Timing:** slowly, sudden stop, two beats, then accelerate.
- **Body mechanics:** bent knees, weight on the right foot, both hands at chest height.
- **Intent:** cautious, exhausted, celebratory—when it changes visible motion.

Mocara generates body motion. Camera work, scene rendering, facial performance, object physics, and prop simulation remain outside this pipeline. Use Animation Lab constraints for exact contact and keyed poses.

## Animation Lab

Animation Lab is the in-editor refinement surface:

- Select supported deform bones and rotate them; hands, feet, and hips also expose translation controls.
- Add pose keys and edit **Ease In**, **Hold**, and **Ease Out** numerically or directly on the timeline.
- Drag a complete interval, resize its three regions, nudge the selected key, or duplicate it.
- Apply local AutoPose to the current clip or regenerate from bounded Kimodo constraints.
- Load one of up to four deterministic candidates without importing every alternative.
- Preview on an assembled MetaHuman Blueprint or a UE5 mannequin target.
- Export the newest clip to FBX when the downstream workflow needs it.

Friendly labels such as “Left Upper Arm” stay separate from exact skeleton identifiers used for retargeting and constraints.

## Configuration

The main controls live under **Project Settings → Plugins → Mocara**.

| Setting | Default | Purpose |
| --- | --- | --- |
| `SidecarUrl` | `http://127.0.0.1:8765` | Loopback service address. Changing the port moves both the client and launcher. |
| `bAutoStartSidecar` | on | Lets the editor own the sidecar lifecycle. |
| `WslDistro` | `Ubuntu` | WSL distribution used for setup and generation. |
| `SidecarRoot` | empty | Optional development override; empty uses the installed plugin root. |
| `TargetMesh` | empty | Retarget destination; empty enables UE5 mannequin discovery. |
| `PreviewCharacterClass` | empty | Optional assembled character; empty enables compatible MetaHuman Blueprint discovery. |
| `FootPlantingStrength` | `1.0` | Main retarget foot-stability control. |
| `bAutoSaveGenerated` | on | Saves generated Unreal assets automatically. |

Runtime scripts also accept `MOCARA_ROOT`, `MOCARA_OUTPUT_DIR`, `MOCARA_PORT`, `MOCARA_PIDFILE`, `MOCARA_MODEL`, `TEXT_ENCODER_FP32`, `VENV`, `KIMODO_SRC`, `KIMODO_REF`, `KIMODO_URL`, and `UV_VERSION`.

### Local API boundary

The sidecar binds only to `127.0.0.1`. Every endpoint except `/health` requires an `X-Mocara-Client` header. That header blocks ordinary cross-origin webpages from driving the service; it is **not** authentication against other local processes.

Do not expose the sidecar port to a LAN or the internet. See [`SECURITY.md`](SECURITY.md) for the intended trust boundary.

## Development

Install the sidecar test dependencies and run the portable suite:

```powershell
py -3.11 -m pip install -e ".\Sidecar[test]"
py -3.11 -m pytest Tests -q
```

Run Unreal automation from the editor with the `Mocara.` filter, or from a clean host project:

```powershell
UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests Mocara.;Quit" -unattended -nop4 -nosplash -NullRHI -log
```

Package the distributable plugin with Unreal Automation Tool:

```powershell
RunUAT.bat BuildPlugin -Plugin=<Path>\Mocara.uplugin -Package=<OutputDirectory> -TargetPlatforms=Win64
```

### Repository map

```text
Mocara.uplugin                 plugin descriptor
Config/FilterPlugin.ini       BuildPlugin packaging boundary
Resources/                    pinned SOMA reference skeleton
Scripts/                      WSL doctor, setup, run, and stop tools
Sidecar/                      installable FastAPI service
Source/MocaraEditor/          editor-only Unreal C++ module and tests
Tests/                        portable Python and script contract tests
docs/                         architecture, design decision, and build brief
```

| Read next | Purpose |
| --- | --- |
| [Architecture](docs/ARCHITECTURE.md) | Concrete components, data flow, failure model, and extension seams. |
| [Build Your Own](docs/BUILD_YOUR_OWN.md) | Standalone implementation brief for another engineering agent or team. |
| [Contributing](CONTRIBUTING.md) | Scope, test layers, packaging proof, and pull-request expectations. |
| [Security](SECURITY.md) | Loopback trust boundary and vulnerability reporting. |
| [Public plugin boundary ADR](docs/decisions/0001-public-plugin-boundary.md) | Why the public repository is a standalone plugin rather than a host project export. |
| [Changelog](CHANGELOG.md) | Public release history and known acceptance limits. |

## Limitations

- Windows + WSL2 Ubuntu + NVIDIA only.
- Editor plugin only; it does not add a packaged-game runtime.
- One generation job at a time. Additional variations run sequentially.
- A single default pidfile means two projects should not share one sidecar instance.
- The loopback service trusts other local processes.
- Pose-editing keys are session state until baked or regenerated.
- MetaHuman support currently generates body animation, not facial animation.
- Built-in target profiles cover UE5 mannequin and MetaHuman body conventions. Other skeletons need a validated target profile and IK chain mapping.
- Every assembled MetaHuman body and clothing combination still needs a human viewport acceptance pass.

## License and third-party terms

Mocara source is licensed under the [Apache License 2.0](LICENSE). Kimodo source is fetched during setup and is not vendored here. Kimodo and Llama checkpoints, Unreal Engine, and Epic content have separate terms that Apache-2.0 does not replace. See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

---

<p align="center">
  Built for a local, inspectable animation workflow: direct the motion, see the evidence, keep the asset.
</p>
