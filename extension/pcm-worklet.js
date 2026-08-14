// extension/pcm-worklet.js
//
// AudioWorkletProcessor loaded into the AudioContext by offscreen.js
// (ctx.audioWorklet.addModule) and instantiated once per source (mic, tab)
// as an AudioWorkletNode. Runs on the browser's dedicated audio rendering
// thread, not offscreen.js's main thread -- process() is called
// automatically by the audio graph roughly every 128 samples, and results
// are handed back to offscreen.js's makeAccumulator() via port.postMessage.
// Downmixes input to mono Float32 and posts each 128-frame quantum to the page.
class PcmWriter extends AudioWorkletProcessor {
  process(inputs) {
    const input = inputs[0];
    if (!input || input.length === 0) return true;
    const frameLength = input[0].length;
    const mono = new Float32Array(frameLength);
    for (let channel = 0; channel < input.length; channel++) {
      const channelData = input[channel];
      for (let i = 0; i < frameLength; i++) mono[i] += channelData[i];
    }
    if (input.length > 1) for (let i = 0; i < frameLength; i++) mono[i] /= input.length;
    this.port.postMessage(mono, [mono.buffer]);
    return true;
  }
}
registerProcessor("pcm-writer", PcmWriter);
