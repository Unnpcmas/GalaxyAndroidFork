# Galaxy Android

**Unofficial fork, maintained by Unncpmas.**
[Leer en español](README.md)

This repository is Unncpmas's fork of [Galaxy Android by OpenPort Labs](https://github.com/OpenPort-Labs/Galaxy-Android). Unncpmas maintains this fork; the original code, decompilation work, and upstream contributions remain credited to their respective authors. See [Credits](CREDITS.en.md).

## Latest download

- **Android ARM64 APK, version 0.0.38 (version code 43):** [Download GalaxyAndroid-v0.0.38-arm64.apk](https://github.com/Unnpcmas/GalaxyAndroidFork/releases/download/0.0.38/GalaxyAndroid-v0.0.38-arm64.apk)
- **GitHub Release:** [Galaxy Android 0.0.38](https://github.com/Unnpcmas/GalaxyAndroidFork/releases/tag/0.0.38)
- **Full source code (.zip):** [Download Galaxy-Android-Source-0.0.38.zip](https://github.com/Unnpcmas/GalaxyAndroidFork/releases/download/0.0.38/Galaxy-Android-Source-0.0.38.zip)
- **Browse project files:** [source tree](https://github.com/Unnpcmas/GalaxyAndroidFork/tree/main)
- **Build date, notes, and SHA-256 checks:** [LATEST_UPDATE.md](LATEST_UPDATE.md)

Only the latest APK is included. This build targets `arm64-v8a`; it does not include 32-bit support.

## About the project

Galaxy Android is a native Android port built from C++ code, not an emulator. This fork contains ongoing work on:

- Native ARM64 execution and OpenGL ES/Vulkan graphics options.
- Configurable touch controls and gamepad support.
- Local display and rendering settings.
- On-device diagnostics for startup, graphics, audio, and errors.
- Progress, save, and load features that are still being developed.

Stability and compatibility vary by device and build. This is a snapshot of capabilities present in the project, not a promise that every feature is complete or works identically on every phone.

## Repository layout

- `decomp/`: reconstructed source files from the original project. The many small files are part of the build; they are not game copies.
- `platform/` and `installer-mobile/`: the Android port, interface, and native installer tools.
- `tools/`: resource-preparation and packaging utilities.
- `docs/`: technical and user guides.

The build and install helpers referenced by the documentation are retained. One obsolete script that pushed to the upstream repository has been removed. The main guides, credits, and release notes are available in Spanish and English. Upstream code and inherited technical documentation remain in their original language to preserve meaning and attribution.

## Game files and rights

- Neither the APK nor the source tree contains a copy of the game, a ROM, save files, or extracted game data.
- You are responsible for supplying a lawfully obtained game copy and following the laws that apply where you live.
- Do not upload game dumps, disc images, game assets, save files, signing keys, or other private material here.
- This is an unofficial project and is not affiliated with, sponsored by, or endorsed by Nintendo. All game names, trademarks, and rights belong to their respective owners.
- Whether this repository is public or private does not replace license obligations or grant rights to third-party material.

## Building from source

See the [installation guide](docs/INSTALLATION.md) and [technical documentation](docs/TECHNICAL.md). Android builds require the Android SDK/NDK, JDK, and the tools listed in those guides. Keep your signing key locally; it is not part of this repository or the source tree. Spanish versions are listed in the [documentation index](docs/README.md).

## Licenses and credits

The main project code retains the upstream project's GNU GPL v3.0 license. Some components carry their own licenses. Review [`LICENSE`](LICENSE), [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md), and [Credits](CREDITS.en.md) before reusing or redistributing any part.

## Support this fork

[Unncpmas on Patreon](https://www.patreon.com/c/unnpcmas/membership)
