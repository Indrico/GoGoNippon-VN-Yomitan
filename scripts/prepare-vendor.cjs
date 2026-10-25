const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const {execFileSync} = require('node:child_process');
const root = path.resolve(__dirname, '..');
const archive = path.join(root, '.cache/yomitan-chrome.zip');
const destination = path.join(root, 'vendor/yomitan');
if (!fs.existsSync(archive)) throw new Error('Download Yomitan 26.10.12.0 to .cache/yomitan-chrome.zip first (see README).');
fs.rmSync(destination, {recursive: true, force: true});
fs.mkdirSync(destination, {recursive: true});
execFileSync('python3', ['-c', 'import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])', archive, destination]);
const manifestPath = path.join(destination, 'manifest.json');
const manifest = JSON.parse(fs.readFileSync(manifestPath));
if (manifest.version !== '26.10.12.0') throw new Error('Unexpected Yomitan version');
// A stable extension identity keeps IndexedDB/dictionaries across launches and upgrades.
const keyPath = path.join(root, 'assets/extension-key.txt');
if (!fs.existsSync(keyPath)) {
  const {publicKey} = crypto.generateKeyPairSync('rsa', {modulusLength: 2048});
  fs.writeFileSync(keyPath, publicKey.export({format: 'der', type: 'spki'}).toString('base64') + '\n');
}
manifest.key = fs.readFileSync(keyPath, 'utf8').trim();
fs.writeFileSync(manifestPath, JSON.stringify(manifest, null, 2) + '\n');
const popupPath = path.join(destination, 'js/app/popup.js');
let popup = fs.readFileSync(popupPath, 'utf8');
const anchor = "this._frame.style.setProperty('visibility', value ? 'visible' : 'hidden', 'important');";
if (!popup.includes(anchor)) throw new Error('Yomitan popup visibility integration needs review');
popup = popup.replace(anchor, `${anchor}\n        window.dispatchEvent(new CustomEvent('gogonippon-popup', {detail: {visible: value}}));`);
for (const [field, option] of [['_useSecureFrameUrl', 'useSecurePopupFrameUrl'], ['_useShadowDom', 'usePopupShadowDom']]) {
  const original = `this.${field} = general.${option};`;
  if (!popup.includes(original)) throw new Error(`Yomitan ${option} integration needs review`);
  popup = popup.replace(original, `this.${field} = document.documentElement.dataset.gogonippon === 'overlay' ? false : general.${option};`);
}
fs.writeFileSync(popupPath, popup);
// Invoke the actual Yomitan frontend in its extension world. No synthesized DOM
// mouse events or focus change is necessary while the game owns keyboard focus.
const frontendPath = path.join(destination, 'js/app/frontend.js');
let frontend = fs.readFileSync(frontendPath, 'utf8');
const readyAnchor = 'this._signalFrontendReady(null);';
if (!frontend.includes(readyAnchor)) throw new Error('Yomitan frontend integration needs review');
frontend = frontend.replace(readyAnchor, `${readyAnchor}
        if (location.protocol === 'file:' && document.documentElement.dataset.gogonippon === 'overlay') {
            // Overlay policy is an instance override; imported browser profiles remain intact.
            this._canUseWindowPopup = false;
            window.addEventListener('gogonippon-lookup', (event) => {
                const {x, y} = event.detail || {};
                if (!Number.isFinite(x) || !Number.isFinite(y)) return;
                const source = this._textSourceGenerator.getRangeFromPoint(x, y, {deepContentScan: false});
                if (source !== null) {
                    void this._textScanner.search(source, {focus: false}, false, true);
                }
            });
            window.addEventListener('gogonippon-close', () => this._clearSelection(false));
            // Scanning is driven only by the native Shift gate, including inside popups.
            this._textScanner.setEnabled(false);
            document.documentElement.dataset.yomitanReady = 'true';
            window.dispatchEvent(new CustomEvent('gogonippon-yomitan-ready'));
        }`);
// Prevent future options updates from re-enabling unmodified hover scanning.
const enabledAnchor = 'const enabled = (this._options !== null && this._options.general.enable && !this._disabledOverride);';
if (!frontend.includes(enabledAnchor)) throw new Error('Yomitan scanner enable integration needs review');
frontend = frontend.replace(enabledAnchor, "const enabled = (document.documentElement.dataset.gogonippon !== 'overlay' && this._options !== null && this._options.general.enable && !this._disabledOverride);");
// Avoid creating a separate native popup or selecting invisible glyph nodes.
const optionsAnchor = 'const {usePopupWindow, showIframePopupsInRootFrame} =';
if (!frontend.includes(optionsAnchor)) throw new Error('Yomitan popup options integration needs review');
frontend = frontend.replace(optionsAnchor, 'const {usePopupWindow: configuredPopupWindow, showIframePopupsInRootFrame} =');
const optionsEnd = "const isIframe = !this._useProxyPopup && (window !== window.parent);";
frontend = frontend.replace(optionsEnd, "const usePopupWindow = document.documentElement.dataset.gogonippon !== 'overlay' && configuredPopupWindow;\n        " + optionsEnd);
fs.writeFileSync(frontendPath, frontend);
const scannerPath = path.join(destination, 'js/language/text-scanner.js');
let scanner = fs.readFileSync(scannerPath, 'utf8');
scanner = scanner.replace('this._selectText = selectText;', "this._selectText = document.documentElement.dataset.gogonippon === 'overlay' ? false : selectText;");
scanner = scanner.replace('this._layoutAwareScan = layoutAwareScan;', "this._layoutAwareScan = document.documentElement.dataset.gogonippon === 'overlay' ? false : layoutAwareScan;");
fs.writeFileSync(scannerPath, scanner);
// Electron does not expose chrome.permissions consistently. Report only the
// permissions declared by this hosted extension, never optional capabilities.
const permissionsPath = path.join(destination, 'js/data/permissions-util.js');
let permissions = fs.readFileSync(permissionsPath, 'utf8');
permissions = permissions.replace('export function getAllPermissions() {', `export function getAllPermissions() {
    if (!chrome.permissions || !chrome.permissions.getAll) {
        const manifest = chrome.runtime.getManifest();
        return Promise.resolve({permissions: manifest.permissions || [], origins: manifest.host_permissions || []});
    }`);
permissions = permissions.replace('export function hasPermissions(permissions) {', `export function hasPermissions(permissions) {
    if (!chrome.permissions || !chrome.permissions.contains) {
        return getAllPermissions().then((current) => (permissions.permissions || []).every((p) => current.permissions.includes(p)) && (permissions.origins || []).every((p) => current.origins.includes(p)));
    }`);
permissions = permissions.replace('export function setPermissionsGranted(permissions, shouldHave) {', `export function setPermissionsGranted(permissions, shouldHave) {
    if (!chrome.permissions || !chrome.permissions.request) return shouldHave ? hasPermissions(permissions) : Promise.resolve(false);`);
fs.writeFileSync(permissionsPath, permissions);
const backendPath = path.join(destination, 'js/background/backend.js');
let backend = fs.readFileSync(backendPath, 'utf8');
backend = backend.replace('async _openWelcomeGuidePage() {', 'async _openWelcomeGuidePage() {\n        if (!chrome.tabs.create) return; // The launcher hosts onboarding/settings.');
fs.writeFileSync(backendPath, backend);
fs.writeFileSync(path.join(destination, 'GOGONIPPON-SOURCE.json'), JSON.stringify({
  version: manifest.version,
  source: 'https://github.com/yomidevs/yomitan/releases/tag/26.10.12.0',
  archiveSha256: crypto.createHash('sha256').update(fs.readFileSync(archive)).digest('hex'),
  changes: ['stable extension key', 'popup visibility event', 'native Shift-controlled lookup without focus', 'overlay instance options']
}, null, 2) + '\n');
console.log('Prepared Yomitan', manifest.version);
