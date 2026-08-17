// extension/offscreen.js
//
// Does the actual audio work: captures mic + tab audio, runs both through
// the pcm-worklet.js AudioWorklet to get mono Float32 chunks, accumulates
// those into fixed-size 100ms frames, and ships each frame as a binary
// WebSocket message to the desktop app (net/ws_server.h) using the wire
// format in core/protocol.h. Lives in an offscreen document (not the service
// worker) because getUserMedia/AudioContext require a DOM context that MV3
// service workers don't have. Capture graph: getUserMedia(mic) and
// getUserMedia(tab) -> AudioContext -> one AudioWorkletNode per source ->
// makeAccumulator() -> sendFrame() -> WebSocket. Commanded by sw.js via
// chrome.runtime messages (start/stop); reports status back the same way.
const TAG = { mic: 0, tab: 1 };
const CHUNK_SAMPLES = 1600; // 100 ms @ 16 kHz.
const SAMPLE_RATE_HZ = 16000; // Wire-contract sample rate; must match desktop/src/core/protocol.h's kSampleRateHz.
const PCM_MAX = 32767; // Max magnitude of a signed PCM16 sample.
const BACKOFF_INITIAL_MS = 500; // First WS reconnect delay.
const BACKOFF_MAX_MS = 8000; // Reconnect delay ceiling.

let ws = null, ctx = null, tracks = [], reconnectTimer = null, active = false;
let wsUrl = "";
let reconnectAttempts = 0;
// start() awaits multiple async steps (getUserMedia x2, addModule), each of
// which yields control and gives the user (or a bug) a window to call
// stop() -- or click Start again -- before the previous start() finishes.
// Without a guard, a stale start() resuming after that would clobber the
// state a newer stop()/start() already set up (or open a duplicate WS/audio
// graph). startGeneration makes each start() attempt check, after every
// await, whether it is still the current attempt before touching shared
// state; if not, it tears down whatever it privately acquired and bails.
let startGeneration = 0; // Bumped by stop() (and by a fresh start()) to invalidate stale in-flight start() calls.

function status(patch) {
  chrome.runtime.sendMessage({ type: "offscreen-status", patch }).catch(() => {});
}

function sendFrame(tag, samples) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  const buf = new ArrayBuffer(9 + samples.length * 2);
  const view = new DataView(buf);
  view.setUint8(0, tag);
  view.setFloat64(1, Date.now(), true); // Little-endian.
  for (let i = 0; i < samples.length; i++) {
    const clampedSample = Math.max(-1, Math.min(1, samples[i]));
    view.setInt16(9 + i * 2, (clampedSample * PCM_MAX) | 0, true);
  }
  ws.send(buf);
}

function makeAccumulator(tag) {
  let acc = new Float32Array(CHUNK_SAMPLES);
  let fill = 0;
  return (mono) => {
    let off = 0;
    while (off < mono.length) {
      const take = Math.min(CHUNK_SAMPLES - fill, mono.length - off);
      acc.set(mono.subarray(off, off + take), fill);
      fill += take; off += take;
      if (fill === CHUNK_SAMPLES) { sendFrame(tag, acc); fill = 0; }
    }
  };
}

function connectWs() {
  ws = new WebSocket(wsUrl);
  ws.binaryType = "arraybuffer";
  ws.onopen = () => {
    reconnectAttempts = 0;
    status({ ws: "connected", error: null });
    ws.send(JSON.stringify({ type: "hello", version: 1, sampleRate: SAMPLE_RATE_HZ, channels: 1, format: "s16le", streams: ["mic", "tab"] }));
  };
  ws.onmessage = (ev) => {
    if (typeof ev.data !== "string") return;
    try {
      const msg = JSON.parse(ev.data);
      if (msg.type === "status") status({ desktop: { engine: msg.engine, provider: msg.provider, streams: msg.streams } });
      else if (msg.type === "error") status({ error: "desktop: " + msg.message });
    } catch {}
  };
  ws.onclose = () => {
    status({ ws: "disconnected", desktop: null });
    if (active) {
      reconnectAttempts++;
      const delay = Math.min(BACKOFF_MAX_MS, BACKOFF_INITIAL_MS * Math.pow(2, reconnectAttempts - 1));
      reconnectTimer = setTimeout(connectWs, delay);
    }
  };
  ws.onerror = () => {};
}

async function start(tabStreamId, port) {
  // Capturing myGen here (and bumping startGeneration) also invalidates any older
  // in-flight start() call, so only the most recent start()/stop() "wins".
  const myGen = ++startGeneration;
  active = true;
  // Plain ws:// to loopback: audio never leaves this machine, so transport
  // encryption adds nothing today. A future remote desktop app would need
  // wss:// (TLS) here and authentication on the server side.
  wsUrl = `ws://127.0.0.1:${port}`;

  let micStream;
  try {
    micStream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true } });
  } catch (err) {
    active = false;
    if (startGeneration !== myGen) return; // stop() (or a newer start) superseded this attempt; it already reported idle.
    if (err.name === "NotAllowedError") {
      status({ capture: "error", micPermission: "needed", error: "Microphone permission needed — click the button in the popup." });
      chrome.runtime.sendMessage({ cmd: "openPermissionPage" }).catch(() => {});
    } else {
      status({ capture: "error", error: "microphone unavailable: " + err.name });
    }
    return;
  }
  if (startGeneration !== myGen) { micStream.getTracks().forEach((track) => track.stop()); return; }

  let tabStream;
  try {
    tabStream = await navigator.mediaDevices.getUserMedia({
      audio: { mandatory: { chromeMediaSource: "tab", chromeMediaSourceId: tabStreamId } },
    });
  } catch (err) {
    micStream.getTracks().forEach((track) => track.stop());
    if (startGeneration !== myGen) return; // Superseded; stop() already reported idle.
    status({ capture: "error", error: "tab capture failed: " + (err.message || err) });
    return;
  }
  if (startGeneration !== myGen) {
    micStream.getTracks().forEach((track) => track.stop());
    tabStream.getTracks().forEach((track) => track.stop());
    return;
  }

  tracks = [...micStream.getTracks(), ...tabStream.getTracks()];

  // Keep the call audible: route captured tab audio back to the speakers.
  document.getElementById("passthrough").srcObject = tabStream;

  ctx = new AudioContext({ sampleRate: SAMPLE_RATE_HZ });  // Browser resamples for us.
  await ctx.audioWorklet.addModule("pcm-worklet.js");
  if (startGeneration !== myGen) {
    // ctx/tracks are module-level and may already have been torn down by a racing
    // stop() during this await, so null-guard rather than assuming they're live.
    if (ctx) { ctx.close(); ctx = null; }
    tracks.forEach((track) => track.stop());
    tracks = [];
    return;
  }
  const micNode = new AudioWorkletNode(ctx, "pcm-writer");
  const tabNode = new AudioWorkletNode(ctx, "pcm-writer");
  const micAcc = makeAccumulator(TAG.mic);
  const tabAcc = makeAccumulator(TAG.tab);
  micNode.port.onmessage = (event) => micAcc(event.data);
  tabNode.port.onmessage = (event) => tabAcc(event.data);
  ctx.createMediaStreamSource(micStream).connect(micNode);
  ctx.createMediaStreamSource(tabStream).connect(tabNode);

  // AudioWorkletNodes with no path to the context's destination can be
  // treated as unconnected/inactive by the audio graph and never have their
  // process() called at all (behavior varies but is not reliable to depend
  // on), which would silently stop audio from ever reaching sendFrame.
  // Route both nodes through a zero-gain sink to destination: this keeps the
  // graph "live" for both nodes without adding any audible output (on top of
  // the separate, intentional tab-audio passthrough above).
  const sink = new GainNode(ctx, { gain: 0 });
  micNode.connect(sink);
  tabNode.connect(sink);
  sink.connect(ctx.destination);

  connectWs();
  status({ capture: "running", micPermission: "granted" });
}

function stop() {
  startGeneration++;
  active = false;
  clearTimeout(reconnectTimer);
  reconnectTimer = null;
  reconnectAttempts = 0;
  if (ws) { try { ws.send(JSON.stringify({ type: "bye" })); } catch {} ws.close(); ws = null; }
  tracks.forEach((track) => track.stop());
  tracks = [];
  if (ctx) { ctx.close(); ctx = null; }
  status({ capture: "idle", ws: "disconnected", desktop: null });
}

chrome.runtime.onMessage.addListener((msg) => {
  if (msg.target !== "offscreen") return;
  if (msg.cmd === "start") start(msg.tabStreamId, msg.port);
  else if (msg.cmd === "stop") stop();
});
