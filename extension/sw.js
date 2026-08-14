// extension/sw.js
//
// MV3 background service worker: the extension's message-routing hub. Owns
// the offscreen document's lifecycle (creates it on start, closes it on
// stop -- offscreen.js does the actual mic/tab capture, since service
// workers cannot use getUserMedia/AudioContext), and relays status between
// offscreen.js and popup.js. Has no persistent state of its own beyond
// lastStatus, which is checkpointed to chrome.storage.session because the
// worker itself is evicted and re-run from scratch after ~30s idle.
const OFFSCREEN_URL = "offscreen.html";
const DESKTOP_PORT = 8765;

let lastStatus = {
  type: "ext-status", capture: "idle", ws: "disconnected",
  desktop: null, error: null, micPermission: "unknown",
};

// MV3 service workers are evicted after ~30s idle and re-spawned on the next
// event; every module-scope variable (including lastStatus above) resets to
// its initial value on each fresh evaluation of this script. Restore
// whatever was last persisted so a freshly-woken worker doesn't report
// "idle/idle" to a popup that opens right after an eviction.
chrome.storage.session.get("lastStatus").then((stored) => {
  if (stored && stored.lastStatus) lastStatus = stored.lastStatus;
}).catch(() => {});

function broadcast(patch) {
  lastStatus = { ...lastStatus, ...patch, type: "ext-status" };
  chrome.runtime.sendMessage(lastStatus).catch(() => {});
  // MV3 service workers are evicted after ~30s idle; module-scope state
  // (lastStatus) is lost on the next wake. Persist to session storage
  // (cleared on browser close, unlike chrome.storage.local) so a
  // subsequently-woken worker's getStatus handler can restore it instead of
  // the popup seeing a reset-to-initial "idle/idle" state. Fire-and-forget:
  // a failed write here shouldn't block/break the broadcast.
  chrome.storage.session.set({ lastStatus }).catch(() => {});
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
  else if (msg.cmd === "getStatus") {
    // Read storage first rather than trusting module-scope lastStatus: if
    // this worker instance just woke from eviction, lastStatus may still be
    // holding its initial "idle/idle" default until the startup restore
    // above resolves. Prefer whatever is persisted; fall back to the
    // in-memory value if nothing was ever persisted (e.g. fresh install).
    (async () => {
      let stored;
      try {
        stored = await chrome.storage.session.get("lastStatus");
      } catch {
        stored = {};
      }
      sendResponse(stored.lastStatus ?? lastStatus);
    })();
  }
  else if (msg.type === "offscreen-status") { broadcast(msg.patch); }
  else if (msg.cmd === "openPermissionPage") {
    chrome.tabs.create({ url: chrome.runtime.getURL("perm.html") });
    sendResponse({});
  }
  return true;
});
