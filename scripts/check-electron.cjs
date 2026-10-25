// Run with Electron under Xvfb. All settings and fixtures use a throwaway profile.
const {app, BrowserWindow} = require('electron');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {writeZip} = require('../src/zip.cjs');
const profile = process.env.STUDY_CHECK_PROFILE || fs.mkdtempSync(path.join(os.tmpdir(), 'gogonippon-ui-'));
const restore = process.env.STUDY_CHECK_RESTORE === '1';
app.setPath('userData', profile);
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
async function until(callback, label, timeout = 30000) {
  const end = Date.now() + timeout;
  let value;
  while (Date.now() < end) { value = await callback(); if (value) return value; await sleep(100); }
  throw new Error(`Timeout: ${label}`);
}
app.on('web-contents-created', (_event, contents) => {
  contents.debugger.attach('1.3');
  contents.debugger.sendCommand('Runtime.enable').catch(() => {});
  contents.debugger.on('message', (_event, method, params) => {
    if (method === 'Runtime.consoleAPICalled' && params.type === 'error') {
      for (const arg of params.args || []) if (arg.description) console.log('CONSOLE-STACK', arg.description);
    }
    if (method === 'Runtime.exceptionThrown') console.log('EXCEPTION', JSON.stringify(params.exceptionDetails));
  });
  contents.on('console-message', (_event, ...args) => {
    const details = args[0];
    const message = typeof details === 'object' ? details.message : args[1];
    if (/error|failed|exception|undefined/i.test(message || '')) console.log('BROWSER', message);
  });
});
require('../src/main.cjs');
app.whenReady().then(async () => {
  try {
    const launcher = await until(() => BrowserWindow.getAllWindows().find((w) => w.webContents.getURL().endsWith('launcher.html')), 'launcher');
    await until(() => launcher.webContents.executeJavaScript("document.querySelector('#status').textContent.includes('Ready.')"), 'Yomitan load');
    await launcher.webContents.executeJavaScript("document.querySelector('[data-action=demo]').click()");
    const overlay = await until(() => BrowserWindow.getAllWindows().find((w) => w.webContents.getURL().endsWith('overlay.html')), 'overlay');
    await until(() => overlay.webContents.executeJavaScript("document.documentElement.dataset.yomitanReady === 'true'"), 'Yomitan content frontend');
    await until(() => overlay.webContents.executeJavaScript("document.querySelectorAll('.glyph').length > 20"), 'demo glyphs');
    await sleep(1800);
    overlay.webContents.send('study:event', {type: 'pointer', active: true, x: 84, y: 551, shift: false});
    await until(() => overlay.webContents.executeJavaScript("!document.querySelector('#furigana').hidden && document.querySelector('#furigana').textContent === 'にほんご'"), 'hover furigana');
    console.log('PASS Electron startup, Yomitan content frontend, hover furigana');
    fs.mkdirSync(path.join(__dirname, '../.cache'), {recursive: true});
    fs.writeFileSync(path.join(__dirname, '../.cache/furigana-preview.png'), (await overlay.webContents.capturePage()).toPNG());

    if (!restore) {
    const fixturePath = path.join(profile, 'fixture-dictionary.zip');
    writeZip(fixturePath, [
      ['index.json', JSON.stringify({title: 'GoGoNippon Test Dictionary', revision: '1', format: 3, sequenced: true})],
      ['term_bank_1.json', JSON.stringify([
        ['日本語', 'にほんご', '', '', 1, ['Japanese language — integration fixture'], 1, ''],
        ['東京', 'とうきょう', '', '', 1, ['Tokyo — diagnostic replay fixture'], 2, '']
      ])]
    ]);
    await launcher.webContents.executeJavaScript("document.querySelector('[data-action=settings]').click()");
    const settings = await until(() => BrowserWindow.getAllWindows().find((w) => /chrome-extension:.*settings.html/.test(w.webContents.getURL())), 'settings');
    await until(() => settings.webContents.executeJavaScript("!!document.querySelector('#dictionary-import-file-input')"), 'dictionary importer');
    if (!settings.webContents.debugger.isAttached()) settings.webContents.debugger.attach('1.3');
    const {root} = await settings.webContents.debugger.sendCommand('DOM.getDocument');
    const {nodeId} = await settings.webContents.debugger.sendCommand('DOM.querySelector', {nodeId: root.nodeId, selector: '#dictionary-import-file-input'});
    await settings.webContents.debugger.sendCommand('DOM.setFileInputFiles', {nodeId, files: [fixturePath]});
    await until(() => settings.webContents.executeJavaScript(`new Promise((resolve) => chrome.runtime.sendMessage({action:'getDictionaryInfo', params:{}}, (r) => resolve(r && r.result && r.result.some((d) => d.title === 'GoGoNippon Test Dictionary'))))`), 'dictionary import', 45000);
    console.log('PASS real Yomitan dictionary ZIP import');
    settings.close(); overlay.show();
    }
    overlay.webContents.send('study:event', {type: 'pointer', active: true, x: 84, y: 551, shift: true});
    await until(() => overlay.webContents.executeJavaScript("[...document.querySelectorAll('iframe.yomitan-popup')].some((f) => getComputedStyle(f).visibility === 'visible')"), 'dictionary popup');
    const dictionaryFrame = await until(() => overlay.webContents.mainFrame.frames.find((f) => /popup.html/.test(f.url)), 'popup frame');
    await until(() => dictionaryFrame.executeJavaScript("document.body.textContent.includes('Japanese language — integration fixture')"), 'correct dictionary entry');
    console.log('PASS Shift lookup uses imported dictionary and real Yomitan popup');
    if (restore) console.log('PASS imported dictionary and extension identity survive application restart');
    overlay.webContents.send('study:event', {type: 'pointer', active: true, x: 84, y: 551, shift: false});
    await sleep(200);
    assert.ok(await overlay.webContents.executeJavaScript("document.querySelector('#furigana').hidden"));
    await fs.promises.mkdir(path.join(__dirname, '../.cache'), {recursive: true});
    fs.writeFileSync(path.join(__dirname, '../.cache/overlay-preview.png'), (await overlay.webContents.capturePage()).toPNG());
    overlay.webContents.send('study:event', {type: 'clear'});
    await until(() => overlay.webContents.executeJavaScript("document.querySelectorAll('.glyph').length === 0 && ![...document.querySelectorAll('iframe.yomitan-popup')].some((f) => getComputedStyle(f).visibility === 'visible')"), 'page clear');
    console.log('PASS page clear removes stale text and popup');

    const progressive = {type: 'dialogue', page: 500, sequence: 10001, width: 1024, height: 768,
      text: '日本語の勉強を始めましょう。', glyphs: [0, 1].map((i) => ({start: i, end: i + 1, x: 68 + i * 32, y: 535, width: 32, height: 32}))};
    overlay.webContents.send('study:event', progressive);
    overlay.webContents.send('study:event', {type: 'pointer', active: true, x: 84, y: 551, shift: false});
    overlay.webContents.send('study:event', {type: 'tokens', id: '500:10001', tokens: [{start: 0, end: 3, reading: 'にほんご'}]});
    await until(() => overlay.webContents.executeJavaScript("!document.querySelector('#furigana').hidden"), 'progressive reading');
    progressive.sequence++;
    progressive.glyphs.push({start: 2, end: 3, x: 132, y: 535, width: 32, height: 32});
    overlay.webContents.send('study:event', progressive);
    overlay.webContents.send('study:event', {type: 'tokens', id: '500:10002', tokens: [{start: 0, end: 3, reading: 'にほんご'}]});
    await until(() => overlay.webContents.executeJavaScript("!document.querySelector('#furigana').hidden && document.querySelector('#furigana').textContent === 'にほんご'"), 'stationary cursor across reveal updates');
    assert.ok(await overlay.webContents.executeJavaScript("getComputedStyle(document.querySelector('#glyphs')).color === 'rgba(0, 0, 0, 0)'"));
    console.log('PASS progressive reveal keeps stationary hover and unrevealed text transparent');
    await overlay.webContents.executeJavaScript("window.studyAnchor = document.querySelector('.glyph'); window.studyCloseCount = 0; window.addEventListener('gogonippon-close', () => { window.studyCloseCount++; });");
    for (let index = 3; index < 6; index++) {
      progressive.sequence++;
      progressive.glyphs.push({start: index, end: index + 1, x: 68 + index * 32, y: 535, width: 32, height: 32});
      overlay.webContents.send('study:event', progressive);
      await sleep(40);
      assert.ok(await overlay.webContents.executeJavaScript("!document.querySelector('#furigana').hidden && document.querySelector('.glyph') === window.studyAnchor"), 'reveal update must preserve reading and its DOM anchor');
    }
    overlay.webContents.send('study:event', {type: 'pointer', active: true, x: 84, y: 551, shift: true});
    await until(() => overlay.webContents.executeJavaScript("[...document.querySelectorAll('iframe.yomitan-popup')].some((f) => getComputedStyle(f).visibility === 'visible')"), 'popup before progressive updates');
    await overlay.webContents.executeJavaScript('window.studyCloseCount = 0;');
    for (let index = 6; index < progressive.text.length; index++) {
      progressive.sequence++;
      progressive.glyphs.push({start: index, end: index + 1, x: 68 + index * 32, y: 535, width: 32, height: 32});
      overlay.webContents.send('study:event', progressive);
      await sleep(40);
      assert.ok(await overlay.webContents.executeJavaScript("window.studyCloseCount === 0 && document.querySelector('.glyph') === window.studyAnchor && [...document.querySelectorAll('iframe.yomitan-popup')].some((f) => getComputedStyle(f).visibility === 'visible')"), 'reveal update must leave Yomitan open without replacing the source node');
    }
    await sleep(500);
    assert.ok(await overlay.webContents.executeJavaScript('window.studyCloseCount === 0'));
    console.log('PASS furigana and Yomitan stay visible throughout reveal updates and after completion');

    if (process.env.STUDY_REPLAY_DIALOGUE) {
      const replay = JSON.parse(fs.readFileSync(process.env.STUDY_REPLAY_DIALOGUE, 'utf8'));
      replay.sequence = 20000;
      overlay.webContents.send('study:event', replay);
      const start = replay.text.indexOf('東京');
      assert.ok(start >= 0);
      const glyph = replay.glyphs.find((g) => g.start === start);
      assert.ok(glyph);
      const [width, height] = await overlay.webContents.executeJavaScript('[innerWidth,innerHeight]');
      const scale = Math.min(width / replay.width, height / replay.height);
      const x = (width - replay.width * scale) / 2 + (glyph.x + glyph.width / 2) * scale;
      const y = (height - replay.height * scale) / 2 + (glyph.y + glyph.height / 2) * scale;
      overlay.webContents.send('study:event', {type: 'pointer', active: true, x, y, shift: false});
      overlay.webContents.send('study:event', {type: 'tokens', id: `${replay.page}:20000`, tokens: [{start, end: start + 2, reading: 'とうきょう'}]});
      await until(() => overlay.webContents.executeJavaScript("!document.querySelector('#furigana').hidden && document.querySelector('#furigana').textContent === 'とうきょう'"), 'real dialogue coordinates and reading');
      overlay.webContents.send('study:event', {type: 'pointer', active: true, x, y, shift: true});
      await until(() => dictionaryFrame.executeJavaScript("document.body.textContent.includes('Tokyo — diagnostic replay fixture')"), 'real dialogue Yomitan lookup');
      console.log('PASS diagnostic Japanese text and engine coordinates produce Tokyo reading and Yomitan entry');
    }
    app.exit(0);
  } catch (error) {
    console.error(error.stack);
    for (const w of BrowserWindow.getAllWindows()) {
      console.log('WINDOW', w.webContents.getURL());
      try { console.log('FRAMES', await w.webContents.executeJavaScript("[...document.querySelectorAll('iframe')].map(f => ({src:f.src, style:f.getAttribute('style')}))")); } catch {}
      for (const f of w.webContents.mainFrame.frames) {
        console.log('FRAME', f.url);
        try { console.log('FRAME-STATE', await f.executeJavaScript("JSON.stringify({ready:document.readyState, chrome:typeof chrome.runtime, body:document.body.innerText.slice(0,500)})")); } catch {}
      }
    }
    console.log('PROFILE', profile); app.exit(1);
  }
});
