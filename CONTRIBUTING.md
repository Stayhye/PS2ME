# Contributing to PS2ME

Thanks for your interest in PS2ME! This document covers the branch model, the release
flow, and a few conventions.

## Branch model

- **`main`** — the release branch. It always reflects the latest published release and is
  protected; changes land only through pull requests. Every release is tagged `vX.Y.Z` here.
- **`develop`** — the integration branch for ongoing work. Branch your feature/fix off
  `develop`, and open a pull request back into it.

```
feature/xyz ──▶ develop ──(release)──▶ main ──▶ tag vX.Y.Z
```

## Making changes

1. Branch from `develop` (e.g. `feature/short-name` or `fix/short-name`).
2. Keep commits focused and by responsibility; write clear, imperative subject lines
   in English.
3. Match the surrounding code style. Comments and user-visible strings are **en-US**.
4. Open a PR into `develop` describing what changed and how you tested it.

## Building & testing

The build runs in Docker; see [README.md](README.md#building-from-source). The phoneME
source tree and the PS2 SDK are external and are not vendored here, so builds are produced
locally rather than in CI. Test changes under PCSX2 and, where possible, on real hardware.

## Releasing

Releases are cut from `main`:

1. `./version.sh {major|minor|patch}` — bump `ps2/version.h`.
2. Update [`CHANGELOG.md`](CHANGELOG.md) with the new version section.
3. Commit `Release vX.Y.Z`, then build a stripped release ELF
   (`docker/phoneme-cross/build-release-ps2.sh`).
4. Tag `vX.Y.Z` and run `tools/publish-release.sh`, which pushes the tag and uploads the
   ELF (and `bgm.adpcm`) to a GitHub Release. Pushing the tag also triggers the
   `release.yml` workflow, which creates the release and its notes from the changelog.

Pushing to `main` publishes the project website (`docs/`) via the `pages.yml` workflow.

## License

By contributing, you agree that your contributions are licensed under the project's
[GPL-2.0](LICENSE) license.
