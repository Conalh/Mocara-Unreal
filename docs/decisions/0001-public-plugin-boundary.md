# ADR 0001: Publish the standalone plugin, not the host project

- Status: accepted
- Date: 2026-08-24

## Context

Mocara was developed inside a private Unreal host project containing generated animation assets, editor configuration, local validation content, and unrelated authoring history. A public collaboration surface needs reproducible source without disclosing or relicensing that host-project material.

## Decision

The public repository starts from a fresh Git history whose root is the installable `Mocara` plugin directory. It contains the Unreal module, packaged sidecar, launcher scripts, required SOMA reference pose, tests, and public documentation.

It excludes the host `.uproject`, host `Config` and `Content`, generated assets, local test captures, downloaded Kimodo source, model checkpoints, credentials, caches, binaries, and the private repository's commit history.

## Consequences

- Contributors can clone directly into `<ProjectRoot>/Plugins/Mocara`.
- Plugin packaging has one explicit boundary and no dependency on the private host checkout.
- Public history cannot expose deleted private files from earlier commits.
- Host-project integration and viewport acceptance remain separate verification steps.
- Changes developed privately must be deliberately exported or reapplied to this repository, including tests and notices.
