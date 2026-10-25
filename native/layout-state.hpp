#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <utility>

// Engine-independent ownership and raster matching, shared by the DLL and
// diagnostic replay. An allocation address alone never identifies a glyph.
namespace study {
struct Box { int x = 0, y = 0, width = 0, height = 0; };
struct Glyph {
  uintptr_t node = 0, pixels = 0;
  int bitmapWidth = 0, bitmapHeight = 0;
  std::string text;
  unsigned units = 0, start = 0, end = 0;
  Box box;
  bool drawn = false;
};
struct Layout {
  uintptr_t root = 0, surface = 0;
  bool rasterActive = true;
  std::string text;
  std::vector<Glyph> glyphs;
};
class LayoutStore {
public:
  std::map<uintptr_t, Layout> layouts;
  uint64_t page = 1;

  void clearSurface(uintptr_t surface) {
    if (layouts.erase(surface)) ++page;
  }
  void clearRoot(uintptr_t root) {
    for (auto it = layouts.begin(); it != layouts.end();) {
      if (it->second.root == root && it->second.rasterActive) { it = layouts.erase(it); ++page; }
      else ++it;
    }
  }
  bool releaseRoot(uintptr_t root) {
    bool released = false;
    for (auto& entry : layouts) {
      auto& layout = entry.second;
      if (layout.root == root && layout.rasterActive) {
        // The engine frees temporary glyph allocations after drawing, while
        // the surface still displays the completed text. Retain the hit boxes
        // and revoke raster matching before these allocations can be reused.
        layout.rasterActive = false; released = true;
      }
    }
    return released;
  }
  void eraseSurface(uintptr_t surface) {
    const auto found = layouts.find(surface);
    if (found == layouts.end()) return;
    if (!found->second.rasterActive) { clearSurface(surface); return; }
    bool changed = false;
    for (auto& glyph : found->second.glyphs) { changed |= glyph.drawn; glyph.drawn = false; }
    if (changed) ++page;
  }
  bool replace(Layout layout) {
    if (!layout.surface || !layout.root || layout.glyphs.empty() || layout.glyphs.size() > 2048) return false;
    clearRoot(layout.root);
    if (layouts.size() >= 16 && !layouts.count(layout.surface)) return false;
    layout.text.clear();
    layout.rasterActive = true;
    unsigned index = 0;
    for (auto& glyph : layout.glyphs) {
      if (glyph.text.empty() || !glyph.units || glyph.units > 2) return false;
      glyph.start = index; index += glyph.units; glyph.end = index;
      glyph.drawn = false;
      layout.text += glyph.text;
    }
    layouts[layout.surface] = std::move(layout);
    ++page;
    return true;
  }
  bool draw(uintptr_t surface, uintptr_t pixels, int width, int height, Box box, int originX, int originY) {
    const auto found = layouts.find(surface);
    if (found == layouts.end() || !found->second.rasterActive || !pixels || box.width <= 0 || box.height <= 0) return false;
    for (auto& glyph : found->second.glyphs) {
      if (glyph.pixels != pixels || glyph.bitmapWidth != width || glyph.bitmapHeight != height) continue;
      glyph.box = {box.x - originX, box.y - originY, box.width, box.height};
      glyph.drawn = true;
      return true;
    }
    return false;
  }
};
}
