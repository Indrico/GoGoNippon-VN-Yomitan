#include "common.hpp"
#include "MinHook.h"
#include "bgi1528.hpp"
#include "layout-state.hpp"
#include <atomic>
#include <deque>
#include <mutex>
#include <sstream>
#include <array>
#include <map>
#include <set>
#include <cstring>

static uintptr_t imageBase;
static DWORD imageSize;
static std::atomic<bool> running{false};
static std::atomic<unsigned> sampleCount{0};
static std::mutex queueMutex;
static std::deque<std::string> queue;
static std::vector<void*> stubs;
static std::map<void*, bool> installedHooks;
static StudyInit configuration;
static decltype(&TextOutA) originalTextOutA;
static decltype(&TextOutW) originalTextOutW;
static unsigned canvasWidth = 0, canvasHeight = 0;
static std::atomic<bool> verifiedProfile{false};
static std::string glyphCodec, canvasFit;
static std::atomic<unsigned> layoutSamples{0}, blitSamples{0}, clearSamples{0};
static std::atomic<bool> liveAdapter{false};

static bool emit(std::string message) {
  // Renderer/game threads never wait for disk or IPC, and skip a sample on
  // contention instead of adding latency to the game's font path.
  std::unique_lock<std::mutex> lock(queueMutex, std::try_to_lock);
  if (!lock || queue.size() >= 2048) return false;
  queue.push_back(std::move(message) + '\n');
  return true;
}
static void status(const std::string& message, const std::string& detail = "") {
  emit("{\"type\":\"status\",\"message\":" + quote(message) + ",\"detail\":" + quote(detail) + "}");
}
static bool startupStatus(HANDLE pipe, const std::string& message, const std::string& detail = "") {
  const auto line = "{\"type\":\"status\",\"message\":" + quote(message) + ",\"detail\":" + quote(detail) + "}\n";
  DWORD written = 0;
  if (!WriteFile(pipe, line.data(), line.size(), &written, nullptr)) return false;
  if (written != line.size()) { SetLastError(ERROR_WRITE_FAULT); return false; }
  return true;
}
static bool japanese(const std::wstring& text) {
  return std::any_of(text.begin(), text.end(), [](wchar_t c) { return (c >= 0x3040 && c <= 0x30ff) || (c >= 0x3400 && c <= 0x9fff); });
}
static std::wstring pointerText(uintptr_t address, size_t maximum = 256) {
  if (address < 0x10000 || address > 0x7fffffff) return {};
  std::string bytes;
  for (size_t i = 0; i < maximum;) {
    char buffer[32];
    const size_t count = std::min({sizeof(buffer), maximum - i, size_t(4096 - ((address + i) & 4095))});
    if (!readMemory(reinterpret_cast<void*>(address + i), buffer, count)) return {};
    for (size_t j = 0; j < count; ++j) {
      if (!buffer[j]) return wide(bytes, 932);
      bytes += buffer[j];
    }
    i += count;
  }
  return {};
}
static std::string callstack() {
  void* frames[20]; USHORT n = CaptureStackBackTrace(1, 20, frames, nullptr);
  std::string out = "["; bool first = true;
  for (USHORT i = 0; i < n; ++i) {
    uintptr_t address = reinterpret_cast<uintptr_t>(frames[i]);
    if (address < imageBase || address >= imageBase + imageSize) continue;
    if (!first) out += ',';
    first = false;
    const uintptr_t start = address >= imageBase + 24 ? address - 24 : imageBase;
    out += "{\"rva\":" + std::to_string(address - imageBase) + ",\"before\":" + quote(hexBytes(reinterpret_cast<void*>(start), address - start)) +
      ",\"after\":" + quote(hexBytes(reinterpret_cast<void*>(address), 32)) + "}";
  }
  return out + ']';
}
static void recordGdi(const char* api, HDC dc, int x, int y, const std::wstring& text) {
  if (!japanese(text) || sampleCount.fetch_add(1) >= 3000) return;
  BITMAP bitmap{}; GetObjectW(GetCurrentObject(dc, OBJ_BITMAP), sizeof(bitmap), &bitmap);
  HWND window = WindowFromDC(dc);
  emit("{\"type\":\"gdi-sample\",\"api\":" + quote(api) + ",\"text\":" + quote(utf8(text)) +
    ",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
    ",\"bitmapWidth\":" + std::to_string(bitmap.bmWidth) + ",\"bitmapHeight\":" + std::to_string(bitmap.bmHeight) +
    ",\"windowDC\":" + (window ? "true" : "false") + ",\"coordinateStatus\":\"unverified\",\"stack\":" + callstack() + "}");
}
static BOOL WINAPI hookTextOutA(HDC dc, int x, int y, LPCSTR text, int count) {
  thread_local bool busy = false;
  if (!busy && text && count > 0 && count <= 1024) {
    busy = true;
    std::string copy(count, '\0');
    if (readMemory(text, copy.data(), count)) recordGdi("TextOutA", dc, x, y, wide(copy, 932));
    busy = false;
  }
  return originalTextOutA(dc, x, y, text, count);
}
static BOOL WINAPI hookTextOutW(HDC dc, int x, int y, LPCWSTR text, int count) {
  thread_local bool busy = false;
  if (!busy && text && count > 0 && count <= 1024) {
    busy = true;
    std::wstring copy(count, L'\0');
    if (readMemory(text, copy.data(), count * sizeof(wchar_t))) recordGdi("TextOutW", dc, x, y, copy);
    busy = false;
  }
  return originalTextOutW(dc, x, y, text, count);
}
struct Registers {
  uint32_t edi, esi, ebp, savedEsp, ebx, edx, ecx, eax, flags;
};
static uintptr_t originalStack(const Registers* r) { return reinterpret_cast<uintptr_t>(r) + sizeof(Registers); }

// Layout and blit ownership were correlated in the second Windows diagnostic.
// Diagnostic samples remain bounded; the live adapter has no sample limit.
struct LayoutGlyph {
  uintptr_t node = 0, pixels = 0, root = 0;
  uint32_t code = 0;
  int32_t x = 0, y = 0, width = 0, height = 0;
  RECT lastRect{};
  bool drawn = false;
};
struct LayoutCapture {
  uintptr_t root = 0, surface = 0;
  std::wstring text;
  std::vector<LayoutGlyph> glyphs;
  bool active = false;
};
static thread_local LayoutCapture capture;
static thread_local uintptr_t resetRoot = 0, resetSurface = 0;
static std::map<uintptr_t, LayoutGlyph> rasterGlyphs;
static std::mutex rasterMutex;
static study::LayoutStore liveLayouts;
static bool stackValue(const Registers* r, unsigned offset, uint32_t& value) {
  return readMemory(reinterpret_cast<void*>(originalStack(r) + offset), &value, sizeof(value));
}
static std::wstring characterText(uint32_t code) {
  std::string bytes;
  if (code > 0xffff || code < 0x20) return {};
  if (code > 0xff) bytes += static_cast<char>(code >> 8);
  bytes += static_cast<char>(code & 255);
  return wide(bytes, 932);
}
static void __cdecl recordParserBegin(Registers* r) {
  capture = LayoutCapture{};
  uint32_t root = 0, text = 0;
  if (!stackValue(r, 4, root) || !stackValue(r, 12, text)) return;
  capture.text = pointerText(text, 8192);
  if (!japanese(capture.text)) return;
  capture.root = root;
  if (root == resetRoot) capture.surface = resetSurface;
  capture.active = true;
  if (layoutSamples.load() >= 300) return;
  std::string args = "[";
  for (unsigned i = 1; i <= 13; ++i) {
    uint32_t value = 0; stackValue(r, i * 4, value);
    if (i > 1) args += ',';
    args += "{\"offset\":" + std::to_string(i * 4) + ",\"value\":" + std::to_string(value);
    if (i != 3 && value >= 0x10000) args += ",\"bytes\":" + quote(hexBytes(reinterpret_cast<void*>(value), 80));
    args += '}';
  }
  emit("{\"type\":\"text-layout-begin\",\"probeVersion\":2,\"rva\":" + std::to_string(bgi1528::parser) +
    ",\"root\":" + std::to_string(root) + ",\"text\":" + quote(utf8(capture.text)) + ",\"arguments\":" + args + "]}");
}
static void __cdecl recordLayoutGlyph(Registers* r) {
  if (!capture.active || capture.glyphs.size() >= 2048) return;
  uint32_t code = 0, root = 0;
  if (!stackValue(r, bgi1528::character, code) || !stackValue(r, bgi1528::bodyRoot, root) || root != capture.root) return;
  uint32_t node[18];
  if (!readMemory(reinterpret_cast<void*>(r->ebp), node, sizeof(node))) return;
  LayoutGlyph glyph;
  glyph.node = r->ebp; glyph.root = root; glyph.code = code;
  glyph.x = static_cast<int32_t>(node[bgi1528::nodeX / 4]);
  glyph.y = static_cast<int32_t>(node[bgi1528::nodeY / 4]);
  glyph.pixels = node[bgi1528::image / 4];
  glyph.width = static_cast<int32_t>(node[bgi1528::imageWidth / 4]);
  glyph.height = static_cast<int32_t>(node[bgi1528::imageHeight / 4]);
  capture.glyphs.push_back(glyph);
}
static void __cdecl recordLayoutReturn(Registers* r) {
  if (!capture.active) return;
  capture.active = false;
  uint32_t root = 0, layout[18];
  if (!stackValue(r, bgi1528::returnRoot, root) || root != capture.root ||
      !readMemory(reinterpret_cast<void*>(root), layout, sizeof(layout))) return;
  std::map<uintptr_t, LayoutGlyph> codes;
  for (const auto& glyph : capture.glyphs) codes[glyph.node] = glyph;
  uintptr_t address = layout[bgi1528::linkedListHead / 4];
  std::set<uintptr_t> visited;
  study::Layout live;
  live.root = root; live.surface = capture.surface;
  std::string nodes = "[";
  unsigned count = 0;
  while (address && count < 2048 && visited.insert(address).second) {
    uint32_t node[18];
    if (!readMemory(reinterpret_cast<void*>(address), node, sizeof(node))) break;
    if (count++) nodes += ',';
    nodes += "{\"address\":" + std::to_string(address) + ",\"bytes\":" + quote(hexBytes(node, sizeof(node)));
    auto found = codes.find(address);
    if (found != codes.end()) {
      auto glyph = found->second;
      glyph.x = static_cast<int32_t>(node[4]); glyph.y = static_cast<int32_t>(node[5]);
      glyph.pixels = node[8]; glyph.width = static_cast<int32_t>(node[10]); glyph.height = static_cast<int32_t>(node[11]);
      const auto decoded = characterText(glyph.code);
      if (!decoded.empty()) {
        study::Glyph entry;
        entry.node = glyph.node; entry.pixels = glyph.pixels;
        entry.bitmapWidth = glyph.width; entry.bitmapHeight = glyph.height;
        entry.text = utf8(decoded); entry.units = decoded.size();
        live.glyphs.push_back(std::move(entry));
      }
      nodes += ",\"code\":" + std::to_string(glyph.code) + ",\"text\":" + quote(utf8(characterText(glyph.code))) +
        ",\"x\":" + std::to_string(glyph.x) + ",\"y\":" + std::to_string(glyph.y) +
        ",\"width\":" + std::to_string(glyph.width) + ",\"height\":" + std::to_string(glyph.height) + ",\"pixels\":" + std::to_string(glyph.pixels);
      std::unique_lock<std::mutex> lock(rasterMutex, std::try_to_lock);
      if (lock && glyph.pixels) {
        if (rasterGlyphs.size() >= 8192) rasterGlyphs.clear();
        rasterGlyphs[glyph.pixels] = glyph;
      }
    }
    nodes += '}';
    address = node[17];
  }
  if (live.surface && live.glyphs.size() == capture.glyphs.size()) {
    std::lock_guard<std::mutex> lock(rasterMutex);
    liveLayouts.replace(std::move(live));
  }
  if (layoutSamples.load() >= 300) return;
  layoutSamples.fetch_add(1);
  emit("{\"type\":\"text-layout-end\",\"probeVersion\":2,\"root\":" + std::to_string(root) +
    ",\"text\":" + quote(utf8(capture.text)) + ",\"layoutBytes\":" + quote(hexBytes(layout, sizeof(layout))) +
    ",\"capturedGlyphs\":" + std::to_string(capture.glyphs.size()) + ",\"nodes\":" + nodes + "]}");
}
static void __cdecl recordBlitReady(Registers* r) {
  uint32_t descriptor[6], rectPointer = 0;
  RECT rect{};
  if (!readMemory(reinterpret_cast<void*>(r->ebp), descriptor, sizeof(descriptor)) ||
      !stackValue(r, 0x54, rectPointer) || !readMemory(reinterpret_cast<void*>(rectPointer), &rect, sizeof(rect))) return;
  LayoutGlyph glyph;
  bool liveMatched = false;
  {
    std::lock_guard<std::mutex> lock(rasterMutex);
    if (liveAdapter && liveLayouts.layouts.count(r->ebx)) {
      int32_t origin[2];
      if (readMemory(reinterpret_cast<void*>(r->ebx + bgi1528::surfaceOriginX), origin, sizeof(origin))) {
        liveMatched = liveLayouts.draw(r->ebx, descriptor[0], descriptor[2], descriptor[3],
          {rect.left, rect.top, rect.right - rect.left + 1, rect.bottom - rect.top + 1}, origin[0], origin[1]);
      }
    }
    if (blitSamples.load() >= 4000) return;
    auto found = rasterGlyphs.find(descriptor[0]);
    if (found == rasterGlyphs.end()) return;
    auto& stored = found->second;
    if (stored.drawn && EqualRect(&stored.lastRect, &rect)) return;
    stored.lastRect = rect; stored.drawn = true; glyph = stored;
  }
  blitSamples.fetch_add(1);
  emit("{\"type\":\"glyph-blit\",\"probeVersion\":2,\"root\":" + std::to_string(glyph.root) +
    ",\"node\":" + std::to_string(glyph.node) + ",\"code\":" + std::to_string(glyph.code) + ",\"text\":" + quote(utf8(characterText(glyph.code))) +
    ",\"surface\":" + std::to_string(r->ebx) + ",\"x\":" + std::to_string(rect.left) + ",\"y\":" + std::to_string(rect.top) +
    ",\"right\":" + std::to_string(rect.right) + ",\"bottom\":" + std::to_string(rect.bottom) +
    ",\"requestedX\":" + std::to_string(static_cast<int32_t>(r->esi)) + ",\"requestedY\":" + std::to_string(static_cast<int32_t>(r->edi)) +
    ",\"bitmapBytes\":" + quote(hexBytes(descriptor, sizeof(descriptor))) + ",\"surfaceBytes\":" + quote(hexBytes(reinterpret_cast<void*>(r->ebx), 0x3c0)) +
    ",\"liveMatched\":" + (liveMatched ? "true" : "false") + ",\"coordinateStatus\":\"needs-visual-confirmation\"}");
}
static void __cdecl recordTextReset(Registers* r) {
  resetRoot = r->ecx + bgi1528::objectRoot; resetSurface = 0;
  uint32_t surface = 0, vtable = 0;
  if (readMemory(reinterpret_cast<void*>(r->ecx + bgi1528::objectSurface), &surface, sizeof(surface)) &&
      readMemory(reinterpret_cast<void*>(surface), &vtable, sizeof(vtable)) && vtable == imageBase + bgi1528::surfaceVtable) {
    resetSurface = surface;
    std::lock_guard<std::mutex> lock(rasterMutex);
    liveLayouts.clearSurface(surface);
  }
  uint32_t text = 0; stackValue(r, 4, text);
  const auto decoded = pointerText(text, 8192);
  if (japanese(decoded) && layoutSamples.load() < 300) emit("{\"type\":\"text-object-reset\",\"object\":" + std::to_string(r->ecx) +
    ",\"surfacePointer\":" + quote(hexBytes(reinterpret_cast<void*>(r->ecx + 0x20), 4)) + ",\"text\":" + quote(utf8(decoded)) +
    ",\"objectBytes\":" + quote(hexBytes(reinterpret_cast<void*>(r->ecx), 0xe0)) + "}");
}
static void __cdecl recordLayoutClear(Registers* r) {
  uint32_t root = 0;
  if (!stackValue(r, 4, root)) return;
  std::lock_guard<std::mutex> lock(rasterMutex);
  if (liveLayouts.releaseRoot(root) && clearSamples.fetch_add(1) < 1000) emit("{\"type\":\"text-layout-release\",\"root\":" + std::to_string(root) + "}");
}
static void __cdecl recordSurfaceClear(Registers* r) {
  std::lock_guard<std::mutex> lock(rasterMutex);
  if (!liveLayouts.layouts.count(r->ecx)) return;
  liveLayouts.eraseSurface(r->ecx);
  if (clearSamples.fetch_add(1) < 1000) emit("{\"type\":\"surface-clear\",\"surface\":" + std::to_string(r->ecx) + "}");
}

static void publishLiveDialogue(std::string& lastPayload) {
  if (!liveAdapter) return;
  std::string text, glyphs;
  unsigned offset = 0, count = 0;
  uint64_t page = 0;
  {
    std::unique_lock<std::mutex> lock(rasterMutex, std::try_to_lock);
    if (!lock) return;
    struct Visible { const study::Layout* layout; int x, y; };
    std::vector<Visible> visible;
    for (auto it = liveLayouts.layouts.begin(); it != liveLayouts.layouts.end();) {
      uint32_t surface[0xa4 / 4];
      if (!readMemory(reinterpret_cast<void*>(it->first), surface, sizeof(surface)) || surface[0] != imageBase + bgi1528::surfaceVtable) {
        it = liveLayouts.layouts.erase(it); ++liveLayouts.page; continue;
      }
      // These fields are the exact predicate of the surface's IsVisible virtual
      // method (RVA 0x12f80); hiding a parent propagates to its child surfaces.
      if (surface[1] && surface[2] && surface[0xa0 / 4] < 0x100 &&
          std::any_of(it->second.glyphs.begin(), it->second.glyphs.end(), [](const auto& g) { return g.drawn; })) {
        visible.push_back({&it->second, static_cast<int32_t>(surface[bgi1528::surfaceOriginX / 4]), static_cast<int32_t>(surface[bgi1528::surfaceOriginY / 4])});
      }
      ++it;
    }
    std::sort(visible.begin(), visible.end(), [](const auto& a, const auto& b) { return a.y == b.y ? a.x < b.x : a.y < b.y; });
    for (const auto& entry : visible) {
      const auto& layout = *entry.layout;
      if (offset + layout.glyphs.back().end + 1 > 16384) break;
      if (!text.empty()) { text += '\n'; ++offset; }
      text += layout.text;
      for (size_t i = 0; i < layout.glyphs.size(); ++i) {
        const auto& glyph = layout.glyphs[i];
        if (!glyph.drawn) continue;
        auto box = glyph.box;
        // Font bitmap padding overlaps the next glyph by two pixels in the
        // supplied game. Hit regions stop at the next actual advance.
        if (i + 1 < layout.glyphs.size()) {
          const auto& next = layout.glyphs[i + 1];
          if (next.drawn && next.box.y == box.y && next.box.x > box.x) box.width = std::min(box.width, next.box.x - box.x);
        }
        box.x += entry.x; box.y += entry.y;
        const int right = std::min<int>(canvasWidth, box.x + box.width), bottom = std::min<int>(canvasHeight, box.y + box.height);
        box.x = std::max(0, box.x); box.y = std::max(0, box.y);
        box.width = right - box.x; box.height = bottom - box.y;
        if (box.width <= 0 || box.height <= 0) continue;
        if (count++) glyphs += ',';
        glyphs += "{\"start\":" + std::to_string(offset + glyph.start) + ",\"end\":" + std::to_string(offset + glyph.end) +
          ",\"x\":" + std::to_string(box.x) + ",\"y\":" + std::to_string(box.y) + ",\"width\":" + std::to_string(box.width) + ",\"height\":" + std::to_string(box.height) + '}';
      }
      offset += layout.glyphs.back().end;
    }
    page = liveLayouts.page;
  }
  const auto payload = count ? "{\"type\":\"dialogue\",\"engineVersion\":4,\"page\":" + std::to_string(page) +
    ",\"width\":" + std::to_string(canvasWidth) + ",\"height\":" + std::to_string(canvasHeight) + ",\"fit\":" + quote(canvasFit) +
    ",\"text\":" + quote(text) + ",\"glyphs\":[" + glyphs + "]}" : std::string("{\"type\":\"clear\"}");
  if (payload != lastPayload && emit(payload)) lastPayload = payload;
}
struct GameWindowGeometry { LONG area = 0; RECT client{}; };
static BOOL CALLBACK inspectGameWindow(HWND window, LPARAM parameter) {
  DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
  if (pid != GetCurrentProcessId() || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
  RECT rect{};
  if (!GetClientRect(window, &rect)) return TRUE;
  auto& result = *reinterpret_cast<GameWindowGeometry*>(parameter);
  const LONG area = (rect.right - rect.left) * (rect.bottom - rect.top);
  if (area > result.area) { result.area = area; result.client = rect; }
  return TRUE;
}
struct ValueSource { std::string kind; uint32_t offset = 0; int32_t displacement = 0; };
static ValueSource charSource, xSource, ySource, widthSource, heightSource;
static bool readValue(const ValueSource& source, const Registers* registers, uint32_t& value) {
  if (source.kind == "constant") { value = source.offset; return true; }
  if (source.kind == "stack") return readMemory(reinterpret_cast<void*>(originalStack(registers) + source.offset), &value, sizeof(value));
  const std::array<std::pair<const char*, uint32_t>, 7> values{{{"eax", registers->eax}, {"ebx", registers->ebx}, {"ecx", registers->ecx}, {"edx", registers->edx}, {"esi", registers->esi}, {"edi", registers->edi}, {"ebp", registers->ebp}}};
  for (const auto& pair : values) if (source.kind == pair.first) {
    value = pair.second;
    if (source.displacement) return readMemory(reinterpret_cast<void*>(value + source.displacement), &value, sizeof(value));
    return true;
  }
  return false;
}
static void __cdecl recordGlyph(Registers* registers) {
  if (!verifiedProfile) return;
  uint32_t code, x, y, width, height;
  if (!readValue(charSource, registers, code) || !readValue(xSource, registers, x) || !readValue(ySource, registers, y) ||
      !readValue(widthSource, registers, width) || !readValue(heightSource, registers, height)) return;
  if (!width || width > 256 || !height || height > 256 || x >= canvasWidth || y >= canvasHeight) return;
  std::wstring text;
  if (glyphCodec == "utf16") { if (code >= 0x20 && code <= 0xffff && !(code >= 0xd800 && code <= 0xdfff)) text += static_cast<wchar_t>(code); }
  else if (glyphCodec == "sjis-be") {
    std::string bytes;
    if (code > 255) bytes += static_cast<char>((code >> 8) & 255);
    bytes += static_cast<char>(code & 255); text = wide(bytes, 932);
  }
  if (text.empty()) return;
  emit("{\"type\":\"glyph\",\"text\":" + quote(utf8(text)) + ",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
    ",\"width\":" + std::to_string(width) + ",\"height\":" + std::to_string(height) +
    ",\"canvasWidth\":" + std::to_string(canvasWidth) + ",\"canvasHeight\":" + std::to_string(canvasHeight) + ",\"fit\":" + quote(canvasFit) + "}");
}
static void __cdecl recordClear(Registers*) { emit("{\"type\":\"clear\"}"); }
static void __cdecl recordCandidate(Registers* registers) {
  if (sampleCount.load() >= 3000) return;
  uint32_t values[17]{};
  if (!readMemory(reinterpret_cast<void*>(originalStack(registers)), values, sizeof(values))) return;
  std::string arguments = "["; bool hasJapanese = false;
  for (unsigned i = 1; i < 17; ++i) {
    auto text = pointerText(values[i]);
    if (i > 1) arguments += ',';
    arguments += "{\"stackOffset\":" + std::to_string(i * 4) + ",\"value\":" + std::to_string(values[i]);
    if (japanese(text)) { arguments += ",\"sjisText\":" + quote(utf8(text)); hasJapanese = true; }
    if (values[i] >= 0x10000) arguments += ",\"pointedBytes\":" + quote(hexBytes(reinterpret_cast<void*>(values[i]), 48));
    arguments += '}';
  }
  if (!hasJapanese) return;
  sampleCount.fetch_add(1);
  emit("{\"type\":\"engine-sample\",\"returnRva\":" + std::to_string(values[0] - imageBase) +
    ",\"arguments\":" + arguments + "],\"registers\":{\"eax\":" + std::to_string(registers->eax) + ",\"ecx\":" + std::to_string(registers->ecx) +
    ",\"edx\":" + std::to_string(registers->edx) + ",\"esi\":" + std::to_string(registers->esi) + ",\"edi\":" + std::to_string(registers->edi) + "}}");
}
static bool hookFunction(void* target, void(__cdecl* callback)(Registers*)) {
  if (installedHooks.count(target)) return MH_EnableHook(target) == MH_OK;
  // x86 bridge preserves flags, all general registers, x87 and SSE state.
  // MinHook relocates the original prologue into its own trampoline.
  auto* code = static_cast<unsigned char*>(VirtualAlloc(nullptr, 128, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
  if (!code) return false;
  void* trampoline = nullptr;
  if (MH_CreateHook(target, code, &trampoline) != MH_OK) { VirtualFree(code, 0, MEM_RELEASE); return false; }
  std::vector<unsigned char> bytes;
  auto append = [&](std::initializer_list<unsigned char> v) { bytes.insert(bytes.end(), v); };
  auto dword = [&](uint32_t v) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<unsigned char>(v >> (i * 8))); };
  append({0x9c, 0x60, 0xfc, 0x89, 0xe0}); // pushfd; pushad; cld; mov eax,esp
  append({0x81, 0xec}); dword(528); // sub esp,528
  append({0x83, 0xe4, 0xf0}); // and esp,-16
  append({0x0f, 0xae, 0x04, 0x24}); // fxsave [esp]
  append({0x89, 0x84, 0x24}); dword(512); // mov [esp+512],eax
  append({0x83, 0xec, 0x0c, 0x50, 0xb8}); dword(reinterpret_cast<uintptr_t>(callback)); // pad 12; push eax; mov eax,callback
  append({0xff, 0xd0, 0x83, 0xc4, 0x10}); // call eax (16-byte aligned); add esp,16
  append({0x0f, 0xae, 0x0c, 0x24}); // fxrstor [esp]
  append({0x8b, 0xa4, 0x24}); dword(512); // mov esp,[esp+512]
  append({0x61, 0x9d, 0xe9}); // popad; popfd; jmp trampoline
  dword(reinterpret_cast<uintptr_t>(trampoline) - (reinterpret_cast<uintptr_t>(code) + bytes.size() + 4));
  std::copy(bytes.begin(), bytes.end(), code);
  FlushInstructionCache(GetCurrentProcess(), code, bytes.size());
  DWORD old; VirtualProtect(code, 128, PAGE_EXECUTE_READ, &old);
  stubs.push_back(code);
  installedHooks[target] = true;
  return MH_EnableHook(target) == MH_OK;
}
static std::string ini(const wchar_t* section, const wchar_t* name) {
  wchar_t buffer[512];
  GetPrivateProfileStringW(section, name, L"", buffer, 512, configuration.profile);
  return utf8(buffer);
}
static bool parseSource(const wchar_t* key, ValueSource& source) {
  const auto value = ini(L"Glyph", key);
  const auto colon = value.find(':');
  if (colon == std::string::npos) return false;
  source.kind = value.substr(0, colon);
  char* end = nullptr;
  const long number = std::strtol(value.c_str() + colon + 1, &end, 0);
  if (*end) return false;
  if (source.kind == "constant") { if (number < 0 || number > 16384) return false; source.offset = number; return true; }
  if (source.kind == "stack") { if (number < 4 || number > 256 || number % 4) return false; source.offset = number; return true; }
  if (source.kind == "eax" || source.kind == "ebx" || source.kind == "ecx" || source.kind == "edx" || source.kind == "esi" || source.kind == "edi" || source.kind == "ebp") {
    if (number < -4096 || number > 4096) return false;
    source.displacement = number; return true;
  }
  return false;
}
static void* profileTarget(const wchar_t* section) {
  const auto rvaText = ini(section, L"Rva"), signature = ini(section, L"Signature");
  char* end = nullptr; unsigned long rva = std::strtoul(rvaText.c_str(), &end, 0);
  if (rvaText.empty() || *end || !rva || rva >= imageSize || signature.size() < 16 || signature.size() > 128 || signature.size() % 2) return nullptr;
  auto* address = reinterpret_cast<void*>(imageBase + rva);
  if (hexBytes(address, signature.size() / 2) != signature) return nullptr;
  MEMORY_BASIC_INFORMATION memory{};
  if (!VirtualQuery(address, &memory, sizeof(memory)) || !(memory.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return nullptr;
  return address;
}
static bool installProfile() {
  if (ini(L"Engine", L"Verified") != "1") return false;
  if (ini(L"Engine", L"Sha256") != "2cc889b0e60f5663a0adc6c1b3bc598ac0adb3d6b39b7d5eae6391cadd209c88") return false;
  canvasWidth = GetPrivateProfileIntW(L"Engine", L"CanvasWidth", 0, configuration.profile);
  canvasHeight = GetPrivateProfileIntW(L"Engine", L"CanvasHeight", 0, configuration.profile);
  glyphCodec = ini(L"Glyph", L"Codec");
  canvasFit = ini(L"Engine", L"Fit");
  if (canvasFit != "contain" && canvasFit != "stretch") return false;
  if (!canvasWidth || canvasWidth > 16384 || !canvasHeight || canvasHeight > 16384 || (glyphCodec != "utf16" && glyphCodec != "sjis-be")) return false;
  if (!parseSource(L"Character", charSource) || !parseSource(L"X", xSource) || !parseSource(L"Y", ySource) || !parseSource(L"Width", widthSource) || !parseSource(L"Height", heightSource)) return false;
  void* glyph = profileTarget(L"Glyph"), *clear = profileTarget(L"Clear");
  if (!glyph || !clear) return false;
  if (!hookFunction(clear, recordClear) || !hookFunction(glyph, recordGlyph)) return false;
  verifiedProfile = true;
  status("BGI coordinate hook installed.", "Hover for furigana; Shift + hover for Yomitan."); return true;
}
static bool installKnownLayoutProbes() {
  struct Site { unsigned rva; const char* signature; const char* name; void(__cdecl* callback)(Registers*); };
  const Site sites[] = {
    {bgi1528::parser, bgi1528::parserSignature, "text-parser", recordParserBegin},
    {bgi1528::glyphLayout, bgi1528::glyphLayoutSignature, "glyph-layout", recordLayoutGlyph},
    {bgi1528::layoutReturn, bgi1528::layoutReturnSignature, "layout-return", recordLayoutReturn},
    {bgi1528::blitReady, bgi1528::blitReadySignature, "glyph-blit", recordBlitReady},
    {bgi1528::textReset, bgi1528::textResetSignature, "text-reset", recordTextReset},
    {bgi1528::layoutClear, bgi1528::layoutClearSignature, "layout-release", recordLayoutClear},
    {bgi1528::surfaceClear, bgi1528::surfaceClearSignature, "surface-clear", recordSurfaceClear}
  };
  // Check the complete set before modifying any engine instruction.
  for (const auto& site : sites) {
    const auto address = reinterpret_cast<void*>(imageBase + site.rva);
    if (site.rva >= imageSize || (!installedHooks.count(address) &&
        hexBytes(address, std::strlen(site.signature) / 2) != site.signature)) return false;
  }
  unsigned installed = 0;
  for (const auto& site : sites) {
    const bool ok = hookFunction(reinterpret_cast<void*>(imageBase + site.rva), site.callback);
    if (ok) ++installed;
    emit("{\"type\":\"probe-site\",\"probeVersion\":4,\"name\":" + quote(site.name) + ",\"rva\":" +
      std::to_string(site.rva) + ",\"installed\":" + (ok ? "true" : "false") + "}");
  }
  if (installed == sizeof(sites) / sizeof(sites[0])) {
    canvasWidth = 800; canvasHeight = 600; canvasFit = "contain";
    liveAdapter = ini(L"Engine", L"Adapter") == "bgi-1.528" && ini(L"Engine", L"Live") == "1";
    if (liveAdapter) status("BGI text hover is active (adapter v4).", "Advance one dialogue page. Hover remains available after the text finishes; Shift + hover for Yomitan.");
    else status("BGI layout probe is active.", "The live adapter is disabled in the engine profile.");
  }
  else status("Some BGI adapter v4 hooks could not be installed.", "Export diagnostics to inspect the hook installation results.");
  return true;
}
static void inspectCandidates() {
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(imageBase);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(imageBase + dos->e_lfanew);
  auto* sections = IMAGE_FIRST_SECTION(nt);
  const unsigned char pattern[] = {0x0f, 0xbe, 0xc0, 0x83, 0xc0, 0xfe, 0x83, 0xf8, 0x06};
  unsigned count = 0;
  for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s) {
    if (!(sections[s].Characteristics & IMAGE_SCN_CNT_CODE) || std::string(reinterpret_cast<char*>(sections[s].Name), 5) != ".text") continue;
    DWORD size = sections[s].Misc.VirtualSize;
    if (size > 16 * 1024 * 1024) continue;
    std::vector<unsigned char> bytes(size);
    if (!readMemory(reinterpret_cast<void*>(imageBase + sections[s].VirtualAddress), bytes.data(), size)) continue;
    for (size_t i = 0x200; i + sizeof(pattern) < bytes.size() && count < 4; ++i) {
      if (!std::equal(std::begin(pattern), std::end(pattern), bytes.begin() + i)) continue;
      // Signature family used by BGI's text parser. These are diagnostic hooks;
      // no guessed position is ever promoted to a dialogue/glyph message.
      for (size_t back = i; back > i - 0x200; --back) {
        const bool padding = bytes[back - 1] == 0xcc || bytes[back - 1] == 0x90;
        const bool prologue = (bytes[back] == 0x55 && bytes[back + 1] == 0x8b && bytes[back + 2] == 0xec) ||
          (bytes[back] == 0x6a && bytes[back + 1] == 0xff && bytes[back + 2] == 0x68) ||
          ((bytes[back] == 0x83 || bytes[back] == 0x81) && bytes[back + 1] == 0xec);
        if (!padding || !prologue || ((sections[s].VirtualAddress + back) & 15)) continue;
        const uintptr_t rva = sections[s].VirtualAddress + back;
        if (hookFunction(reinterpret_cast<void*>(imageBase + rva), recordCandidate)) {
          emit("{\"type\":\"candidate\",\"rva\":" + std::to_string(rva) + ",\"patternRva\":" + std::to_string(sections[s].VirtualAddress + i) + "}"); count++;
        }
        break;
      }
    }
  }
  emit("{\"type\":\"candidate-scan\",\"count\":" + std::to_string(count) + "}");
}
static void installGdiProbes() {
  // Generic GDI probes are a fallback for unknown engines. The verified BGI
  // adapter gets its text and geometry directly from its seven engine sites.
  const auto a = MH_CreateHookApi(L"gdi32.dll", "TextOutA", reinterpret_cast<void*>(hookTextOutA), reinterpret_cast<void**>(&originalTextOutA));
  const auto w = MH_CreateHookApi(L"gdi32.dll", "TextOutW", reinterpret_cast<void*>(hookTextOutW), reinterpret_cast<void**>(&originalTextOutW));
  if (a == MH_OK || a == MH_ERROR_ALREADY_CREATED) MH_EnableHook(reinterpret_cast<void*>(TextOutA));
  if (w == MH_OK || w == MH_ERROR_ALREADY_CREATED) MH_EnableHook(reinterpret_cast<void*>(TextOutW));
  if (a != MH_OK && a != MH_ERROR_ALREADY_CREATED && w != MH_OK && w != MH_ERROR_ALREADY_CREATED) status("The GDI probe could not be installed.", MH_StatusToString(a));
}
static DWORD WINAPI run(void* parameter) {
  const HANDLE pipe = static_cast<HANDLE>(parameter);
  if (!startupStatus(pipe, "Engine worker started; waiting for BGI initialization…", "Text hooks will be installed in about six seconds.")) {
    CloseHandle(pipe); running = false; return 1;
  }
  auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(imageBase);
  auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(imageBase + dos->e_lfanew);
  imageSize = nt->OptionalHeader.SizeOfImage;
  GameWindowGeometry geometry;
  EnumWindows(inspectGameWindow, reinterpret_cast<LPARAM>(&geometry));
  emit("{\"type\":\"engine-window-geometry\",\"probeVersion\":2,\"width\":" + std::to_string(geometry.client.right) +
    ",\"height\":" + std::to_string(geometry.client.bottom) + ",\"dpiAware\":" + (IsProcessDPIAware() ? "true" : "false") +
    ",\"coordinateSpace\":\"game-thread-client\"}");
  const ULONGLONG started = GetTickCount64();
  bool scanned = false, profileInstalled = false;
  std::string pendingWrite, lastLivePayload;
  ULONGLONG lastPublished = 0;
  while (running) {
    // Delay profile resolution and parser inspection until the executable's
    // normal startup has had a chance to unpack its code.
    if (!scanned && GetTickCount64() - started > 6000) {
      if (!startupStatus(pipe, "Installing BGI text hooks…", "Initializing the hook library after engine startup.")) break;
      const auto initialized = MH_Initialize();
      if (initialized != MH_OK && initialized != MH_ERROR_ALREADY_INITIALIZED) {
        startupStatus(pipe, "MinHook could not be initialized.", MH_StatusToString(initialized));
        break;
      }
      profileInstalled = installProfile();
      if (!profileInstalled && !installKnownLayoutProbes()) {
        installGdiProbes();
        inspectCandidates();
        status("The engine signatures do not match.", "Diagnostic mode is active. Export diagnostics to inspect this engine version.");
      }
      emit(std::string("{\"type\":\"adapter-ready\",\"live\":") + (verifiedProfile || liveAdapter ? "true" : "false") + "}");
      scanned = true;
    }
    if (GetTickCount64() - lastPublished >= 30) {
      publishLiveDialogue(lastLivePayload);
      lastPublished = GetTickCount64();
    }
    if (pendingWrite.empty()) {
      std::lock_guard<std::mutex> lock(queueMutex);
      while (!queue.empty() && pendingWrite.size() < 60000) { pendingWrite += queue.front(); queue.pop_front(); }
    }
    if (!pendingWrite.empty()) {
      DWORD written = 0;
      if (!WriteFile(pipe, pendingWrite.data(), pendingWrite.size(), &written, nullptr)) break;
      pendingWrite.erase(0, written);
    }
    DWORD available = 0;
    if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) break;
    if (available) { char command[64]; DWORD read; if (ReadFile(pipe, command, sizeof(command), &read, nullptr) && read) break; }
    Sleep(10);
  }
  verifiedProfile = false;
  liveAdapter = false;
  // Disable first; leave stubs and this module resident. A thread already inside
  // a detour may still be returning through the trampoline. Unloading here
  // would race the game and could crash it. A new attachment reuses this DLL.
  MH_DisableHook(MH_ALL_HOOKS);
  CloseHandle(pipe);
  running = false;
  return 0;
}
extern "C" __declspec(dllexport) DWORD WINAPI StudyStart(StudyInit* remote) {
  StudyInit init{};
  if (!readMemory(remote, &init, sizeof(init)) || init.size != sizeof(init)) return studyStartFailure(ERROR_INVALID_PARAMETER);
  bool expected = false;
  if (!running.compare_exchange_strong(expected, true)) return studyStartFailure(ERROR_ALREADY_EXISTS);
  // Keep existing trampolines resident and re-enable them on reattachment;
  // an in-flight game call can still be using one after a detach.
  configuration = init;
  sampleCount = 0; layoutSamples = 0; blitSamples = 0; clearSamples = 0; verifiedProfile = false; liveAdapter = false;
  { std::lock_guard<std::mutex> lock(rasterMutex); rasterGlyphs.clear(); liveLayouts = study::LayoutStore{}; }
  { std::lock_guard<std::mutex> lock(queueMutex); queue.clear(); }
  // Establish IPC before reporting a successful remote entry point. The old
  // entry returned 1 even if its worker later failed to open the pipe.
  HANDLE pipe = INVALID_HANDLE_VALUE;
  DWORD pipeError = ERROR_FILE_NOT_FOUND;
  for (unsigned attempt = 0; attempt < 100; ++attempt) {
    pipe = CreateFileW(configuration.pipe, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) break;
    pipeError = GetLastError();
    if (pipeError != ERROR_FILE_NOT_FOUND && pipeError != ERROR_PIPE_BUSY) break;
    Sleep(50);
  }
  if (pipe == INVALID_HANDLE_VALUE) { running = false; return studyStartFailure(pipeError); }
  if (!startupStatus(pipe, "Engine connection established.", "Waiting for the hook worker to start.")) {
    const auto error = GetLastError(); CloseHandle(pipe); running = false; return studyStartFailure(error);
  }
  HANDLE thread = CreateThread(nullptr, 0, run, pipe, 0, nullptr);
  if (!thread) { const auto error = GetLastError(); CloseHandle(pipe); running = false; return studyStartFailure(error); }
  CloseHandle(thread); return 1;
}
BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    imageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); DisableThreadLibraryCalls(module);
  }
  return TRUE;
}
