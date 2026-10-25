// Use electron-builder's own NSIS reader to extract the unsigned uninstaller
// on Linux. The default path executes a temporary Windows stub through Wine.
// This reader is already used upstream on macOS and produces the same PE file.
const path = require('node:path');
const {build, Platform, Arch} = require('electron-builder');
const root = path.resolve(__dirname, '..');
const metadata = require('../package.json');
if (require('app-builder-lib/package.json').version !== '26.18.0') {
  throw new Error('Review the NSIS reader integration before changing electron-builder.');
}
if (process.platform === 'linux') {
  const {WineVmManager} = require('app-builder-lib/out/vm/WineVm');
  const {UninstallerReader} = require('app-builder-lib/out/targets/nsis/nsisUtil');
  const original = WineVmManager.prototype.exec;
  const installer = path.join(root, 'dist', `GoGoNippon-Study-${metadata.version}-Windows-x64-Setup.exe`);
  WineVmManager.prototype.exec = function(file, args, ...rest) {
    if (path.resolve(file) === installer && args.length === 0) {
      console.log('Extracting the NSIS uninstaller without executing the Windows stub.');
      return UninstallerReader.exec(file, `${file.slice(0, -3)}__uninstaller.exe`);
    }
    return original.call(this, file, args, ...rest);
  };
}
build({projectDir: root, targets: Platform.WINDOWS.createTarget(['nsis', '7z'], Arch.x64), publish: 'never'})
  .catch((error) => { console.error(error); process.exitCode = 1; });
