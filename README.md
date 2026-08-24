# Mocara

Mocara is a beta Unreal Engine 5.8 editor plugin for generating character motion from text, previewing and editing it in the editor, and retargeting it onto UE5 mannequins or MetaHuman bodies.

It connects an Unreal C++/Slate authoring tool on Windows to a local [Kimodo](https://github.com/nv-tlabs/kimodo) motion-generation service in WSL2. The generated motion returns as BVH and NPZ data, then Unreal imports, retargets, previews, and saves the result as animation assets.

> **Status:** beta 0.2.0. Windows 10/11 + WSL2 Ubuntu + NVIDIA GPU only. This repository contains source, not model weights or Epic content.

## What the AI stack actually does

The language model is not the motion generator:

1. Meta Llama 3 8B, adapted through LLM2Vec, encodes the text prompt into conditioning vectors.
2. Kimodo-SOMA-RP-v1.1 uses those vectors, the seed, guidance values, and optional pose constraints to generate skeletal motion.
3. Mocara transports that motion into Unreal and owns import, retargeting, authoring, preview, and asset creation.

See [Architecture](docs/ARCHITECTURE.md) for the complete system and [Build Your Own](docs/BUILD_YOUR_OWN.md) for a standalone brief you can give to another coding agent.

## Architecture at a glance

```text
Windows / Unreal Editor                         WSL2 / Ubuntu / NVIDIA CUDA

Prompt + settings
      |
      v
Slate editor tab -> C++ HTTP client  ------->  FastAPI sidecar
      ^                127.0.0.1:8765          |  LLM2Vec + Llama 3 8B
      |                                         |  Kimodo-SOMA-RP-v1.1
      |                                         v
Animation Lab <- import + IK retarget  <-----  BVH + NPZ + provenance JSON
      |
      v
UAnimSequence assets in the host project
```

The sidecar stays warm so the text encoder and motion model do not reload for every generation. Mocara launches and stops it through ownership-checked scripts, keeps the HTTP service on loopback, and writes generated interchange files under the host project's `Saved` directory.

## Prerequisites

- Windows 10 or 11 with WSL2 and an Ubuntu distribution.
- An NVIDIA CUDA-capable GPU and current Windows driver.
- Unreal Engine 5.8 with its Windows C++ build toolchain.
- Git in WSL.
- A Hugging Face account with access to:
  - [`meta-llama/Meta-Llama-3-8B-Instruct`](https://huggingface.co/meta-llama/Meta-Llama-3-8B-Instruct)
  - [`nvidia/Kimodo-SOMA-RP-v1.1`](https://huggingface.co/nvidia/Kimodo-SOMA-RP-v1.1)
- A Hugging Face read token. Keep it out of the repository.

Mocara's setup script installs `uv`, Python 3.10, a virtual environment, a GPU-matched PyTorch build, and a pinned Kimodo source revision. Model downloads remain subject to their own licenses.

## Install from source

Clone this repository as `Mocara` inside the project's `Plugins` directory:

```powershell
git clone https://github.com/Conalh/Mocara-Unreal.git <ProjectRoot>\Plugins\Mocara
```

Regenerate the host project's IDE files if needed, build the editor target, then open Unreal. Enable **Mocara**, **IK Rig**, and **Control Rig** if the editor asks, and restart.

Open **Window -> Mocara**, then run:

```text
Mocara.Doctor
Mocara.Setup
```

`Mocara.Doctor` reports the WSL, GPU, token, Kimodo, and gated Llama prerequisites. `Mocara.Setup` installs or repairs the WSL environment and is safe to rerun.

### Hugging Face token lookup

Mocara checks, in order:

1. `HF_TOKEN` in the Windows environment.
2. `HKCU\Environment\HF_TOKEN` in the Windows registry.
3. `%USERPROFILE%\.cache\huggingface\token`.

The token is forwarded to WSL for setup and runtime access. Never commit a token, `.env` file, Hugging Face cache, or downloaded model checkpoint.

## Generate motion

Open **Window -> Mocara**, enter a motion prompt, choose duration and generation controls, and press **Generate**. The first request in an editor session may wait while the model loads. Mocara then polls the job, imports the returned BVH, creates or reuses IK retarget assets, and loads the result in Animation Lab.

For a MetaHuman, set **Project Settings -> Plugins -> Mocara -> Target Mesh** to a compatible body or mocap-body Skeletal Mesh. Leave it empty for automatic UE5 mannequin discovery. **Preview Character Class** can point at a specific assembled character; otherwise Mocara searches the host project for a compatible MetaHuman Blueprint.

Generated assets default to `/Game/Mocara/Generated` and `/Game/Mocara/Retarget` in the host project.

### Writing effective motion prompts

Describe visible body mechanics, timing, direction, and intent. One clear action usually works better than camera language or prose about mood.

```text
A person takes three cautious steps forward, pauses, looks over the left shoulder,
then turns the torso and runs forward with urgent, uneven strides.
```

Useful prompt ingredients:

- **Action:** walk, vault, crouch, reach, turn, brace, swing.
- **Direction:** forward, diagonally left, clockwise, toward the floor.
- **Timing:** slowly, sudden stop, two beats, then accelerate.
- **Body mechanics:** bent knees, weight on the right foot, both hands at chest height.
- **Intent:** cautious, exhausted, celebratory—when it changes the visible motion.

Avoid asking for scene rendering, facial acting, object simulation, or camera work. Mocara generates body motion. For exact contact or keyed poses, use Animation Lab constraints after the broad motion is generated.

## Animation Lab

- Select and rotate bones, place pose keys on the timeline, then regenerate with constraints.
- Apply local AutoPose to bake keys onto the current clip without another model call.
- Edit Ease In, Hold, and Ease Out intervals directly on the timeline.
- Generate one to four deterministic candidates from a base seed; candidates run sequentially to cap peak VRAM.
- Use **Two-Hand Grip + Regenerate** to express a shared moving grip relationship without hard-coding a weapon-specific hand distance.
- Each generation writes provenance JSON beside the BVH and NPZ files.

## Configuration

The main settings live under **Project Settings -> Plugins -> Mocara**.

| Setting | Default | Purpose |
| --- | --- | --- |
| `SidecarUrl` | `http://127.0.0.1:8765` | Loopback service address. Changing the port moves both client and launcher. |
| `bAutoStartSidecar` | on | Lets the editor own the sidecar lifetime. |
| `WslDistro` | `Ubuntu` | WSL distribution used for setup and generation. |
| `SidecarRoot` | empty | Optional development override; empty uses the installed plugin root. |
| `TargetMesh` | empty | Retarget destination; empty enables mannequin discovery. |
| `PreviewCharacterClass` | empty | Optional assembled character for Animation Lab. |
| `FootPlantingStrength` | `1.0` | Main retarget foot-stability control. |
| `bAutoSaveGenerated` | on | Saves generated Unreal assets automatically. |

Runtime scripts also accept `MOCARA_ROOT`, `MOCARA_OUTPUT_DIR`, `MOCARA_PORT`, `MOCARA_PIDFILE`, `MOCARA_MODEL`, `TEXT_ENCODER_FP32`, `VENV`, `KIMODO_SRC`, `KIMODO_REF`, `KIMODO_URL`, and `UV_VERSION`.

## Local API boundary

The sidecar binds only to `127.0.0.1`. Every endpoint except `/health` requires an `X-Mocara-Client` header. That header prevents ordinary cross-origin browser requests from driving the service; it is not authentication against other local processes.

Do not expose the port to a LAN or the internet. See [Security](SECURITY.md) for the intended trust boundary.

## Development

Install the sidecar test dependencies and run the portable suite:

```powershell
py -3.11 -m pip install -e ".\Sidecar[test]"
py -3.11 -m pytest Tests -q
```

Run Unreal automation tests in the editor with the `Mocara.` filter. A command-line host-project run looks like:

```powershell
UnrealEditor-Cmd.exe <Project>.uproject -ExecCmds="Automation RunTests Mocara.;Quit" -unattended -nop4 -nosplash -NullRHI -log
```

Package the plugin with Unreal Automation Tool:

```powershell
RunUAT.bat BuildPlugin -Plugin=<Path>\Mocara.uplugin -Package=<OutputDirectory> -TargetPlatforms=Win64
```

See [Contributing](CONTRIBUTING.md) for the full verification contract.

## Limitations

- Windows + WSL2 Ubuntu + NVIDIA only.
- Editor plugin only; it does not add a packaged-game runtime.
- One generation job at a time. Additional variations run sequentially.
- A single default pidfile means two projects should not share one sidecar instance.
- The loopback service trusts local processes.
- Pose-editing keys are session state until baked or regenerated.
- MetaHuman support currently generates body animation, not facial animation.
- Built-in target profiles cover UE5 mannequin and MetaHuman body conventions. Other skeletons require a validated target profile and IK chain mapping.
- Each assembled MetaHuman body and clothing combination still needs a human viewport acceptance pass.

## License and third-party terms

Mocara source is licensed under [Apache License 2.0](LICENSE). Kimodo source is fetched during setup and is not vendored here. Model checkpoints, Llama access, Unreal Engine, and Epic content have separate terms that Apache-2.0 does not replace. See [Third-Party Notices](THIRD_PARTY_NOTICES.md).
