# Agent guide

## Purpose and scope

This repository builds **GoGoNippon Study**, a Windows companion for **Go! Go! Nippon! (2011)**. It displays Yomitan's actual dictionary popup with **Shift + hover**, and offline furigana with ordinary hover, over the game's Japanese dialogue. The launcher, messages, installer, and documentation are in English.

Start with [README.md](README.md), [ENGINE-NOTES.md](ENGINE-NOTES.md), and [CHANGELOG.md](CHANGELOG.md). The engine notes describe the reverse engineering evidence, hook sites, coordinates, and remaining limits.

Only BGI 1.528 from the supported 2011 edition is handled. The launcher verifies this original `BGI.exe` SHA-256 before attaching:

```text
2cc889b0e60f5663a0adc6c1b3bc598ac0adb3d6b39b7d5eae6391cadd209c88
```

Do not assume the 2015/2016 editions or other BGI revisions have the same layout. The companion injects hooks into the running game; it does not replace the executable on disk.

## Architecture and files

```text
BGI.exe -> study-hook.dll -> named pipe -> study-helper.exe -> Electron overlay
                                                              |-- Yomitan popup
                                                              `-- Kuromoji readings
```

- `src/main.cjs`: Electron windows, game attachment, extension profile, diagnostics, and shutdown.
- `src/launcher.*`: English launcher interface.
- `src/overlay.mjs`, `src/overlay.css`: glyph hit targets, reveal updates, readings, and popup interaction.
- `src/text-model.mjs`, `src/furigana-worker.cjs`: dialogue/token state and offline Japanese tokenization.
- `src/preload.cjs`, `src/process-control.cjs`: renderer bridge and child-process control.
- `native/helper.cpp`, `native/engine-pipe.hpp`: x86 injection, startup handshake, window/input tracking, and pipe transport.
- `native/hook.cpp`, `native/bgi1528.hpp`, `native/layout-state.hpp`: engine hooks, signatures, ownership, and displayed text lifetime.
- `profiles/bgi-1.528.ini`: supported executable profile.
- `scripts/prepare-vendor.cjs`: reproducible changes to the pinned Yomitan extension. Change this script rather than hand-editing generated `vendor/yomitan` files.
- `scripts/build-windows.cjs`, `scripts/package-release.py`: full Windows packaging, small updates, corresponding source, and checksums.
- `docs/screenshots/image_1.png`: the user-approved README screenshot. Do not bundle documentation screenshots into the app runtime.

Electron is **x64**, but the helper and hook DLL must remain **x86**, matching BGI. Dependencies are pinned: Electron 37.10.3, Yomitan 26.10.12.0, Kuromoji 0.1.2, MinHook 1.3.4, and Electron Builder 26.18.0. Review compatibility and download checksums when changing versions.

## Behavior to preserve

- BGI frees temporary layout nodes when text animation finishes. Keep displayed text and hover rectangles until the actual surface clear or page replacement; layout release alone must not remove them.
- Reconcile existing overlay glyph nodes during progressive reveal. Recreating them on every update causes blinking and loses Yomitan ranges, popups, and reading timers.
- Raster matches must respect surface ownership, allocation, and dimensions. Unrelated glyphs can reuse an old allocation address.
- Arm the overlapped named pipe before invoking `StudyStart`. The DLL must connect and acknowledge startup before reporting success. Keep pipe reads nonblocking so window/input tracking continues.
- Announce the hook worker before MinHook initialization, wait for BGI startup, then install the seven dedicated engine hooks. Generic GDI probes are diagnostic fallback. Do not remove startup/error timeouts or stage reporting.
- Track the selected executable's actual visible window. Steam installations launch through Steam and can display an edition selector.
- Keep the launcher single-instance. On exit, close owned windows, workers, and helper processes; keep the game and Steam running. Never kill unrelated processes by executable name.
- Preserve click-through behavior outside the dictionary popup and the existing extension iframe/preload boundary. Read the security settings together in `ENGINE-NOTES.md` before changing them.

The user confirmed the hover/reveal, Steam launch/shutdown, and **0.3.2 startup fix** on Windows. Version 0.3.1 had the reported attachment failure; do not recommend it for fresh installations. Backlog, menus, save/load transitions, exclusive fullscreen, Wine/Proton, and other engine revisions have not all been validated. Linux builds and replay checks cannot establish Windows gameplay behavior.

## Persistent user data

Keep these identities stable so updates retain imported dictionaries and settings:

- Package name: `gogonippon-yomitan`.
- App ID: `local.gogonippon.yomitan`.
- Electron session partition: `persist:gogonippon-yomitan`.
- Public extension key: `assets/extension-key.txt`; do not regenerate it.
- Windows profile: `%APPDATA%\gogonippon-yomitan` for both installed and portable distributions.

Do not clear user profiles or dictionary storage to work around an upgrade issue. The uninstaller intentionally preserves this data.

## Build and release

Use Node.js 22.23.4+ or 24, npm, Python 3.12+, and x86 MinGW. From the repository root:

```bash
npm ci
npm run vendor
npm run build:native
npm run package:dist
npm run package:release
```

`build:native` finds MinGW in PATH or `.cache/mingw/usr`; `STUDY_CXX` and `STUDY_CC` override compiler paths. The Linux NSIS build uses Electron Builder's bundled `UninstallerReader` instead of executing its temporary Windows stub. Preserve the pinned-version guard in `scripts/build-windows.cjs`; this build does not require Wine.

`dist/` contains the English Setup.exe, Portable.7z, compatible update ZIP, Source.zip, and `SHA256SUMS.txt`. `package:win` builds only the unpacked runtime. `package:release:update` collects an update/source without requiring full installer archives. The compatibility map in `package-release.py` must match the base runtime; the 0.3.2 update requires an existing complete 0.3.1 installation.

For releases, keep package/lock versions consistent, include corresponding source and license notices, inspect archive contents, and verify uploaded sizes/checksums. Source packaging uses an explicit file/directory list: update it when adding project documentation or source. Keep published tags intact. Publish full downloads for fresh installations; small updates alone need their specified base. Both the launcher and game must be closed before replacing the loaded DLL.

Keep downloads compact: retain only English/Japanese Chromium locales, stripped native binaries, compressed full archives, and a separate source download. Electron is the largest runtime component. Do not add build tools, source archives, screenshots, or caches to the Windows runtime.

## Checks, diagnostics, and repository contents

Do not add or run implementation tests unless the user requests testing or verification. Existing checks and their environment requirements are documented in the README. Report cross-compilation, archive inspection, replay results, and actual Windows confirmation separately and accurately.

When diagnosing a user-supplied recording, use `scripts/analyze-diagnostics.py` and the engine notes. Diagnostics can include local paths, game dialogue, and runtime code; keep them private and out of commits/releases. Do not claim a precise cause that the recording does not establish.

Keep original game executables, asset archives, saves, imported dictionaries, private diagnostics, secrets, `node_modules/`, `vendor/`, `.cache/`, `native/bin/`, `native/obj/`, and `dist/` out of Git. Generated release assets belong on GitHub Releases. The explicitly approved README screenshot is public documentation; only `image_1.png` was requested.
