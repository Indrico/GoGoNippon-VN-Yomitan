# BGI 1.528 adapter

## Supported executable

- Game: Go! Go! Nippon! (2011), Steam app 251870.
- Engine: Ethornell / BURIKO General Interpreter 1.528, compatibility 1.69.
- Format: PE32 x86; preferred image base `0x00400000`.
- SHA-256: `2cc889b0e60f5663a0adc6c1b3bc598ac0adb3d6b39b7d5eae6391cadd209c88`.

The launcher verifies the executable. The native adapter waits for runtime engine code and checks all seven instruction signatures before installing the hooks. Addresses below are relative to the module base. Dynamically allocated objects are never hardcoded.

## Data flow

```text
BGI.exe -> study-hook.dll -> named pipe -> study-helper.exe -> Electron
                                                            |-- transparent glyph DOM -> Yomitan
                                                            `-- Kuromoji worker -> hover readings
```

Original game files remain unchanged. The x86 helper locates the selected executable by canonical Windows path, favors the matching visible window, and loads the x86 DLL. Steam installations use `steam://rungameid/251870`; other installations launch with their executable's directory as the working directory.

## Hook sites

| Role | RVA | State |
| --- | --- | --- |
| Text parser | `0x29a50` | CP932 input at entry ESP + `0x0c` |
| Glyph layout | `0x2b211` | Character at ESP + `0x2c`; node in EBP |
| Layout return | `0x2b2c0` | Root through ESP + `0xd20` |
| Blit after clipping | `0x202e6` | Surface in EBX; descriptor in EBP; rectangle through ESP + `0x54` |
| Text reset | `0x295e0` | Text owner and destination surface |
| Layout release | `0x2b380` | Temporary layout destruction |
| Surface clear | `0x20610` | Actual displayed bitmap erasure |

Signatures and structure constants are in `native/bgi1528.hpp`; hooks are in `native/hook.cpp`. The x86 bridge preserves registers, flags, x87 and SSE state, and callback stack alignment.

## Text lifetime and coordinates

Layout nodes are `0x48` bytes: placement at `+0x10/+0x14`, bitmap descriptor at `+0x20`, next pointer at `+0x44`. A text object owns its surface through `+0x20` and layout root through `+0x7c`.

Raster matching requires the current surface owner, bitmap allocation, and dimensions. Allocation addresses alone are insufficient because unrelated glyphs can reuse older Japanese raster allocations.

BGI frees temporary nodes when its reveal animation ends while rendered text remains visible. `LayoutStore.releaseRoot` revokes raster ownership and retains displayed text, hit rectangles, and page identity. `eraseSurface` removes retained text when the game actually clears its bitmap. Active layouts can redraw after a clear, including one before their first glyph. A later reset replaces the old page.

Surface origin is read at `+0x24/+0x28`. Visibility requires `surface[+4] != 0`, `surface[+8] != 0`, and `surface[+0xa0] < 256`. Typical main dialogue uses an 800×600 canvas, origin `(21,435)`, 703×56 text bitmap, and 21×21 glyph rasters with 19-pixel advances. Hit boxes stop at the next advance to avoid overlapping targets.

The DLL emits complete `dialogue` snapshots about every 30 ms with Unicode text, UTF-16 indices, and rectangles for drawn glyphs. Bounded diagnostics do not stop live capture.

## Overlay and Yomitan

The helper reports the physical client rectangle and Shift/Escape state every 25 ms. Electron converts physical coordinates to DIP and maps the canvas with `contain` scaling. The overlay ignores mouse input outside an open dictionary popup, allowing the game to retain focus.

Reveal updates reconcile existing glyph DOM nodes, preserving Yomitan source ranges, popups, tokens, and reading timers. Real page changes clear stale targets. Kuromoji tokenization is cached by complete sentence.

`scripts/prepare-vendor.cjs` patches the pinned Yomitan extension. Native Shift events invoke the real scanner. Ordinary automatic scanning, text selection, separate popup windows, secure blob frame URLs, and shadow DOM are disabled only for this overlay instance. Popup visibility events control click-through behavior.

The extension needs Electron subframe initialization. The overlay uses `nodeIntegration: false`, `nodeIntegrationInSubFrames: true`, `contextIsolation: true`, `sandbox: false`, and `webSecurity: false` for its extension iframe. Its preload exposes the bridge only in the main frame. Top-level navigation is blocked; external HTTP(S) links open in the browser. Review these settings together when changing Electron or the hosted extension.

The extension public key, app name, app ID, and partition `persist:gogonippon-yomitan` remain stable across releases. Service workers are invalidated on extension revisions without clearing dictionary IndexedDB.

## Process lifecycle

One instance owns the launcher lock; a second launch restores the existing window. Closing the launcher destroys its overlay/settings windows, stops the reading worker, flushes profile storage, and sends `quit` to the helper. An unresponsive helper is stopped by its own PID after a grace period. BGI and Steam keep running.

The DLL disables hooks on detach and keeps its hook storage resident, avoiding deallocation while game threads may still return through a trampoline. The game can be reattached without replacing its executable.

## Evidence and limits

Windows recordings established ownership, foreign raster reuse, geometry, and the shorter lifetime of layout nodes compared with displayed text. The user confirmed the hover and Steam startup/shutdown fixes work. Private recordings, runtime dumps, screenshots, and game dialogue are excluded from the repository and release assets.

Existing replay checks exercise the actual ownership implementation against a user-supplied recording. Existing Electron checks cover dictionary import, popup rendering, persistence, furigana, reveal stability, and shutdown. Linux checks cannot establish untested Windows menu, save/load, backlog, fullscreen, or other revision behavior.

## References

- [REA](https://github.com/morluto/rea)
- [Yomitan](https://github.com/yomidevs/yomitan)
- [Electron extension support](https://www.electronjs.org/docs/latest/api/extensions)
- [MinHook](https://github.com/TsudaKageyu/minhook)
