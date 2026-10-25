# Changelog

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
