#pragma once
// Addresses and signatures checked against the four 2026-10-25 Windows code
// snapshots, not the encrypted .text in the on-disk executable.
namespace bgi1528 {
constexpr unsigned parser = 0x29a50;
constexpr const char* parserSignature = "6aff68a6f8490064a100000000506489";
constexpr unsigned glyphLayout = 0x2b211;
constexpr const char* glyphLayoutSignature = "8b5424105250894d1ce8110100008b94";
constexpr unsigned layoutReturn = 0x2b2c0;
constexpr const char* layoutReturnSignature = "8b8c24100d00005b64890d0000000081";
constexpr unsigned blitReady = 0x202e6;
constexpr const char* blitReadySignature = "8bcb555756e8301600005f5e5d33c05b";
constexpr unsigned textReset = 0x295e0;
constexpr const char* textResetSignature = "568bf18d467c50e8941d00008b4c2418";
constexpr unsigned layoutClear = 0x2b380;
constexpr const char* layoutClearSignature = "56578b7c240c8b774485f6742d538bde";
constexpr unsigned surfaceClear = 0x20610;
constexpr const char* surfaceClearSignature = "568bf16a008d866c01000050e88f7cfe";
constexpr unsigned surfaceVtable = 0xa30d0;
constexpr unsigned objectSurface = 0x20, objectRoot = 0x7c;
constexpr unsigned surfaceOriginX = 0x24, surfaceOriginY = 0x28;
// Current ESP is S-0xd28 in the parser body and S-0xd1c at layoutReturn.
constexpr unsigned bodyRoot = 0xd2c;
constexpr unsigned returnRoot = 0xd20;
constexpr unsigned character = 0x2c;
constexpr unsigned linkedListHead = 0x44;
constexpr unsigned image = 0x20;
constexpr unsigned nodeX = 0x10, nodeY = 0x14;
constexpr unsigned imageWidth = 0x28, imageHeight = 0x2c;
}
