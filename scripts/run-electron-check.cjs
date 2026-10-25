const {spawnSync} = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'gogonippon-ui-'));
let failed = false;
for (const restore of ['0', '1']) {
  const result = spawnSync(require('electron'), [path.join(__dirname, 'check-electron.cjs'), ...process.argv.slice(2)], {
    stdio: 'inherit', env: {...process.env, STUDY_CHECK_PROFILE: profile, STUDY_CHECK_RESTORE: restore}
  });
  if (result.error || result.status !== 0) { console.error(result.error || `Electron check exited ${result.status}`); failed = true; break; }
}
if (failed) { console.error('Diagnostic test profile:', profile); process.exitCode = 1; }
else { fs.rmSync(profile, {recursive: true, force: true}); }
