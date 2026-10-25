export function validateDialogue(message) {
  if (!message || message.type !== 'dialogue' || typeof message.text !== 'string' || message.text.length > 16384 ||
      !Number.isSafeInteger(message.sequence) || !Number.isSafeInteger(message.page) ||
      !Number.isFinite(message.width) || message.width <= 0 || !Number.isFinite(message.height) || message.height <= 0 ||
      !Array.isArray(message.glyphs) || message.glyphs.length > 16384) return null;
  let previous = -1;
  const glyphs = [];
  for (const glyph of message.glyphs) {
    if (!Number.isSafeInteger(glyph.start) || !Number.isSafeInteger(glyph.end) || glyph.start <= previous ||
        glyph.start < 0 || glyph.end <= glyph.start || glyph.end > message.text.length ||
        ![glyph.x, glyph.y, glyph.width, glyph.height].every(Number.isFinite) || glyph.width <= 0 || glyph.height <= 0) return null;
    previous = glyph.end - 1;
    glyphs.push({...glyph});
  }
  return {...message, glyphs};
}

export function hitGlyph(dialogue, x, y) {
  return dialogue?.glyphs.find((g) => x >= g.x && x < g.x + g.width && y >= g.y && y < g.y + g.height) ?? null;
}

export function viewportFor(dialogue, width, height) {
  if (dialogue.fit === 'stretch') return {x: 0, y: 0, sx: width / dialogue.width, sy: height / dialogue.height};
  const scale = Math.min(width / dialogue.width, height / dialogue.height);
  return {x: (width - dialogue.width * scale) / 2, y: (height - dialogue.height * scale) / 2, sx: scale, sy: scale};
}

export function tokenBox(dialogue, token, hoveredGlyph) {
  // A wrapped word gets one reading anchored on the hovered visual line.
  const glyphs = dialogue.glyphs.filter((g) => g.start < token.end && g.end > token.start &&
    Math.abs(g.y - hoveredGlyph.y) < Math.min(g.height, hoveredGlyph.height) / 2);
  if (!glyphs.length) return null;
  const x = Math.min(...glyphs.map((g) => g.x));
  const y = Math.min(...glyphs.map((g) => g.y));
  return {x, y, width: Math.max(...glyphs.map((g) => g.x + g.width)) - x, height: Math.max(...glyphs.map((g) => g.height))};
}
