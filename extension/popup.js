// extension/popup.js
//
// The toolbar popup UI: a thin view over sw.js's status. Sends start/stop
// commands and getStatus requests to sw.js, and re-renders on every
// "ext-status" broadcast; holds no capture logic itself. Opens/closes with
// the popup window, so it always re-syncs via getStatus on open rather than
// relying on having received every broadcast while it was closed.
let running = false;

function render(st) {
  if (!st) return;
  running = st.capture === "running" || st.capture === "starting";
  document.getElementById("toggle").textContent = running ? "Stop capture" : "Start capture";
  const set = (id, text, cls) => {
    const el = document.getElementById(id);
    el.textContent = text;
    el.className = cls;
  };
  set("capture", st.capture, st.capture === "running" ? "ok" : "dim");
  set("ws", st.ws, st.ws === "connected" ? "ok" : "bad");
  set("engine", st.desktop ? `${st.desktop.engine} (${st.desktop.provider})` : "—", "dim");
  set("streams", st.desktop ? `${st.desktop.streams.mic} / ${st.desktop.streams.tab}` : "— / —", "dim");
  document.getElementById("error").textContent = st.error || "";
}

document.getElementById("toggle").addEventListener("click", async () => {
  await chrome.runtime.sendMessage({ cmd: running ? "stop" : "start" });
});

chrome.runtime.onMessage.addListener((msg) => {
  if (msg.type === "ext-status") render(msg);
});

chrome.runtime.sendMessage({ cmd: "getStatus" }).then(render).catch(() => {});
