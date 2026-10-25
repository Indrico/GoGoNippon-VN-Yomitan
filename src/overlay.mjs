import {validateDialogue, hitGlyph, tokenBox, viewportFor} from './text-model.mjs';
const glyphLayer = document.querySelector('#glyphs');
const furigana = document.querySelector('#furigana');
const lookupStatus = document.querySelector('#lookup-status');
let current = null, tokens = [], pointer = null, hoverTimer, statusTimer;
let popupVisible = false, yomitanReady = false, demo = false, lastLookup = '';
let popupRects = [], hoverKey = '';
const glyphNodes = [];

function closePopup() {
  window.dispatchEvent(new CustomEvent('gogonippon-close'));
  popupVisible = false;
  lastLookup = '';
  reportPopup();
}
function reset() {
  clearTimeout(hoverTimer); clearTimeout(statusTimer);
  hoverKey = '';
  current = null; tokens = []; pointer = null;
  glyphLayer.replaceChildren(); glyphNodes.length = 0;
  furigana.hidden = true; lookupStatus.hidden = true;
  closePopup();
}
function render() {
  if (!current) return;
  const {x: ox, y: oy, sx, sy} = viewportFor(current, innerWidth, innerHeight);
  const previous = new Map(glyphNodes.map((entry) => [`${entry.g.start}:${entry.g.end}`, entry]));
  const measure = [];
  let cursor = glyphLayer.firstChild;
  const place = (node) => {
    if (node === cursor) cursor = cursor.nextSibling;
    else glyphLayer.insertBefore(node, cursor);
  };
  const placeText = (text) => {
    const node = cursor?.nodeType === Node.TEXT_NODE ? cursor : document.createTextNode(text);
    if (node.data !== text) node.data = text;
    place(node);
  };
  let offset = 0;
  glyphNodes.length = 0;
  for (const g of current.glyphs) {
    // Include layout whitespace in document order, without manufacturing text.
    if (g.start > offset) placeText(current.text.slice(offset, g.start));
    const old = previous.get(`${g.start}:${g.end}`);
    const node = old?.node || document.createElement('span');
    if (!old) { node.className = 'glyph'; node.textContent = current.text.slice(g.start, g.end); }
    const style = {left: `${ox + g.x * sx}px`, top: `${oy + g.y * sy}px`,
      width: `${g.width * sx}px`, height: `${g.height * sy}px`, fontSize: `${g.height * sy}px`};
    const styleKey = JSON.stringify(style);
    if (styleKey !== old?.styleKey) {
      Object.assign(node.style, style, {transform: 'none'});
      measure.push({node, g});
    }
    place(node); glyphNodes.push({node, g, styleKey}); offset = g.end;
  }
  if (offset < current.text.length) placeText(current.text.slice(offset));
  while (cursor) { const next = cursor.nextSibling; cursor.remove(); cursor = next; }
  // Make browser caret hit testing agree with engine advance widths even when
  // the game's bundled font differs from the overlay font.
  for (const {node, g} of measure) {
    const range = document.createRange(); range.selectNodeContents(node);
    const measured = range.getBoundingClientRect().width;
    if (measured > 0) node.style.transform = `scaleX(${g.width * sx / measured})`;
  }
}
function showReading(token, glyph) {
  if (!current || popupVisible) return;
  const box = tokenBox(current, token, glyph);
  if (!box) return;
  const {x: ox, y: oy, sx, sy} = viewportFor(current, innerWidth, innerHeight);
  furigana.textContent = token.reading;
  furigana.hidden = false;
  const r = furigana.getBoundingClientRect();
  const left = Math.max(3, Math.min(innerWidth - r.width - 3, ox + (box.x + box.width / 2) * sx - r.width / 2));
  let top = oy + box.y * sy - r.height - 5;
  if (top < 3) top = oy + (box.y + box.height) * sy + 5;
  furigana.style.left = `${left}px`;
  furigana.style.top = `${Math.min(innerHeight - r.height - 3, Math.max(3, top))}px`;
}
function updatePointer(p) {
  const previous = pointer; pointer = p;
  if (!current || !p.active) {
    furigana.hidden = true; clearTimeout(hoverTimer); hoverKey = '';
    if (popupVisible) closePopup();
    return;
  }
  if (p.escape) { if (demo) void window.study.action('close-demo'); else closePopup(); return; }
  const view = viewportFor(current, innerWidth, innerHeight);
  const glyph = hitGlyph(current, (p.x - view.x) / view.sx, (p.y - view.y) / view.sy);
  if (!glyph) {
    clearTimeout(hoverTimer); furigana.hidden = true; hoverKey = '';
    if (popupVisible && !popupRects.some((r) => p.x >= r.x - 12 && p.x <= r.x + r.width + 12 && p.y >= r.y - 18 && p.y <= r.y + r.height + 18)) {
      // Allow the cursor to cross the gap between the source word and popup.
      clearTimeout(statusTimer); statusTimer = setTimeout(closePopup, 500);
    }
    lastLookup = ''; return;
  }
  clearTimeout(statusTimer);
  if (p.shift) {
    clearTimeout(hoverTimer); furigana.hidden = true; hoverKey = '';
    const key = `${current.page}:${glyph.start}`;
    if (key !== lastLookup) {
      lastLookup = key;
      if (yomitanReady) window.dispatchEvent(new CustomEvent('gogonippon-lookup', {detail: {x: p.x, y: p.y}}));
      else { lookupStatus.hidden = false; setTimeout(() => { lookupStatus.hidden = true; }, 3000); }
    }
    return;
  }
  lastLookup = '';
  const token = tokens.find((t) => glyph.start >= t.start && glyph.start < t.end);
  if (!token || popupVisible) { furigana.hidden = true; hoverKey = ''; return; }
  const nextHoverKey = `${current.page}:${token.start}:${glyph.y}`;
  if (hoverKey === nextHoverKey && previous && !previous.shift) {
    if (!furigana.hidden) showReading(token, glyph);
    return;
  }
  hoverKey = nextHoverKey;
  clearTimeout(hoverTimer); furigana.hidden = true;
  const page = current.page;
  hoverTimer = setTimeout(() => { if (current?.page === page && hoverKey === nextHoverKey && !pointer?.shift) showReading(token, glyph); }, 150);
}
function reportPopup() {
  popupRects = [...document.querySelectorAll('iframe.yomitan-popup')]
    .filter((frame) => getComputedStyle(frame).visibility === 'visible')
    .map((frame) => { const r = frame.getBoundingClientRect(); return {x: r.x, y: r.y, width: r.width, height: r.height}; });
  popupVisible = popupRects.length > 0;
  window.study.popup(popupRects);
}
window.addEventListener('gogonippon-popup', () => {
  furigana.hidden = true;
  requestAnimationFrame(reportPopup);
});
window.addEventListener('gogonippon-yomitan-ready', () => { yomitanReady = true; });
window.addEventListener('resize', () => { furigana.hidden = true; closePopup(); render(); });
window.addEventListener('mousemove', (e) => { if (demo) updatePointer({x: e.clientX, y: e.clientY, shift: e.shiftKey, active: true}); });
window.addEventListener('keydown', (e) => {
  if (demo && e.key === 'Escape') void window.study.action('close-demo');
  if (demo && e.key === 'Shift' && pointer) updatePointer({...pointer, shift: true});
});
window.addEventListener('keyup', (e) => { if (demo && e.key === 'Shift' && pointer) updatePointer({...pointer, shift: false}); });
window.study.on((message) => {
  if (message.type === 'demo') {
    demo = true; document.body.classList.add('demo'); document.querySelector('#demo-scene').hidden = false;
  } else if (message.type === 'dialogue') {
    const next = validateDialogue(message);
    if (!next || (current && next.sequence <= current.sequence)) return;
    const previousPointer = pointer;
    const samePage = current && next.page === current.page && next.text === current.text &&
      next.width === current.width && next.height === current.height && next.fit === current.fit;
    // Reveal updates extend the same sentence. Preserve its DOM ranges, token
    // readings, hover timer and open dictionary until the actual page changes.
    if (!samePage) reset();
    current = next; render();
    if (previousPointer) updatePointer(previousPointer);
  } else if (message.type === 'tokens' && current && message.id === `${current.page}:${current.sequence}`) {
    tokens = message.tokens;
    if (pointer) updatePointer({...pointer});
  } else if (message.type === 'pointer') updatePointer(message);
  else if (message.type === 'clear') reset();
});
// Popup dimensions can change after dictionary images and fonts have loaded.
setInterval(() => { if (popupVisible) reportPopup(); }, 150);
window.study.ready();
