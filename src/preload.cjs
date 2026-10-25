const {contextBridge, ipcRenderer} = require('electron');
if (process.isMainFrame) contextBridge.exposeInMainWorld('study', {
  action: (action) => ipcRenderer.invoke('study:action', action),
  on: (callback) => {
    const listener = (_event, message) => callback(message);
    ipcRenderer.on('study:event', listener);
    return () => ipcRenderer.removeListener('study:event', listener);
  },
  popup: (rects) => ipcRenderer.send('study:popup', rects),
  ready: () => ipcRenderer.send('study:renderer-ready')
});
