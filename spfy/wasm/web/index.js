// In-browser glue for the spfy WASM build.
//
// Flow:
//   1. Load the emscripten factory (createSpfyModule) at RUNTIME via
//      a <script> tag, not webpack's static `import`. This keeps the
//      dev server happy even when ./build.sh hasn't been run yet, and
//      sidesteps webpack trying to parse emscripten's UMD glue.
//   2. Await the module's runtime init → cwrap the C entrypoints.
//   3. Call spfy_wasm_init('/voice') once (voice loaded from the
//      preloaded virtual FS).
//   4. On Speak: call spfy_wasm_synth(text), read the int16 PCM from
//      WASM memory, convert to Float32 [-1,1], feed an AudioBuffer.
//   5. Stop button cancels any active AudioBufferSourceNode.
//   6. Save button packs the last PCM into a RIFF/WAVE blob and
//      triggers a download.

function loadEmscriptenFactory() {
    // The emscripten output is built with -s MODULARIZE=1
    // -s EXPORT_NAME=createSpfyModule, so loading the script via a <script>
    // tag attaches a `createSpfyModule` factory to window. We resolve to
    // that. If the script 404s (build not run yet), the rejection bubbles
    // up to boot() which renders a friendly status.
    return new Promise((resolve, reject) => {
        if (window.createSpfyModule) { resolve(window.createSpfyModule); return; }
        const s = document.createElement("script");
        // Same-origin relative URL; dev server serves dist/ at root via the
        // webpack.config.js static-dir entry.
        s.src = "spfy_wasm.js";
        s.async = true;
        s.onload = () => {
            if (window.createSpfyModule) resolve(window.createSpfyModule);
            else reject(new Error("spfy_wasm.js loaded but createSpfyModule is undefined"));
        };
        s.onerror = () => reject(new Error(
            "Failed to load spfy_wasm.js — run `./build.sh` to compile the WASM module first."));
        document.head.appendChild(s);
    });
}

const $ = (id) => document.getElementById(id);
const statusEl = $("status");
const progressEl = $("load-progress");
const synthSec = $("synth-section");
const textInput = $("text-input");
const speakBtn = $("speak-btn");
const stopBtn = $("stop-btn");
const saveBtn = $("save-btn");
const metaBox = $("meta");
const metaRate = $("meta-rate");
const metaSamps = $("meta-samples");
const metaElapsed = $("meta-elapsed");

const state = {
    module: null,
    initialized: false,
    audioCtx: null,
    activeSource: null,
    lastPcm: null,       // Int16Array, the last synthesis
    lastRate: 0,
};

// ---------------------------------------------------------------------
// Module bring-up
// ---------------------------------------------------------------------

function setStatus(text, cls = "") {
    statusEl.textContent = text;
    statusEl.className = "status" + (cls ? " " + cls : "");
}

async function boot() {
    setStatus("Loading WebAssembly module...");

    const createSpfyModule = await loadEmscriptenFactory();
    setStatus("Downloading WebAssembly module + voice data...");

    state.module = await createSpfyModule({
        // Routed to console; the C side prints diagnostics there.
        print: (msg) => console.log("[spfy]", msg),
        printErr: (msg) => console.warn("[spfy]", msg),
        setStatus: (s) => { if (s) console.log("[spfy.status]", s); },
        monitorRunDependencies: (left) => {
            // Emscripten decrements this as each async dependency resolves
            // (notably the .data file fetch). It hits 0 right before
            // onRuntimeInitialized.
            if (left > 0) setStatus(`Initializing... (${left} task${left === 1 ? "" : "s"} pending)`);
        },
    });

    // Pin the cwrap'd entry points.
    const init = state.module.cwrap("spfy_wasm_init", "number", ["string"]);
    const synth = state.module.cwrap("spfy_wasm_synth", "number", ["string"]);
    const pcmPtr = state.module.cwrap("spfy_wasm_pcm_ptr", "number", []);
    const pcmLen = state.module.cwrap("spfy_wasm_pcm_len", "number", []);
    const sampleRate = state.module.cwrap("spfy_wasm_sample_rate", "number", []);
    const reset = state.module.cwrap("spfy_wasm_reset", null, []);

    state.api = { init, synth, pcmPtr, pcmLen, sampleRate, reset };

    setStatus("Loading voice (Tom 8 kHz)...");
    const t0 = performance.now();
    const rc = init("/voice");
    if (rc !== 0) {
        setStatus(`Voice load failed (code ${rc}).`, "error");
        return;
    }
    state.initialized = true;
    state.lastRate = sampleRate();
    const dt = (performance.now() - t0).toFixed(0);
    setStatus(`Ready. Voice loaded in ${dt} ms.`, "ok");
    progressEl.value = 100;
    synthSec.hidden = false;
    speakBtn.disabled = false;
}

// ---------------------------------------------------------------------
// Synth + playback
// ---------------------------------------------------------------------

function ensureAudio() {
    if (!state.audioCtx) {
        state.audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    }
    if (state.audioCtx.state === "suspended") state.audioCtx.resume();
    return state.audioCtx;
}

function stopPlayback() {
    if (state.activeSource) {
        try { state.activeSource.stop(); } catch (_) { }
        state.activeSource.disconnect();
        state.activeSource = null;
    }
    stopBtn.disabled = true;
    speakBtn.disabled = false;
}

async function speak() {
    if (!state.initialized) return;
    const text = textInput.value.trim();
    if (!text) return;

    stopPlayback();
    speakBtn.disabled = true;
    setStatus("Synthesizing...");

    const t0 = performance.now();
    // Marshalling the JS string through the cwrap'd `string` arg copies
    // it into a temporary on the WASM stack; the C side strdups what it
    // needs.
    const rc = state.api.synth(text);
    if (rc !== 0) {
        setStatus(`Synthesis failed (code ${rc}).`, "error");
        speakBtn.disabled = false;
        return;
    }

    const ptr = state.api.pcmPtr();
    const nSamples = state.api.pcmLen();
    const rate = state.api.sampleRate();
    if (!ptr || !nSamples) {
        setStatus("Synthesis returned empty PCM.", "error");
        speakBtn.disabled = false;
        return;
    }

    // The pointer is into the wasm module's HEAP16 view. Copy out so a
    // future synth (which may realloc the buffer) can't invalidate the
    // typed-array view we hand to AudioBuffer.
    const pcmView = state.module.HEAP16.subarray(ptr >> 1, (ptr >> 1) + nSamples);
    const pcmCopy = new Int16Array(pcmView);   // .slice(), basically
    state.lastPcm = pcmCopy;
    state.lastRate = rate;

    const dt = (performance.now() - t0).toFixed(0);
    setStatus(`Synthesized ${nSamples.toLocaleString()} samples in ${dt} ms.`, "ok");

    metaRate.textContent = `Sample rate: ${rate} Hz`;
    metaSamps.textContent = `Duration: ${(nSamples / rate).toFixed(2)} s`;
    metaElapsed.textContent = `Synth time: ${dt} ms`;
    metaBox.hidden = false;

    saveBtn.disabled = false;

    // Convert s16 → f32 [-1, 1] for Web Audio.
    const audio = ensureAudio();
    const float = new Float32Array(nSamples);
    for (let i = 0; i < nSamples; i++) float[i] = pcmCopy[i] / 32768;

    // AudioBuffer wants the buffer at the destination's sample rate;
    // Web Audio resamples internally if our rate (e.g. 8000) differs.
    const buf = audio.createBuffer(1, nSamples, rate);
    buf.copyToChannel(float, 0);

    const src = audio.createBufferSource();
    src.buffer = buf;
    src.connect(audio.destination);
    src.onended = () => {
        if (state.activeSource === src) stopPlayback();
    };
    src.start();
    state.activeSource = src;
    stopBtn.disabled = false;
    speakBtn.disabled = false;   // re-enable for back-to-back synth
}

// ---------------------------------------------------------------------
// Save as WAV (RIFF header + s16 mono LE).
// ---------------------------------------------------------------------

function packWav(pcm, rate) {
    const dataBytes = pcm.length * 2;
    const buf = new ArrayBuffer(44 + dataBytes);
    const dv = new DataView(buf);
    const writeStr = (off, s) => {
        for (let i = 0; i < s.length; i++) dv.setUint8(off + i, s.charCodeAt(i));
    };
    writeStr(0, "RIFF");
    dv.setUint32(4, 36 + dataBytes, true);
    writeStr(8, "WAVE");
    writeStr(12, "fmt ");
    dv.setUint32(16, 16, true);             // PCM chunk size
    dv.setUint16(20, 1, true);             // format = 1 (PCM)
    dv.setUint16(22, 1, true);             // channels = 1
    dv.setUint32(24, rate, true);           // sample rate
    dv.setUint32(28, rate * 2, true);       // byte rate (rate * bytes/sample)
    dv.setUint16(32, 2, true);              // block align
    dv.setUint16(34, 16, true);             // bits per sample
    writeStr(36, "data");
    dv.setUint32(40, dataBytes, true);
    // PCM payload — little-endian s16.
    let off = 44;
    for (let i = 0; i < pcm.length; i++, off += 2) dv.setInt16(off, pcm[i], true);
    return new Blob([buf], { type: "audio/wav" });
}

function downloadWav() {
    if (!state.lastPcm) return;
    const blob = packWav(state.lastPcm, state.lastRate);
    const url = URL.createObjectURL(blob);
    const a = document.createElement("a");
    a.href = url;
    a.download = "spfy_synth.wav";
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    URL.revokeObjectURL(url);
}

// ---------------------------------------------------------------------
// Wire up
// ---------------------------------------------------------------------

speakBtn.addEventListener("click", () => {
    speak().catch(err => {
        console.error(err);
        setStatus(`Error: ${err.message || err}`, "error");
        speakBtn.disabled = false;
    });
});
stopBtn.addEventListener("click", stopPlayback);
saveBtn.addEventListener("click", downloadWav);

// Pressing Enter (without Shift) inside the textarea also speaks.
textInput.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && !e.shiftKey) {
        e.preventDefault();
        speak();
    }
});

boot().catch(err => {
    console.error(err);
    setStatus(`Failed to load module: ${err.message || err}`, "error");
});
