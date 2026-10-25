const assert = require('node:assert/strict');
const {Worker} = require('node:worker_threads');
const {once} = require('node:events');
const fs = require('node:fs');
const path = require('node:path');
const os = require('node:os');
const {execFileSync} = require('node:child_process');
const {writeZip} = require('../src/zip.cjs');

(async () => {
  const {validateDialogue, hitGlyph, tokenBox, viewportFor} = await import('../src/text-model.mjs');
  const page = {type: 'dialogue', page: 1, sequence: 1, text: '日本\n語', width: 100, height: 100,
    glyphs: [{start: 0, end: 1, x: 10, y: 40, width: 20, height: 20},
      {start: 1, end: 2, x: 30, y: 40, width: 20, height: 20},
      {start: 3, end: 4, x: 10, y: 70, width: 20, height: 20}]};
  assert.ok(validateDialogue(page));
  assert.equal(validateDialogue({...page, glyphs: [{...page.glyphs[0], end: 99}]}), null);
  assert.equal(validateDialogue({...page, glyphs: [{...page.glyphs[0], width: NaN}]}), null);
  assert.equal(validateDialogue({...page, glyphs: [page.glyphs[1], page.glyphs[0]]}), null);
  assert.equal(hitGlyph(page, 29, 50).start, 0);
  assert.equal(hitGlyph(page, 30, 50).start, 1);
  assert.equal(hitGlyph(page, 60, 50), null);
  assert.deepEqual(tokenBox(page, {start: 0, end: 4}, page.glyphs[0]), {x: 10, y: 40, width: 40, height: 20});
  assert.deepEqual(tokenBox(page, {start: 0, end: 4}, page.glyphs[2]), {x: 10, y: 70, width: 20, height: 20});
  assert.deepEqual(viewportFor(page, 200, 100), {x: 50, y: 0, sx: 1, sy: 1});
  assert.deepEqual(viewportFor(page, 100, 200), {x: 0, y: 50, sx: 1, sy: 1});
  assert.deepEqual(viewportFor({...page, fit: 'stretch'}, 200, 100), {x: 0, y: 0, sx: 2, sy: 1});
  console.log('PASS dialogue validation, hit testing, wrapped-word anchoring, letterboxing');
  const worker = new Worker(path.join(__dirname, '../src/furigana-worker.cjs'));
  try {
    const [ready] = await once(worker, 'message');
    assert.equal(ready.type, 'ready');
    const text = '😀日本語の勉強。東京、東京。ひらがな。';
    worker.postMessage({id: 'page:1', text});
    const [result] = await once(worker, 'message');
    assert.equal(result.type, 'tokens');
    assert.equal(result.id, 'page:1');
    assert.ok(result.tokens.some((t) => t.surface === '日本語' && t.reading === 'にほんご' && t.start === 2));
    assert.equal(result.tokens.filter((t) => t.surface === '東京').length, 2);
    for (const token of result.tokens) assert.equal(text.slice(token.start, token.end), token.surface);
    assert.ok(!result.tokens.some((t) => t.surface === 'ひらがな'));
    console.log('PASS offline readings, repeated words, UTF-16 offsets after emoji');
  } finally { await worker.terminate(); }
  const temporary = fs.mkdtempSync(path.join(os.tmpdir(), 'gogonippon-zip-'));
  try {
    const zip = path.join(temporary, 'diagnostics.zip');
    writeZip(zip, [['events.jsonl', '{"text":"日本語"}\n'], ['code.bin', Buffer.from([0, 255, 128, 10])]]);
    execFileSync('python3', ['-c', "import zipfile,sys; z=zipfile.ZipFile(sys.argv[1]); assert z.testzip() is None; assert z.read('events.jsonl').decode() == '{\"text\":\"日本語\"}\\n'; assert z.read('code.bin') == bytes([0,255,128,10])", zip]);
    console.log('PASS diagnostic ZIP CRC and binary/Unicode contents');
  } finally { fs.rmSync(temporary, {recursive: true, force: true}); }
})().catch((error) => { console.error(error); process.exitCode = 1; });
