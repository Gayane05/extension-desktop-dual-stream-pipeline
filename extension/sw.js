// extension/sw.js
const OFFSCREEN_URL = "offscreen.html";
const DESKTOP_PORT = 8765;

let lastStatus = {
  type: "ext-status", capture: "idle", ws: "disconnected",
  desktop: null, error: null, micPermission: "unknown",
};

function broadcast(patch) {
  lastStatus = { ...lastStatus, ...patch, type: "ext-status" };
  chrome.runtime.sendMessage(lastStatus).catch(() => {});
}

async function hasOffscreen() {
  const contexts = await chrome.runtime.getContexts({ contextTypes: ["OFFSCREEN_DOCUMENT"] });
  return contexts.length > 0;
}

async function start() {
  broadcast({ capture: "starting", error: null });
  try {
    const [tab] = await chrome.tabs.query({ active: true, currentWindow: true });
    if (!tab) throw new Error("no active tab");
    const tabStreamId = await chrome.tabCapture.getMediaStreamId({ targetTabId: tab.id });
    if (!(await hasOffscreen())) {
      await chrome.offscreen.createDocument({
        url: OFFSCREEN_URL,
        reasons: ["USER_MEDIA"],
        justification: "Capture microphone and tab audio for local transcription",
      });
    }
    await chrome.runtime.sendMessage({ target: "offscreen", cmd: "start", tabStreamId, port: DESKTOP_PORT });
  } catch (e) {
    broadcast({ capture: "error", error: String(e.message || e) });
  }
}

async function stop() {
  try { await chrome.runtime.sendMessage({ target: "offscreen", cmd: "stop" }); } catch {}
  if (await hasOffscreen()) await chrome.offscreen.closeDocument();
  broadcast({ capture: "idle", ws: "disconnected", desktop: null });
}

chrome.runtime.onMessage.addListener((msg, sender, sendResponse) => {
  if (msg.cmd === "start") { start(); sendResponse({}); }
  else if (msg.cmd === "stop") { stop(); sendResponse({}); }
  else if (msg.cmd === "getStatus") { sendResponse(lastStatus); }
  else if (msg.type === "offscreen-status") { broadcast(msg.patch); }
  else if (msg.cmd === "openPermissionPage") {
    chrome.tabs.create({ url: chrome.runtime.getURL("perm.html") });
    sendResponse({});
  }
  return true;
});
