// Stop only the helper we spawned. BGI.exe and Steam are independent processes
// and must keep running when the study application is closed.
function stopHelper(child, graceMs = 1500, killMs = 500) {
  if (!child || child.exitCode !== null || child.signalCode !== null) return Promise.resolve();
  return new Promise((resolve) => {
    let forceTimer, finishTimer;
    const finish = () => {
      clearTimeout(forceTimer); clearTimeout(finishTimer);
      child.removeListener('close', finish); child.removeListener('error', finish);
      resolve();
    };
    child.once('close', finish); child.once('error', finish);
    forceTimer = setTimeout(() => {
      try { child.kill('SIGKILL'); } catch {}
      finishTimer = setTimeout(finish, killMs);
    }, graceMs);
    try {
      if (child.stdin && !child.stdin.destroyed && !child.stdin.writableEnded) child.stdin.end('quit\n');
    } catch {}
  });
}
module.exports = {stopHelper};
