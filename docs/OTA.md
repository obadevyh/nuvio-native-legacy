# Over-the-air updates for the Nuvio Legacy sharing build

**What users see:** the app checks for a newer release at start-up and about every 6 hours (GitHub allows 60 anonymous calls/hour; this uses a few per day).
A card says a new version is available with *Update now*, *Later* and the change notes (up/down scrolls them; works with any D-pad remote).
*Update now* downloads the APK, checks it, then Android's own installer asks the user to confirm. *Later* hides the card for a day (or until a newer version).
Settings has a manual "check for updates". Offline or failing checks are silent.

## Trust model (what must ALL be true before the system installer is even shown)
1. The release comes from the pinned repository `obadevyh/nuvio-native-legacy` (GitHub API `releases/latest`: stable channel only, drafts and prereleases are invisible).
2. The asset is named `Nuvio-<version>-sharing.apk` and its download URL starts with `https://github.com/obadevyh/nuvio-native-legacy/releases/download/`
   (no `..`, query, fragment, `@`, `%`, backslash). Anything else is ignored.
3. The release JSON carries the asset's SHA-256 digest (computed by GitHub); a release without a digest is ignored, and the digest of a neighbouring asset is never borrowed.
4. The tag is exactly `x.y.z` and its versionCode (`x*10000+y*100+z`) is strictly higher than the installed one.
5. The downloaded file's SHA-256 equals that digest.
6. **On the device, before the installer opens** (`ApkVerifica.kt`): the APK has our application id `space.nuvio.nativelegacy.sharing`, is signed with exactly the same
   certificate set as the installed app (unsigned or different signer = refused), and its versionCode is higher (no downgrade, no re-install of the same version).
   Refusals delete the file and show the reason; nothing is uninstalled or cleared.
7. Android itself repeats the signature/downgrade checks and always asks the user to confirm (no silent install; the app never needs system privileges).

**Why a hijacked manifest/GitHub account cannot install an arbitrary APK:** whoever controls the release can choose the file and its digest, but cannot make a file that is
signed by our private key (kept in `%USERPROFILE%\.nuvio-sharing`, never published). Steps 6 and 7 reject every other APK. The worst a hijacker can do is withhold or delay updates.
Do not give the key or its password to GitHub, CI or anyone else.

## First deployment: one manual update is unavoidable
Builds up to 2.0.7 (including `sharing-2.0.7-r3/r4`) point their updater at the *upstream* project's releases, never at this fork. Everyone must install
**2.0.8 (or later) once** by hand (Downloader or adb). From then on every release is offered in-app. Installing over the old app keeps data (same id, same key).

## Releasing
`tools\release_sharing.ps1 -Version 2.0.9 -Notes docs\releases\2.0.9.md` (add `-DryRun` first). It runs the unit tests, builds with the dedicated key, verifies package/versionCode/pinned certificate/
v2 signature/not-debuggable, uploads to a DRAFT release, compares GitHub's digest and a fresh download with the local SHA-256, and only then publishes. A partial release is never visible to `releases/latest`.
Always increase the version (the versionCode must grow with every release).

**Rollback:** publish a *higher* version containing the previous code (Android never downgrades). To stop an update that is already out, publish the fix as the next version; deleting a release only stops new discoveries.

## Keys
The only signing key is `sharing.jks` (alias `nuvio-sharing`, certificate SHA-256 `5f8dc63e966d9e5f48c74038ea877bf89363dbde93a58507c83740cfdec7e611`). A user-only backup copy is in
a backup folder of your choice; keep the password (`sharing.pass`) in a password manager and a second copy on an external drive. If the key is lost, installed copies cannot be updated
(everyone would have to uninstall and reinstall).

## Migration note (2.0.10)

The update repository is compiled into the app. Installs of 2.0.9 and older look at the previous repository and will not see 2.0.10: install it once
by hand (download `Nuvio-2.0.10-sharing.apk` from this repository's releases). It has the same package name and signing certificate, so it installs
over the old app and keeps every setting; from then on updates arrive in the app from this repository.
