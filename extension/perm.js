// extension/perm.js
// extension/perm.js
//
// Standalone permission-prompt page (perm.html), opened by sw.js
// (openPermissionPage) when offscreen.js's getUserMedia fails with
// NotAllowedError. Exists because an offscreen document cannot itself
// surface a browser mic-permission prompt to the user; a real tab can.
document.getElementById("req").addEventListener("click", async () => {
  const out = document.getElementById("result");
  try {
    const s = await navigator.mediaDevices.getUserMedia({ audio: true });
    s.getTracks().forEach((t) => t.stop());
    out.textContent = "Granted! You can close this tab and press Start again.";
  } catch (e) {
    out.textContent = "Denied: " + e.name + " — allow the microphone for this extension in site settings.";
  }
});
