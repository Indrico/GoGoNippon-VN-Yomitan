#include "../native/layout-state.hpp"
#include <cassert>
#include <algorithm>
#include <iostream>
#include <sstream>

static std::string unhex(const std::string& hex) {
  std::string bytes;
  for (size_t i = 0; i < hex.size(); i += 2) bytes += static_cast<char>(std::stoul(hex.substr(i, 2), nullptr, 16));
  return bytes;
}
int main() {
  study::LayoutStore store;
  char command;
  unsigned accepted = 0, rejected = 0, pages = 0, released = 0;
  while (std::cin >> command) {
    if (command == 'R') {
      uintptr_t surface; std::cin >> surface;
      store.clearSurface(surface);
    } else if (command == 'L') {
      study::Layout layout; unsigned count;
      std::cin >> layout.root >> layout.surface >> count;
      for (unsigned i = 0; i < count; ++i) {
        study::Glyph glyph; std::string text;
        std::cin >> glyph.node >> glyph.pixels >> glyph.bitmapWidth >> glyph.bitmapHeight >> glyph.units >> text;
        glyph.text = unhex(text); layout.glyphs.push_back(glyph);
      }
      std::string text; std::cin >> text;
      const auto surface = layout.surface;
      assert(store.replace(std::move(layout)));
      assert(store.layouts.at(surface).text == unhex(text));
      assert(std::none_of(store.layouts.at(surface).glyphs.begin(), store.layouts.at(surface).glyphs.end(), [](auto& g) { return g.drawn; }));
      ++pages;
    } else if (command == 'F') {
      uintptr_t root; std::cin >> root;
      const auto before = store.layouts;
      const auto page = store.page;
      assert(store.releaseRoot(root));
      assert(store.page == page && store.layouts.size() == before.size());
      for (const auto& entry : before) {
        const auto& after = store.layouts.at(entry.first);
        assert(after.text == entry.second.text && after.glyphs.size() == entry.second.glyphs.size());
        for (size_t i = 0; i < after.glyphs.size(); ++i) {
          const auto& glyph = after.glyphs[i]; const auto& old = entry.second.glyphs[i];
          assert(glyph.drawn == old.drawn && glyph.box.x == old.box.x && glyph.box.y == old.box.y);
          assert(glyph.box.width == old.box.width && glyph.box.height == old.box.height);
          if (after.root == root && old.drawn) {
            assert(!store.draw(after.surface, glyph.pixels, glyph.bitmapWidth, glyph.bitmapHeight, {21, 435, 21, 21}, 21, 435));
          }
        }
      }
      ++released;
    } else if (command == 'B') {
      uintptr_t surface, pixels, node; int width, height, ox, oy, expected;
      study::Box box;
      std::cin >> surface >> pixels >> width >> height >> box.x >> box.y >> box.width >> box.height >> ox >> oy >> expected >> node;
      const auto matched = store.draw(surface, pixels, width, height, box, ox, oy);
      assert(matched == bool(expected));
      if (matched) {
        ++accepted;
        const auto& glyphs = store.layouts.at(surface).glyphs;
        const auto found = std::find_if(glyphs.begin(), glyphs.end(), [&](auto& g) { return g.node == node; });
        assert(found != glyphs.end() && found->drawn);
        assert(found->box.x + ox == box.x && found->box.y + oy == box.y);
        assert(found->box.width == box.width && found->box.height == box.height);
      } else ++rejected;
    } else return 2;
  }
  // Freed allocation addresses must not authorize a new draw. A destructor of
  // an older object on the same surface must not clear its replacement.
  study::Layout layout;
  layout.root = 10; layout.surface = 20;
  study::Glyph glyph; glyph.node = 30; glyph.pixels = 40;
  glyph.bitmapWidth = glyph.bitmapHeight = 21; glyph.text = "A"; glyph.units = 1;
  layout.glyphs.push_back(glyph);
  store = study::LayoutStore{};
  assert(store.replace(layout));
  assert(!store.draw(99, 40, 21, 21, {21, 435, 21, 21}, 21, 435));
  assert(!store.draw(20, 40, 12, 23, {21, 435, 12, 23}, 21, 435));
  assert(store.draw(20, 40, 21, 21, {21, 435, 21, 21}, 21, 435));
  layout.root = 11;
  assert(store.replace(layout));
  store.clearRoot(10);
  assert(store.layouts.count(20));
  store.clearRoot(11);
  assert(!store.draw(20, 40, 21, 21, {21, 435, 21, 21}, 21, 435));
  assert(store.replace(layout));
  store.clearSurface(20);
  assert(store.layouts.empty());
  layout.root = 10;
  assert(store.replace(layout));
  assert(store.draw(20, 40, 21, 21, {21, 435, 21, 21}, 21, 435));
  const auto page = store.page;
  assert(store.releaseRoot(10));
  assert(store.page == page && store.layouts.at(20).glyphs[0].drawn);
  assert(!store.draw(20, 40, 21, 21, {99, 99, 21, 21}, 21, 435));
  layout.surface = 21; // The engine can reuse the freed root on another surface.
  assert(store.replace(layout) && store.layouts.count(20));
  assert(store.releaseRoot(10));
  store.eraseSurface(20);
  assert(!store.layouts.count(20) && store.layouts.count(21));
  store.eraseSurface(21);
  assert(store.layouts.empty());
  assert(store.replace(layout));
  store.eraseSurface(21); // A clear before the first draw must permit rendering.
  assert(store.draw(21, 40, 21, 21, {21, 435, 21, 21}, 21, 435));
  store.eraseSurface(21);
  assert(!store.layouts.at(21).glyphs[0].drawn);
  assert(store.draw(21, 40, 21, 21, {21, 435, 21, 21}, 21, 435));
  std::cout << "PASS " << pages << " layouts, " << accepted << " owned glyphs, " << rejected
    << " foreign raster draws rejected; " << released << " completed layouts retained after release; reset, surface erase and allocation reuse checks\n";
}
