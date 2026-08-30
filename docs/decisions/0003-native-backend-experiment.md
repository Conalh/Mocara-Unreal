# ADR 0003: Keep kimodo.cpp behind measurable promotion gates

- Status: Accepted
- Date: 2026-08-30
- Scope: P2a native backend experiment
- Audited upstream revision: `f782a7236706749d1ffeabeed140eb14032d19f3`

## Context

[`localai-org/kimodo.cpp`](https://github.com/localai-org/kimodo.cpp) exposes a compact
C/C++ and GGML implementation with CPU/Vulkan execution, direct text and embedding
inputs, conditioned prompt sequences, and raw root/local-rotation output. Those are
useful future seams for a smaller native runtime.

At the audited revision, upstream explicitly does not provide general constraints,
SOMA 30-to-77-joint expansion, skinned output, or quantized models. Its CLI writes raw
float streams rather than Mocara's BVH contract. A clean Windows/MSVC 19.44 probe on
2026-08-30 configured successfully but failed to compile:

- `src/denoiser.cpp` and `src/llm_tokenizer.cpp` use `std::runtime_error` without the
  declaration MSVC requires;
- `src/llm_text_encoder.cpp` passes a Windows wide `filesystem::path::c_str()` to a
  GGUF API that requires `const char*`.

Enabling that implementation now would remove working authoring behavior and would
make an immature third-party port responsible for Mocara's production path.

## Decision

The NVIDIA Python/CUDA sidecar remains the only production generation backend.
`kimodo.cpp` is represented as an isolated experiment with three controls:

1. `GET /backends` returns a path-free capability catalog. The endpoint uses the same
   client-header guard as job and history APIs. Merely probing status never downloads,
   builds, or executes third-party code.
2. `Scripts/benchmark_kimodo_cpp.py` runs only an explicit checkout, executable,
   SOMA GGUF, and text bundle. The checkout must match the audited revision. The
   tracked checkout and submodules must also be clean. The harness bounds frames,
   steps, iterations, runtime, and captured process output; hashes the actual motion
   and text inputs; checks exact raw-stream byte sizes; samples whole-device NVIDIA
   memory; and writes an atomic JSON report containing no local paths. Executing a
   supplied binary is recorded separately and never claims the controlled-build gate.
3. Setting `MOCARA_BACKEND=localai-kimodo-cpp` is rejected before job creation.
   Configuration or a benchmark report does not silently enable native generation.

The native backend can be promoted only when one evidence record passes all of these
gates on the same machine and workload as the production baseline:

| Gate | Pass condition |
|---|---|
| Source | Exact audited revision |
| Windows build | Pinned source builds and executes with Mocara's Windows toolchain |
| SOMA export | Produces the required 77-joint presentation result |
| Constraints | Preserves Mocara's general constraint inputs |
| Embedding parity | Approved comparison against captured production embeddings |
| Motion parity | Approved comparison against captured production motion tensors |
| Latency | Median candidate latency divided by baseline latency is at most `1.0` |
| VRAM | Candidate peak delta divided by baseline peak delta is at most `1.0` |
| Licensing | Human review approves the selected code, text bundle, and motion weights |

Changing the audited revision, compatibility contract, or thresholds requires a new
decision record and verification. A locally patched checkout is useful for upstream
development but does not pass the exact-source gate.

## Consequences

- Mocara can track native progress without creating a second implicit authority.
- Paths to private checkouts, builds, weights, and evidence files never enter the HTTP
  catalog or benchmark report.
- The experiment is reproducible and useful even while promotion is blocked.
- Current production retains CUDA, 77-joint expansion, constraints, BVH generation,
  provenance, history, and editor behavior.
- Native inference remains unavailable until the build and compatibility gaps are
  closed and parity, performance, VRAM, and licensing are independently accepted.
