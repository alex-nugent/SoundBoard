# Releasing firmware

How a board in the field gets a new version, and the steps to publish one.

## How it fits together

- **Private repo** `alex-nugent/SoundBoardV4` (this one). `firmware/` is the source of truth.
- **Public repo** `alex-nugent/SoundBoard` (`git remote public`). Only the `firmware/` tree, pushed with `git subtree`. Boards fetch releases from here. Not open source (`LICENSE`).
- **Workflow** `.github/workflows/release.yml` in the public repo: a tag `vX.Y.Z` runs the native tests, builds `pros3`, and attaches `soundboard-vX.Y.Z.bin`, `.elf` and `manifest.json` to a GitHub release. The release body is `RELEASE_NOTES.md`.
- **The board** (`src/net/ota.*`) reads `https://github.com/<update.repo>/releases/latest/download/manifest.json`, shows `version` and whether it is the one already running (`FW_VERSION`, from `git describe --tags` at build time, `tools/version.py`), downloads the `.bin`, checks the SHA-256, installs, reboots, and rolls back if the new image fails its health check.
- **Version** is the tag. Nothing in the source needs editing.

## Publish a release

1. Everything under `firmware/` committed. `pio test -e native` passes. The dev build has run on the bench.
2. Write `RELEASE_NOTES.md`: what changed, for Soren. Replace the file, do not append. Plain sentences, no headings needed.
3. Commit.
4. Alex runs, from the repo root:
   ```sh
   firmware/tools/publish.sh v0.12.0
   ```
   It refuses if `firmware/` has uncommitted changes. Without a version it only pushes the tree.
5. Watch https://github.com/alex-nugent/SoundBoard/actions. About three minutes.
6. Check the release:
   ```sh
   curl -sL https://github.com/alex-nugent/SoundBoard/releases/latest/download/manifest.json
   ```
   `version` must be the new tag; `file` and `sha256` present.
7. Check from a board: settings page, Firmware, Check for updates. Or on the console with Wi-Fi joined: `fw check`, then `fw install`. USB power required.

Claude cannot push to the public repo (the tool blocks it, by design). Claude prepares the notes and the commit; Alex runs step 4.

## Versions

`vMAJOR.MINOR.PATCH`. Bump PATCH for fixes, MINOR for features, MAJOR at v1.0.0 when Soren's board ships and for a config schema change after that. Tags are never reused except as below.

## Fixing a broken release

If the workflow failed or the image is wrong and no board has installed it yet: fix, commit, delete the release and tag on GitHub (release page, Delete; then the tag), run `publish.sh` with the same version. The script force-moves the tag.

If a board may already have it: publish the next PATCH version instead.

## After an OTA on the bench unit

An OTA install writes the other app slot and marks it current. To reflash the dev build over USB, erase the OTA record first or the bootloader keeps booting the release:

```sh
esptool.py --chip esp32s3 --port /dev/cu.usbmodem1431401 erase_region 0xe000 0x2000
```

Then `pio run -t upload` as usual.

## Never in the public tree

- Annalise's name, or any family name, phone number or address. `examples/config.example.json` uses placeholders.
- Wi-Fi names or passwords. The bench Wi-Fi lives on the bench card only.
- `RELEASE_NOTES.md` is public: write it for a stranger.

## Where things are

| What | Where |
|---|---|
| Publish script | `tools/publish.sh` |
| Workflow | `.github/workflows/release.yml` |
| Manifest writer | `tools/manifest.py` |
| Version stamp | `tools/version.py` → `FW_VERSION` |
| Updater | `src/net/ota.cpp`, `src/app/update.cpp` |
| Repo the boards read | setting `update.repo`, default `alex-nugent/SoundBoard` |
| Design | `spec/v4/FirmwareSpec.md` §16, §19.11 |
