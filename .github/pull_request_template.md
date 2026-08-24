## What changed

<!-- Describe the user-facing or architectural outcome. -->

## Verification

- [ ] `python -m pytest Tests -q`
- [ ] Bash scripts parse and retain LF endings
- [ ] Unreal `Mocara.` automation tests for affected C++ behavior
- [ ] Clean UE 5.8 `BuildPlugin` package for source/package changes
- [ ] Human viewport acceptance for visual retargeting changes, or not applicable
- [ ] No credentials, model weights, Epic content, host-project files, generated assets, or private paths

## Contracts and notices

- [ ] Public API/settings/paths remain compatible, or the change is documented
- [ ] `CHANGELOG.md` and user documentation are updated when needed
- [ ] Third-party notices and pins are updated when needed
- [ ] Commits include DCO sign-off
