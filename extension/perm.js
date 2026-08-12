// extension/perm.js
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
