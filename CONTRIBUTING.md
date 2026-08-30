# Contributing to Mocara

Mocara welcomes focused bug fixes, tests, documentation, target profiles, and authoring improvements.

## Before changing code

- Search existing issues and open a focused issue for a new feature or architectural change.
- Keep the plugin self-contained: runtime files belong under this repository root and must package with `Mocara.uplugin`.
- Do not add host-project content, Epic sample assets, model checkpoints, credentials, generated animations, or machine-specific paths.
- Preserve the Windows Unreal / WSL Linux boundary described in [Architecture](docs/ARCHITECTURE.md).

## Development setup

1. Install this repository as `<ProjectRoot>/Plugins/Mocara` in an Unreal Engine 5.8 C++ project.
2. Build the host project's editor target.
3. Install test dependencies:

   ```powershell
   py -3.11 -m pip install -e ".\Sidecar[test]"
   ```

4. Run the portable tests:

   ```powershell
   py -3.11 -m pytest Tests -q
   ```

5. Run the `Mocara.` Unreal automation tests and package the plugin with `BuildPlugin` before requesting review for C++ or packaging changes.

## Change rules

- Add a regression test before fixing runtime behavior.
- Treat reflected Unreal types, project settings, HTTP schemas, environment variables,
  model manifests, provenance fields, filenames, and generated asset paths as public
  contracts.
- Keep network access on loopback and validate all request and filesystem boundaries.
- Update `CHANGELOG.md`, public documentation, and third-party notices when behavior, requirements, dependencies, or licensing changes.
- Separate mechanical refactors from behavior changes.
- Keep experimental backends non-selectable until the documented build, compatibility,
  parity, performance, VRAM, and licensing gates pass through a reviewed decision.

## Commit sign-off

Contributions use the [Developer Certificate of Origin](https://developercertificate.org/). Sign each commit with:

```text
git commit -s
```

The sign-off certifies that you have the right to submit the contribution under this repository's Apache-2.0 license.

## Review checklist

- Portable Python tests pass.
- Shell scripts parse under Bash and keep LF endings.
- PowerShell scripts parse without errors.
- Unreal automation tests pass for affected behavior.
- A clean `BuildPlugin` package succeeds for C++ or package-surface changes.
- No secrets, private paths, model weights, host-project content, or generated artifacts enter the diff.
- Human viewport acceptance is recorded for visual retargeting or MetaHuman changes.
