// extension/offscreen.js
const TAG = { mic: 0, tab: 1 };
const CHUNK_SAMPLES = 1600; // 100 ms @ 16 kHz

let ws = null, ctx = null, tracks = [], reconnectTimer = null, active = false;
let wsUrl = "";
let reconnectAttempts = 0;

function status(patch) {
  chrome.runtime.sendMessage({ type: "offscreen-status", patch }).catch(() => {});
}

function sendFrame(tag, samples) {
  if (!ws || ws.readyState !== WebSocket.OPEN) return;
  const buf = new ArrayBuffer(9 + samples.length * 2);
  const view = new DataView(buf);
  view.setUint8(0, tag);
  view.setFloat64(1, Date.now(), true); // little-endian
  for (let i = 0; i < samples.length; i++) {
    const s = Math.max(-1, Math.min(1, samples[i]));
    view.setInt16(9 + i * 2, (s * 32767) | 0, true);
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
    ws.send(JSON.stringify({ type: "hello", version: 1, sampleRate: 16000, channels: 1, format: "s16le", streams: ["mic", "tab"] }));
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
      const delay = Math.min(8000, 500 * Math.pow(2, reconnectAttempts - 1));
      reconnectTimer = setTimeout(connectWs, delay);
    }
  };
  ws.onerror = () => {};
}

async function start(tabStreamId, port) {
  active = true;
  wsUrl = `ws://127.0.0.1:${port}`;

  let micStream;
  try {
    micStream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: true } });
  } catch (e) {
    status({ capture: "error", micPermission: "needed", error: "Microphone permission needed — click the button in the popup." });
    chrome.runtime.sendMessage({ cmd: "openPermissionPage" }).catch(() => {});
    return;
  }
  const tabStream = await navigator.mediaDevices.getUserMedia({
    audio: { mandatory: { chromeMediaSource: "tab", chromeMediaSourceId: tabStreamId } },
  });
  tracks = [...micStream.getTracks(), ...tabStream.getTracks()];

  // keep the call audible: route captured tab audio back to the speakers
  document.getElementById("passthrough").srcObject = tabStream;

  ctx = new AudioContext({ sampleRate: 16000 });  // browser resamples for us
  await ctx.audioWorklet.addModule("pcm-worklet.js");
  const micNode = new AudioWorkletNode(ctx, "pcm-writer");
  const tabNode = new AudioWorkletNode(ctx, "pcm-writer");
  const micAcc = makeAccumulator(TAG.mic);
  const tabAcc = makeAccumulator(TAG.tab);
  micNode.port.onmessage = (e) => micAcc(e.data);
  tabNode.port.onmessage = (e) => tabAcc(e.data);
  ctx.createMediaStreamSource(micStream).connect(micNode);
  ctx.createMediaStreamSource(tabStream).connect(tabNode);

  connectWs();
  status({ capture: "running", micPermission: "granted" });
}

function stop() {
  active = false;
  clearTimeout(reconnectTimer);
  reconnectTimer = null;
  reconnectAttempts = 0;
  if (ws) { try { ws.send(JSON.stringify({ type: "bye" })); } catch {} ws.close(); ws = null; }
  tracks.forEach((t) => t.stop());
  tracks = [];
  if (ctx) { ctx.close(); ctx = null; }
  status({ capture: "idle", ws: "disconnected", desktop: null });
}

chrome.runtime.onMessage.addListener((msg) => {
  if (msg.target !== "offscreen") return;
  if (msg.cmd === "start") start(msg.tabStreamId, msg.port);
  else if (msg.cmd === "stop") stop();
});
