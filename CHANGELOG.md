# Changelog

## 0.3.2 (preview)

- Arm an overlapped engine pipe before DLL startup, with nonblocking read completion checks during window/input tracking.
- Require the DLL to connect and acknowledge startup before returning success; report Win32 startup errors and unexpected remote thread exit codes.
- Update the launcher when the game window is found, and report missing acknowledgements or stalled hook initialization with bounded timeouts.
- Delay MinHook initialization until BGI startup has completed. The supported adapter uses its seven engine hooks directly; generic GDI probes are reserved for diagnostic fallback.
- Provide a small update for existing 0.3.1 installations. Direct Windows confirmation of this startup change is pending.

## 0.3.1

- Provide a small update ZIP for the existing 0.2.2 Windows distribution, with corresponding source as a separate download.
- Build the standard NSIS installer on Linux using Electron Builder's bundled uninstaller reader, without requiring Wine.
- Omit unused development variants of the JavaScript compression library.
- Clarify that both the launcher and game must be closed before replacing the native DLL during updates.

## 0.3.0

- Publish the integration source and reproducible release build scripts.
- Translate the launcher, native messages, installer, and documentation into English.
- Provide a compressed Windows x64 installer and portable 7z archive.
- Keep English and Japanese Chromium locales, strip native debug symbols, and omit development files from the runtime.
- Distribute corresponding source separately, with upstream and release checksums.
- Preserve existing dictionary profiles and the stable extension identity.

Includes earlier adapter v4 and launcher v5 fixes:

- Retain hover targets after text animations finish.
- Preserve furigana timers and Yomitan popups during progressive reveal.
- Track actual surface clears and reject unrelated raster matches.
- Open Steam installations through Steam and connect to existing games.
- Use one launcher instance and close its background processes on exit.
