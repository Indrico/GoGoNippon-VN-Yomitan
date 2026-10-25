const {parentPort} = require('node:worker_threads');
const path = require('node:path');
const kuromoji = require('kuromoji');
const dictionaryPath = path.join(path.dirname(require.resolve('kuromoji/package.json')).replace('app.asar', 'app.asar.unpacked'), 'dict');
kuromoji.builder({dicPath: dictionaryPath}).build((error, tokenizer) => {
  if (error) { parentPort.postMessage({type: 'error', message: error.message}); return; }
  parentPort.on('message', ({id, text}) => {
    try {
      // Surface matching is deliberate: kuromoji's positions count Unicode code
      // points in some paths; engine/browser offsets are always UTF-16.
      let cursor = 0;
      const tokens = [];
      for (const token of tokenizer.tokenize(text)) {
        const start = text.indexOf(token.surface_form, cursor);
        if (start < 0) continue;
        cursor = start + token.surface_form.length;
        if (!/[\p{Script=Han}々〆ヶ]/u.test(token.surface_form) || !token.reading || token.reading === '*') continue;
        const reading = token.reading.replace(/[ァ-ヶ]/g, (c) => String.fromCharCode(c.charCodeAt(0) - 0x60));
        tokens.push({start, end: cursor, surface: token.surface_form, reading});
      }
      parentPort.postMessage({type: 'tokens', id, tokens});
    } catch (error) { parentPort.postMessage({type: 'error', id, message: error.message}); }
  });
  parentPort.postMessage({type: 'ready'});
});
