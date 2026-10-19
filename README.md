# Nuvio Legacy: OpenSeek-integrated sharing build (experimental, unofficial)

> **This is not the official Nuvio or Nuvio Legacy project.** It is an experimental community build derived from
> [iqui27/nuvio-native-legacy](https://github.com/iqui27/nuvio-native-legacy). Please do not ask the original authors for support for this build.

It adds, on top of the upstream app:

* **OpenSeek integration**: seek-preview thumbnails from a self-hosted [OpenSeek](https://github.com/obadevyh/openseek) server, with secure pairing (invitation or one-time code) and certificate pinning.
* **Collaborative library and Contributor Mode** (optional, controlled by the server owner).
* **Simkl** sign-in, and account/server configuration baked in at build time.
* An **in-app updater** that reads this repository's GitHub Releases, verifies the SHA-256, the package name, a higher version and the **same signing certificate** before handing the APK to Android's normal installer.

## Download

Get the latest APK from the [Releases](https://github.com/obadevyh/nuvio-native-legacy/releases) page (`Nuvio-<version>-sharing.apk`). Package name: `space.nuvio.nativelegacy.sharing`
(installs next to the official app without touching it). Installs of 2.0.9 or older must install **2.0.10 or later once by hand**; they check the previous repository for updates.
Older builds are kept on the releases page, marked as pre-releases, for reference only.

## Status

Experimental. Not audited. Provided as is, without warranty. The APKs are signed with a private key that is not in this repository.

## Building

See `docs/OTA.md` for the release process and `INSTALL.md` / `docs/` for the upstream build instructions. The Android build needs a git-ignored
`build/android/nuvio-env.cmake` with your own server and API settings.

## Licence and credits

GNU General Public License v3.0 (`LICENSE`), as upstream. The complete corresponding source of each release is this repository at its tag.
Upstream authors are credited in `docs/UPSTREAM-CONTRIBUTORS.md`; the original README is kept as `docs/UPSTREAM-README.md`.
