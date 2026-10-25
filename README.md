# GoGoNippon VN Yomitan

Learn Japanese while playing **Go! Go! Nippon! (2011)**. This Windows companion places Yomitan's real dictionary popup and offline furigana over the game's original Japanese dialogue.

- **Hover** over a kanji word to see its reading.
- **Shift + hover** to look it up in Yomitan.
- **Escape** to dismiss the popup.
- Hover targets remain available after the text animation finishes.
- Launch through Steam or connect to an existing game. Closing the launcher closes its background processes; the game keeps running.

The launcher, engine messages, installer, and documentation are in English. Japanese learning text stays in Japanese.

## Screenshot

![Yomitan dictionary popup over Japanese dialogue in Go! Go! Nippon!](https://raw.githubusercontent.com/Indrico/GoGoNippon-VN-Yomitan/main/docs/screenshots/image_1.png)

Hold Shift while hovering over Japanese dialogue to open Yomitan's dictionary popup.

## Download

Get the latest version from [Releases](https://github.com/Indrico/GoGoNippon-VN-Yomitan/releases/latest).

| Asset | Use |
| --- | --- |
| `GoGoNippon-Study-<version>-Windows-x64-Setup.exe` | Recommended: English installer with a Start menu shortcut. No separate Electron or Node.js installation needed. |
| `GoGoNippon-Study-<version>-Windows-x64-Portable.7z` | Extract the entire archive, then run `GoGoNippon-Study.exe`. Use 7-Zip if your Windows archive tool cannot open it. |
| `GoGoNippon-Study-0.3.2-Update-from-0.3.1.zip` | Small update for an existing complete 0.3.1 installation. See below. |
| `GoGoNippon-Study-<version>-Source.zip` | Developer source and build scripts. Not needed to use the app. |
| `SHA256SUMS.txt` | Download checksums. |

### Small update from 0.3.1

Version [0.3.2](https://github.com/Indrico/GoGoNippon-VN-Yomitan/releases/tag/v0.3.2) fixes the startup case where the game is playable but the launcher keeps waiting for its window and hover never starts. The user has confirmed that the fix works on Windows.

For an existing complete 0.3.1 installation, close **both the launcher and BGI.exe**, then extract `GoGoNippon-Study-0.3.2-Update-from-0.3.1.zip` into your existing launcher folder and merge/replace `resources`. Reopen the launcher and check its footer says **0.3.2**. Your imported dictionaries stay in the existing profile. For a fresh installation, use the full installer or portable archive.

The runtime includes English and Japanese Chromium locales, stripped native binaries, the offline reading dictionary, and Yomitan. Solid LZMA compression reduces download size. The installed app still includes Electron, its largest component. Game files, diagnostic recordings, build tools, and source archives are excluded from the runtime.

## Requirements

- **Windows 10/11 x64**, with Japanese fonts installed.
- Your own copy of **Go! Go! Nippon! (2011)**, using the supported BGI executable. The 2015/2016 editions are not supported by this adapter.
- Windowed or borderless mode. Exclusive fullscreen and Wine/Proton have not been validated.
- Your own Yomitan dictionary ZIPs. The app uses a separate profile from your browser.

Supported `BGI.exe` SHA-256:

```text
2cc889b0e60f5663a0adc6c1b3bc598ac0adb3d6b39b7d5eae6391cadd209c88
```

The launcher checks this hash before attaching. Original game files are not modified or distributed.

## First run

1. Install the app, or extract the portable archive completely. Start `GoGoNippon-Study.exe`.
2. Click **Choose BGI.exe** and select the original executable. A typical Steam location is:

   ```text
   D:\SteamLibrary\steamapps\common\GoGoNippon\GoGoNippon\BGI.exe
   ```

3. Open **Yomitan Settings** and import your dictionary ZIPs under **Dictionaries**. You can also import a Yomitan settings export. Browser dictionaries must be imported separately.
4. Use **Try Demo** to try readings and lookup without starting the game. Close the demo with Escape.
5. Click **Launch Game**. For a Steam installation, choose the **2011 edition** if Steam presents a version selector. An already running matching game is connected directly.
6. Use windowed/borderless mode and advance to Japanese dialogue. Allow about six seconds for the engine adapter to initialize. Hover for furigana; hold Shift while hovering for Yomitan.

Furigana is generated offline by Kuromoji. Readings for names and ambiguous words can be wrong; use the dictionary to confirm them. Definitions require at least one imported Yomitan dictionary.

## Updating and stored data

Close both the old launcher and the game before updating, so Windows can release the native DLL. Install the new version, or extract the portable release into a new folder. The application identity and extension key remain stable, so both distributions use your existing data at:

```text
%APPDATA%\gogonippon-yomitan
```

This directory contains your game path, Yomitan settings, imported dictionaries, and local diagnostics. The portable app uses this same profile; settings do not live beside its executable. Uninstalling does not delete this data.

## Troubleshooting

| Symptom | Action |
| --- | --- |
| Steam displays a version selector | Choose the 2011 edition and wait for the window. The launcher waits up to 90 seconds. |
| Unsupported executable | Select the supported original `BGI.exe`; inspect its hash with `Get-FileHash -Algorithm SHA256`. |
| Access denied | Run the game and launcher with matching permissions. |
| No dictionary popup | Import a dictionary in Yomitan Settings, try the demo, then Shift + hover a Japanese word. |
| No hover over the game | Check the game version, use windowed mode, wait for the active adapter status, and advance one page. |
| Need to reconnect | Click **Disconnect**, then **Launch Game**. |

**Capture Diagnostics** records engine information. Advance a few Japanese dialogue pages, then select **Export Diagnostics** to save a ZIP. Recordings can contain game text, local paths, and runtime code; share only the recording you intend to provide. Diagnostics are stored locally and are not uploaded automatically.

The main dialogue path and launcher fixes have been confirmed by the user on Windows. Backlog, menus, save/load transitions, exclusive fullscreen, and other game revisions may require further adapter work.

## Build from source

The build below targets Windows from Linux without Wine. Requirements: Node.js 22.23.4+ or 24, npm, Python 3.12+, and x86 MinGW. On Ubuntu, install `g++-mingw-w64-i686-posix` and `gcc-mingw-w64-i686-posix`.

```bash
npm ci
npm run vendor
npm run build:native
npm run package:dist
npm run package:release
```

For a small update build using an already installed compatible runtime:

```bash
npm run package:win
npm run package:release:update
```

Output is written to `dist/`. `package:win` builds just the unpacked runtime. `package:release` collects corresponding source and generates checksums for the current version. The manually triggered [release build workflow](.github/workflows/build.yml) performs the same build and uploads artifacts without publishing a release.

On Linux, `scripts/build-windows.cjs` uses Electron Builder's bundled NSIS uninstaller reader instead of executing its temporary Windows stub through Wine. This small integration is guarded against the pinned Electron Builder version. The app uses the standard NSIS installer and uninstaller templates.

Dependency versions and archive checksums are pinned. `npm run vendor` verifies Yomitan **26.10.12.0** and MinHook **1.3.4**, then applies the Electron changes. Keep `assets/extension-key.txt`: it is a public key providing a stable extension identity for existing dictionaries.

`build-native.py` uses x86 MinGW from PATH or `.cache/mingw/usr`. Override compiler paths with `STUDY_CXX` and `STUDY_CC`. The helper and DLL are always **x86**, matching BGI, while the launcher is x64.

The source companion includes upstream Yomitan source under `upstream/` and the changed extension files under `modified-yomitan/`. Normal builds regenerate the patched extension from the verified Chrome release. To build upstream Yomitan itself, extract its source archive and follow its development instructions (`npm run build -- --target chrome`).

### Existing developer checks

```bash
npm run check:core
xvfb-run -a -s '-screen 0 1280x1000x24' npm run check:electron -- --no-sandbox --disable-gpu
xvfb-run -a -s '-screen 0 1280x1000x24' npm run check:lifecycle -- --no-sandbox --disable-gpu
# Optional replay of a diagnostic you supply yourself; requires Linux g++:
python3 scripts/check-diagnostic-replay.py /path/to/GoGoNippon-diagnostics.zip
```

`--no-sandbox` is for containers that cannot provide Chromium's sandbox. Normal Windows launches do not use it. Electron checks use temporary profiles and a synthetic dictionary. Native game behavior requires Windows; replay checks do not replace a game run.

See [ENGINE-NOTES.md](ENGINE-NOTES.md) and [CHANGELOG.md](CHANGELOG.md).

## License

GPL-3.0-or-later. Dependency licenses and sources are listed in [THIRD_PARTY.md](THIRD_PARTY.md). This is an unofficial companion project, unaffiliated with the game's developers, publishers, or Yomitan maintainers.
