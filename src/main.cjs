const {app, BrowserWindow, ipcMain, dialog, session, screen, shell, protocol} = require('electron');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {spawn} = require('node:child_process');
const readline = require('node:readline');
const {Worker} = require('node:worker_threads');
const {writeZip} = require('./zip.cjs');
const {stopHelper} = require('./process-control.cjs');
if (!app.requestSingleInstanceLock()) { app.quit(); return; }
const EXPECTED_SHA256 = '2cc889b0e60f5663a0adc6c1b3bc598ac0adb3d6b39b7d5eae6391cadd209c88';
protocol.registerSchemesAsPrivileged([{scheme: 'chrome-extension', privileges: {
  standard: true, secure: true, supportFetchAPI: true, corsEnabled: true
}}]);
const root = path.resolve(__dirname, '..');
const resource = (...parts) => path.join(app.isPackaged ? process.resourcesPath : root, ...parts);
let launcher, overlay, settingsWindow, helper, worker, extension, overlaySession;
let gamePath = '', sessionDir = '', lastStatus = null, frame = null, lastPointer = '', lastBounds = '';
let currentDialogue = null, tokenizerReady = false, demoMode = false, shuttingDown = false;
let popupRects = [], engineGlyphs = [], page = 1, sequence = 0, glyphTimer;
let tokenizedText = null, tokenizedTokens = [], tokenRequest = null, tokenSerial = 0;
let shutdownReady = false, shutdownPromise = null;
const readyWindows = new Set();
const configPath = () => path.join(app.getPath('userData'), 'study-config.json');
const send = (window, message) => { if (window && !window.isDestroyed() && readyWindows.has(window.id)) window.webContents.send('study:event', message); };
function log(message) {
  if (!sessionDir || message.type === 'frame') return;
  const filename = path.join(sessionDir, 'events.jsonl');
  if (fs.existsSync(filename) && fs.statSync(filename).size > 32 * 1024 * 1024) return;
  fs.appendFileSync(filename, JSON.stringify({time: new Date().toISOString(), ...message}) + '\n');
}
function status(message, detail = '') {
  lastStatus = {type: 'status', message, detail};
  send(launcher, lastStatus); log(lastStatus);
}
function newSession() {
  sessionDir = path.join(app.getPath('userData'), 'diagnostics', new Date().toISOString().replace(/[:.]/g, '-'));
  fs.mkdirSync(sessionDir, {recursive: true});
  fs.writeFileSync(path.join(sessionDir, 'session.json'), JSON.stringify({
    version: app.getVersion(), platform: process.platform, arch: process.arch,
    electron: process.versions.electron, gamePath, expectedGameSha256: EXPECTED_SHA256,
    mode: demoMode ? 'demo' : 'game', engineAdapter: 'bgi-1.528-v4'
  }, null, 2));
}
function saveConfig() { fs.writeFileSync(configPath(), JSON.stringify({gamePath}, null, 2)); }
function requestReadings() {
  if (!tokenizerReady || !currentDialogue) return;
  const {text, page, sequence} = currentDialogue;
  if (text === tokenizedText) send(overlay, {type: 'tokens', id: `${page}:${sequence}`, tokens: tokenizedTokens});
  else if (tokenRequest?.text !== text) {
    tokenRequest = {id: ++tokenSerial, text};
    worker.postMessage(tokenRequest);
  }
}
function setDialogue(message) {
  currentDialogue = message;
  send(overlay, message);
  log(message);
  requestReadings();
}
function clearDialogue() {
  clearTimeout(glyphTimer); engineGlyphs = []; currentDialogue = null;
  popupRects = []; lastPointer = '';
  send(overlay, {type: 'clear'});
}
function ensureOverlay(demo = false) {
  if (overlay && !overlay.isDestroyed()) overlay.close();
  demoMode = demo; readyWindows.clear();
  // The launcher may already be loaded; preserve its ready state.
  if (launcher && !launcher.isDestroyed()) readyWindows.add(launcher.id);
  overlay = new BrowserWindow({
    width: 1024, height: 768, show: false, frame: false, transparent: true,
    resizable: demo, alwaysOnTop: true, skipTaskbar: !demo, focusable: demo,
    backgroundColor: '#00000000', title: demo ? 'GoGoNippon Study Demo' : 'GoGoNippon Study Overlay',
    // The bundled Yomitan iframe needs Electron subframe initialization. This
    // window hosts only local content; top-level navigation is locked below.
    webPreferences: {preload: path.join(__dirname, 'preload.cjs'), contextIsolation: true,
      nodeIntegration: false, nodeIntegrationInSubFrames: true, sandbox: false, webSecurity: false, session: overlaySession, backgroundThrottling: false}
  });
  overlay.setMenuBarVisibility(false);
  overlay.webContents.on('will-navigate', (event) => event.preventDefault());
  overlay.setIgnoreMouseEvents(!demo, {forward: true});
  overlay.webContents.setWindowOpenHandler(({url}) => {
    if (/^https?:\/\//i.test(url)) void shell.openExternal(url);
    return {action: 'deny'};
  });
  const createdOverlay = overlay;
  overlay.on('closed', () => { if (overlay === createdOverlay) { overlay = null; popupRects = []; } });
  overlay.webContents.on('console-message', (_event, ...args) => {
    const entry = args.find((x) => typeof x === 'string') || String(args[0]?.message || '');
    log({type: 'renderer-log', message: entry.slice(0, 4096)});
  });
  overlay.loadFile(path.join(__dirname, 'overlay.html'));
}
function startWorker() {
  worker = new Worker(path.join(__dirname, 'furigana-worker.cjs'));
  worker.on('message', (message) => {
    if (message.type === 'ready') {
      tokenizerReady = true;
      requestReadings();
    } else if (message.type === 'tokens' && message.id === tokenRequest?.id) {
      tokenizedText = tokenRequest.text; tokenizedTokens = message.tokens; tokenRequest = null;
      requestReadings();
    }
    else if (message.type === 'error') status('Furigana could not be processed.', message.message);
  });
  worker.on('error', (error) => { tokenizerReady = false; status('Furigana processing failed.', error.message); });
}
async function loadYomitan() {
  overlaySession = session.fromPartition('persist:gogonippon-yomitan');
  const extPath = resource(app.isPackaged ? 'yomitan' : 'vendor/yomitan');
  const sourcePath = path.join(extPath, 'GOGONIPPON-SOURCE.json');
  if (!fs.existsSync(sourcePath)) throw new Error('Yomitan is missing. Run npm run vendor before building.');
  const source = fs.readFileSync(sourcePath, 'utf8');
  const revisionPath = path.join(app.getPath('userData'), 'yomitan-revision.txt');
  const revision = crypto.createHash('sha256').update(source).digest('hex');
  if (!fs.existsSync(revisionPath) || fs.readFileSync(revisionPath, 'utf8') !== revision) {
    // Invalidate only workers, keeping dictionaries in IndexedDB intact.
    await overlaySession.clearStorageData({storages: ['serviceworkers']});
  }
  const api = overlaySession.extensions || overlaySession;
  extension = await api.loadExtension(extPath, {allowFileAccess: true});
  fs.writeFileSync(revisionPath, revision);
}
function updateFrame(message) {
  frame = message;
  if (!overlay || overlay.isDestroyed() || demoMode) return;
  const usable = message.visible && message.width > 0 && message.height > 0;
  const active = usable && message.active;
  if (!active) {
    overlay.hide(); popupRects = [];
    send(overlay, {type: 'pointer', active: false, x: 0, y: 0});
    lastPointer = ''; return;
  }
  const physical = {x: message.left, y: message.top, width: message.width, height: message.height};
  const bounds = screen.screenToDipRect(overlay, physical);
  const boundsKey = JSON.stringify(bounds);
  if (boundsKey !== lastBounds) { overlay.setBounds(bounds); lastBounds = boundsKey; popupRects = []; lastPointer = ''; }
  if (!overlay.isVisible()) overlay.showInactive();
  // Ratios use the same client extent as glyph placement, avoiding mixed-DPI
  // screen-point ambiguities while dragging a game between monitors.
  const x = (message.cursorX - message.left) * bounds.width / message.width;
  const y = (message.cursorY - message.top) * bounds.height / message.height;
  const inPopup = popupRects.some((r) => x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height);
  overlay.setIgnoreMouseEvents(!inPopup, {forward: true});
  const pointer = {type: 'pointer', x, y, active: true, shift: message.shift, escape: message.escape};
  const key = JSON.stringify(pointer);
  if (key !== lastPointer) { send(overlay, pointer); lastPointer = key; }
}
function onNative(message) {
  if (message.type === 'frame') { updateFrame(message); return; }
  log(message);
  if (message.type === 'status') status(message.message, message.detail || '');
  else if (message.type === 'game-connected') status('Game window found. Starting the engine adapter…', message.path || '');
  else if (message.type === 'clear') { page++; clearDialogue(); }
  else if (message.type === 'dialogue') {
    // The native adapter sends a complete snapshot; sequence is session-local
    // so reconnects and demo mode cannot reuse an old renderer revision.
    setDialogue({...message, sequence: ++sequence});
    lastPointer = '';
  }
  else if (message.type === 'glyph') {
    const old = engineGlyphs.findIndex((g) => g.x === message.x && g.y === message.y);
    if (old >= 0) engineGlyphs[old] = message; else engineGlyphs.push(message);
    clearTimeout(glyphTimer);
    glyphTimer = setTimeout(() => {
      const glyphs = [], parts = [];
      let index = 0;
      for (const g of engineGlyphs) {
        parts.push(g.text);
        glyphs.push({start: index, end: index + g.text.length, x: g.x, y: g.y, width: g.width, height: g.height});
        index += g.text.length;
      }
      setDialogue({type: 'dialogue', page, sequence: ++sequence, text: parts.join(''),
        width: message.canvasWidth, height: message.canvasHeight, fit: message.fit, glyphs});
    }, 35);
  }
}
async function startGame() {
  if (process.platform !== 'win32') throw new Error('Use the Windows release to connect to the game. Try Demo also works on Linux.');
  if (helper) {
    status('Integration is already running.', 'Waiting for or connected to the game. Use Disconnect to end this attempt.');
    return;
  }
  if (!gamePath || !fs.existsSync(gamePath)) throw new Error('Choose BGI.exe first.');
  const sha256 = crypto.createHash('sha256').update(fs.readFileSync(gamePath)).digest('hex');
  if (sha256 !== EXPECTED_SHA256) throw new Error(`Unsupported game executable (${sha256}). See the supported edition in README.md.`);
  clearDialogue(); ensureOverlay(false); newSession(); lastBounds = ''; lastPointer = '';
  const helperPath = resource(app.isPackaged ? 'native/study-helper.exe' : 'native/bin/study-helper.exe');
  const dllPath = resource(app.isPackaged ? 'native/study-hook.dll' : 'native/bin/study-hook.dll');
  if (!fs.existsSync(helperPath) || !fs.existsSync(dllPath)) throw new Error('The native helper has not been built.');
  const profile = resource('profiles/bgi-1.528.ini');
  helper = spawn(helperPath, ['--game', gamePath, '--dll', dllPath, '--session', sessionDir, '--profile', profile], {windowsHide: true, stdio: ['pipe', 'pipe', 'pipe']});
  const running = helper;
  let nativeStatus = null, spawnError = null, receivedOutput = false;
  const responseTimer = setTimeout(() => {
    if (!receivedOutput && helper === running && !shuttingDown) status('The helper has not responded yet.', `PID ${running.pid || '?'}; use Disconnect, then Export Diagnostics.`);
  }, 10000);
  running.on('spawn', () => {
    log({type: 'helper-started', pid: running.pid, executable: helperPath});
    if (!shuttingDown) status('Helper started; looking for the game…', gamePath);
  });
  const lines = readline.createInterface({input: running.stdout});
  lines.on('line', (line) => {
    if (line.length > 1024 * 1024) return;
    try {
      receivedOutput = true; clearTimeout(responseTimer);
      const message = JSON.parse(line);
      if (message.type === 'status') nativeStatus = message;
      if (!shuttingDown) onNative(message);
    } catch (error) { log({type: 'parse-error', message: error.message, line: line.slice(0, 200)}); }
  });
  running.stdin.on('error', (error) => { if (!shuttingDown) log({type: 'helper-input-error', message: error.message}); });
  running.stderr.on('data', (bytes) => log({type: 'native-stderr', message: bytes.toString().slice(0, 4096)}));
  running.on('error', (error) => { spawnError = error; if (!shuttingDown) status('The helper could not be started.', error.message); });
  running.on('close', (code) => {
    clearTimeout(responseTimer); lines.close();
    if (helper === running) { helper = null; clearDialogue(); if (overlay) overlay.hide(); }
    if (shuttingDown) return;
    if (spawnError) status('The helper could not be started.', spawnError.message);
    else if (code && nativeStatus) status(nativeStatus.message, `${nativeStatus.detail || ''} (helper code ${code})`);
    else status('Integration stopped.', `Helper code: ${code}. The game can still be played. Diagnostics are available to export.`);
  });
  status('Connecting to the BGI engine…', 'Looking for a running game or launching it through Steam.');
}
async function stop() {
  clearDialogue();
  if (overlay) overlay.close();
  await stopHelper(helper);
}
function shutdown() {
  if (shuttingDown) return shutdownPromise;
  shuttingDown = true;
  // A hidden overlay, settings window, extension renderer or worker must never
  // keep the application alive after the user closes its launcher.
  const forceExit = setTimeout(() => app.exit(0), 6000);
  forceExit.unref();
  shutdownPromise = (async () => {
    clearDialogue();
    for (const window of BrowserWindow.getAllWindows()) window.destroy();
    try { overlaySession?.flushStorageData(); } catch {}
    await Promise.allSettled([stopHelper(helper), worker?.terminate(), overlaySession?.cookies.flushStore()]);
    shutdownReady = true;
    app.quit();
  })();
  return shutdownPromise;
}
async function exportDiagnostics() {
  if (!sessionDir) throw new Error('Launch the game or demo first to create a diagnostic session.');
  if (helper) {
    helper.stdin.write('snapshot\n');
    await new Promise((resolve) => setTimeout(resolve, 700));
  }
  const result = await dialog.showSaveDialog(launcher, {title: 'Export Diagnostics', defaultPath: path.join(app.getPath('desktop'), 'GoGoNippon-diagnostics.zip'), filters: [{name: 'ZIP', extensions: ['zip']}]});
  if (result.canceled) return;
  const entries = fs.readdirSync(sessionDir).filter((name) => /\.(json|jsonl|bin|ini)$/.test(name))
    .filter((name) => fs.statSync(path.join(sessionDir, name)).isFile())
    .map((name) => [name, fs.readFileSync(path.join(sessionDir, name))]);
  writeZip(result.filePath, entries);
  status('Diagnostics exported.', result.filePath);
}
function startDemo() {
  if (helper) throw new Error('Disconnect before opening the demo.');
  clearDialogue(); ensureOverlay(true); newSession();
  status('Demo active.', 'Sample sentences for trying furigana and Yomitan. These sentences are provided by the demo.');
}
function demoDialogue() {
  const text = '日本語の勉強を始めましょう。\n東京の駅で切符を買いました。';
  let x = 68, y = 535; const glyphs = [];
  let index = 0;
  for (const character of text) {
    if (character === '\n') { x = 68; y += 52; index++; continue; }
    glyphs.push({start: index, end: index + character.length, x, y, width: 32, height: 32});
    x += 32; index += character.length;
  }
  return {type: 'dialogue', page: ++page, sequence: ++sequence, text, glyphs, width: 1024, height: 768};
}
ipcMain.handle('study:action', async (event, action) => {
  if (shuttingDown) throw new Error('The application is closing.');
  if (event.sender !== launcher?.webContents && !(event.sender === overlay?.webContents && action === 'close-demo')) throw new Error('Invalid sender');
  switch (action) {
    case 'choose': {
      const result = await dialog.showOpenDialog(launcher, {title: 'Choose BGI.exe', properties: ['openFile'], filters: [{name: 'BGI.exe', extensions: ['exe']}]});
      if (!result.canceled) { gamePath = result.filePaths[0]; saveConfig(); send(launcher, {type: 'config', gamePath, version: app.getVersion()}); }
      break;
    }
    case 'launch': await startGame(); break;
    case 'stop': await stop(); break;
    case 'demo': startDemo(); break;
    case 'close-demo': if (demoMode && overlay) overlay.close(); break;
    case 'settings':
      if (!extension) throw new Error('Yomitan could not be loaded.');
      if (settingsWindow && !settingsWindow.isDestroyed()) { settingsWindow.focus(); break; }
      settingsWindow = new BrowserWindow({width: 1080, height: 780, title: 'Yomitan Settings', webPreferences: {session: overlaySession, nodeIntegration: false, nodeIntegrationInSubFrames: true, contextIsolation: true, sandbox: false}});
      settingsWindow.setMenuBarVisibility(false);
      settingsWindow.webContents.on('will-navigate', (event, url) => {
        if (!url.startsWith(`chrome-extension://${extension.id}/`)) event.preventDefault();
      });
      settingsWindow.webContents.setWindowOpenHandler(({url}) => {
        if (/^https?:\/\//i.test(url)) void shell.openExternal(url);
        return {action: 'deny'};
      });
      await settingsWindow.loadURL(`chrome-extension://${extension.id}/settings.html`); break;
    case 'snapshot':
      if (!helper) throw new Error('Connect to the game before capturing engine diagnostics.');
      helper.stdin.write('snapshot\n'); status('Capturing engine diagnostics.', 'Advance a few Japanese dialogue pages, then select Export Diagnostics.'); break;
    case 'export': await exportDiagnostics(); break;
    default: throw new Error('Unknown action');
  }
});
ipcMain.on('study:renderer-ready', (event) => {
  const window = BrowserWindow.fromWebContents(event.sender);
  if (window !== launcher && window !== overlay) return;
  readyWindows.add(window.id);
  if (window === launcher) {
    send(launcher, {type: 'config', gamePath, version: app.getVersion()}); if (lastStatus) send(launcher, lastStatus);
  } else if (demoMode) {
    send(overlay, {type: 'demo'}); overlay.show(); setDialogue(demoDialogue());
  } else if (currentDialogue) setDialogue(currentDialogue);
});
ipcMain.on('study:popup', (event, rects) => {
  if (event.sender !== overlay?.webContents || !Array.isArray(rects) || rects.length > 8) return;
  popupRects = rects.filter((r) => [r.x, r.y, r.width, r.height].every(Number.isFinite) && r.width > 0 && r.height > 0);
});
app.whenReady().then(async () => {
  try { gamePath = JSON.parse(fs.readFileSync(configPath())).gamePath || ''; } catch {}
  if (!gamePath) {
    const candidates = [path.resolve(root, '../BGI.exe'), path.join(process.cwd(), 'BGI.exe'), path.join(path.dirname(app.getPath('exe')), 'BGI.exe'), path.resolve(path.dirname(app.getPath('exe')), '../BGI.exe')];
    gamePath = candidates.find((p) => fs.existsSync(p)) || '';
  }
  launcher = new BrowserWindow({width: 800, height: 840, minWidth: 660, minHeight: 640, title: 'GoGoNippon Study', backgroundColor: '#10141e',
    webPreferences: {preload: path.join(__dirname, 'preload.cjs'), nodeIntegration: false, contextIsolation: true, sandbox: true}});
  launcher.setMenuBarVisibility(false);
  launcher.on('close', (event) => {
    if (shutdownReady) return;
    event.preventDefault(); app.quit();
  });
  await launcher.loadFile(path.join(__dirname, 'launcher.html'));
  if (shuttingDown) return;
  startWorker();
  try { await loadYomitan(); status('Ready.', 'Import a dictionary in Yomitan Settings, then connect to the game. Hover over kanji for furigana; Shift + hover for Yomitan.'); }
  catch (error) { status('Yomitan could not be loaded.', error.message); overlaySession ||= session.fromPartition('persist:gogonippon-yomitan'); }
  if (!shuttingDown && process.argv.includes('--demo')) startDemo();
}).catch((error) => { if (!shuttingDown) { dialog.showErrorBox('GoGoNippon Study', error.stack || error.message); app.quit(); } });
app.on('second-instance', () => {
  if (!launcher || launcher.isDestroyed()) return;
  if (launcher.isMinimized()) launcher.restore();
  launcher.show(); launcher.focus();
});
app.on('before-quit', (event) => {
  if (shutdownReady) return;
  event.preventDefault(); void shutdown();
});
app.on('window-all-closed', () => app.quit());
