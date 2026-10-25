const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const {once} = require('node:events');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const {stopHelper} = require('../src/process-control.cjs');
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
async function until(callback, timeout = 20000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) { const value = callback(); if (value) return value; await sleep(50); }
  throw new Error('Timed out');
}
async function exited(child, timeout) {
  if (child.exitCode !== null) return child.exitCode;
  let timer;
  try { return await Promise.race([once(child, 'exit').then(([code]) => code), new Promise((_, reject) => { timer = setTimeout(() => reject(new Error(`PID ${child.pid} did not exit`)), timeout); })]); }
  finally { clearTimeout(timer); }
}
function alive(pid) {
  try {
    if (process.platform === 'linux' && /\) Z /.test(fs.readFileSync(`/proc/${pid}/stat`, 'utf8'))) return false;
    process.kill(pid, 0); return true;
  } catch { return false; }
}
(async () => {
  for (const cooperative of [true, false]) {
    const setup = "const game = require('child_process').spawn(process.execPath, ['-e', 'setInterval(() => {}, 1000)'], {stdio:'ignore'}); console.log(JSON.stringify({gamePid:game.pid})); ";
    const child = spawn(process.execPath, ['-e', setup + (cooperative ?
      "process.stdin.on('data', b => { if (b.toString().includes('quit')) process.exit(0); });" :
      "process.stdin.resume(); setInterval(() => {}, 1000);")], {stdio: ['pipe', 'pipe', 'pipe']});
    let gamePid;
    try {
      child.stdin.on('error', () => {});
      const [bytes] = await once(child.stdout, 'data'); gamePid = JSON.parse(bytes).gamePid;
      await stopHelper(child, 100, 500);
      await until(() => !alive(child.pid), 1500);
      assert.ok(alive(gamePid), 'stopping the helper must leave its game process running');
    } finally {
      if (gamePid && alive(gamePid)) process.kill(gamePid, 'SIGKILL');
      if (alive(child.pid)) child.kill('SIGKILL');
    }
  }
  console.log('PASS helper graceful stop and bounded forced termination leave the game running');
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), 'gogonippon-close-'));
  let primary, second, pids = [];
  try {
    const entry = path.join(__dirname, 'check-close-electron.cjs');
    const args = [entry, ...process.argv.slice(2)];
    const env = {...process.env, STUDY_LIFECYCLE_DIR: directory};
    primary = spawn(require('electron'), args, {env, stdio: 'inherit'});
    const readyPath = path.join(directory, 'ready.json');
    await until(() => fs.existsSync(readyPath));
    const ready = JSON.parse(fs.readFileSync(readyPath)); pids = ready.pids;
    assert.ok(ready.windows >= 3);
    second = spawn(require('electron'), args, {env: {...env, STUDY_LIFECYCLE_SECOND: '1'}, stdio: 'inherit'});
    assert.equal(await exited(second, 5000), 0);
    await until(() => fs.existsSync(path.join(directory, 'second.json')));
    assert.equal(JSON.parse(fs.readFileSync(path.join(directory, 'second.json'))).visible, true);
    console.log('PASS second launcher exits and activates the existing launcher');
    fs.writeFileSync(path.join(directory, 'close'), 'close');
    assert.equal(await exited(primary, 8000), 0);
    await until(() => pids.every((pid) => !alive(pid)), 5000);
    console.log(`PASS closing launcher with hidden overlay/settings exits all ${pids.length} recorded application processes`);
  } finally {
    for (const child of [primary, second]) if (child && child.exitCode === null) child.kill('SIGKILL');
    for (const pid of pids) if (alive(pid)) { try { process.kill(pid, 'SIGKILL'); } catch {} }
    fs.rmSync(directory, {recursive: true, force: true});
  }
})().catch((error) => { console.error(error.stack); process.exitCode = 1; });
