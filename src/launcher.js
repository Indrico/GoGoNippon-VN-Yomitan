for (const button of document.querySelectorAll('button[data-action]')) {
  button.addEventListener('click', async () => {
    button.disabled = true;
    try { await window.study.action(button.dataset.action); }
    catch (error) { document.querySelector('#status').textContent = error.message; }
    finally { button.disabled = false; }
  });
}
window.study.on((message) => {
  if (message.type === 'config') {
    document.querySelector('#game-path').textContent = message.gamePath || 'Choose BGI.exe in your game folder.';
    document.querySelector('#app-version').textContent = message.version ? `Launcher ${message.version}` : '';
  }
  if (message.type === 'status') {
    document.querySelector('#status').textContent = message.message;
    document.querySelector('#detail').textContent = message.detail || '';
  }
});
window.study.ready();
