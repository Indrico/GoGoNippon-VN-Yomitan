const {app, BrowserWindow} = require('electron');
const fs = require('node:fs');
const path = require('node:path');
const directory = process.env.STUDY_LIFECYCLE_DIR;
app.setPath('userData', path.join(directory, 'profile'));
require('../src/main.cjs');
if (process.env.STUDY_LIFECYCLE_SECOND === '1') return;
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
async function until(callback) {
  for (let i = 0; i < 200; i++) { const value = await callback(); if (value) return value; await sleep(100); }
  throw new Error('Lifecycle setup timed out');
}
app.whenReady().then(async () => {
  const launcher = await until(() => BrowserWindow.getAllWindows().find((w) => w.webContents.getURL().endsWith('launcher.html')));
  await until(() => launcher.webContents.executeJavaScript("document.querySelector('#status').textContent.includes('Ready.')"));
  await launcher.webContents.executeJavaScript("window.study.action('demo')");
  const overlay = await until(() => BrowserWindow.getAllWindows().find((w) => w.webContents.getURL().endsWith('overlay.html')));
  await until(() => overlay.webContents.executeJavaScript("document.documentElement.dataset.yomitanReady === 'true'"));
  await launcher.webContents.executeJavaScript("window.study.action('settings')");
  const settings = await until(() => BrowserWindow.getAllWindows().find((w) => w.webContents.getURL().includes('settings.html')));
  overlay.hide(); settings.hide(); launcher.hide();
  app.once('second-instance', () => fs.writeFileSync(path.join(directory, 'second.json'), JSON.stringify({visible: launcher.isVisible()})));
  fs.writeFileSync(path.join(directory, 'ready.json'), JSON.stringify({
    pids: [...new Set([process.pid, ...app.getAppMetrics().map((entry) => entry.pid)])], windows: BrowserWindow.getAllWindows().length
  }));
  await until(() => fs.existsSync(path.join(directory, 'close')));
  launcher.close(); // Exercise the actual X-button path; do not call app.exit.
}).catch((error) => { console.error(error); app.exit(1); });
