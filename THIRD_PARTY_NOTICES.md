# Third-party notices

Mocara source is Apache-2.0, but that license does not grant rights to every external tool, model, engine, or asset used with it.

## Kimodo

- Project: [NVIDIA Toronto AI Lab Kimodo](https://github.com/nv-tlabs/kimodo)
- Setup revision: `1aece8c124d73d255ceff5086d983b844c9f4e94`
- Source-code license at that revision: Apache License 2.0
- Distribution: Kimodo source is cloned by `Scripts/setup_kimodo.sh`; it is not vendored in this repository.

### SOMA reference pose

`Resources/somaskel77_standard_tpose.bvh` is derived byte-for-byte from Kimodo's `kimodo/assets/skeletons/somaskel77/somaskel77_standard_tpose.bvh` at the revision above.

- SHA-256: `3e8cdaf72d2b12a25450ff1af7da261175a830e186c1e07ff094f99ef604d85b`
- License: Apache License 2.0
- Copyright: its respective Kimodo contributors

The Apache License text is included in [LICENSE](LICENSE), and attribution is retained here and in [NOTICE](NOTICE).

## Model checkpoints and text encoder

No model weights are included in this repository. Setup and runtime may download:

- [Kimodo-SOMA-RP-v1.1](https://huggingface.co/nvidia/Kimodo-SOMA-RP-v1.1), under the license published on its model page.
- [Meta-Llama-3-8B-Instruct](https://huggingface.co/meta-llama/Meta-Llama-3-8B-Instruct), a gated dependency governed by Meta's published model terms.
- [LLM2Vec Meta-Llama adapters](https://huggingface.co/McGill-NLP/LLM2Vec-Meta-Llama-3-8B-Instruct-mntp), under the terms published with those repositories.

Users are responsible for accepting and complying with those terms. Apache-2.0 for Mocara and Kimodo source does not relicense any checkpoint.

## Optional kimodo.cpp experiment

Mocara includes a capability probe and benchmark harness for an explicitly supplied
[`localai-org/kimodo.cpp`](https://github.com/localai-org/kimodo.cpp) checkout at audited
revision `f782a7236706749d1ffeabeed140eb14032d19f3`.

- The external C++ source and GGML dependencies are not vendored, cloned, built, or
  downloaded by this repository.
- The upstream project's original source is published under Apache-2.0 at that revision.
- Native GGUF text and motion weights retain their own model terms and are not included.
- Configuring or benchmarking that checkout does not enable it as Mocara's production
  backend.

## Unreal Engine and Epic content

Mocara calls Unreal Engine APIs but does not distribute Unreal Engine source, binaries, sample project assets, mannequins, MetaHumans, or other Epic content. Install and use those components under Epic's applicable agreements.
