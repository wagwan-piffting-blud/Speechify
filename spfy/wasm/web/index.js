// In-browser glue for the spfy WASM build.
//
// Lazy voice loading:
//   1. Load the emscripten factory (createSpfyModule) at RUNTIME via a
//      <script> tag (not webpack's static import) so the dev server is
//      happy even before ./build.sh has run.
//   2. Fetch voices/manifest.json — the catalog of shippable voices and
//      their asset URLs (produced by tools/stage_voices.py at build time).
//   3. On voice select: make sure we have a module instance for that
//      voice's LANGUAGE (the emulator FE boots one DLL per instance, so a
//      language change means a fresh module), fetch the voice's
//      vin/vdb/vcf over the network — stitching any >90 MiB file that was
//      split into <100 MB parts for GitHub Pages — stream them into the
//      emscripten FS, then call spfy_wasm_init('/voice', prefix).
//   4. On Speak: spfy_wasm_synth(text), read the int16 PCM from WASM
//      memory, convert to Float32 [-1,1], feed an AudioBuffer.
//   5. Stop cancels playback; Save packs the last PCM into a WAV blob.
//
// Nothing is baked into the module: the page starts as just the ~14 MB
// wasm + tiny JS and pulls a voice only when one is chosen.

const MANIFEST_URL = "voices/manifest.json";

function loadEmscriptenFactory() {
    // The emscripten output is built with -s MODULARIZE=1
    // -s EXPORT_NAME=createSpfyModule, so loading the script via a <script>
    // tag attaches a `createSpfyModule` factory to window. The factory can
    // be called repeatedly to mint independent module instances (we do
    // that once per language). If the script 404s (build not run yet), the
    // rejection bubbles up to boot() which renders a friendly status.
    return new Promise((resolve, reject) => {
        if (window.createSpfyModule) { resolve(window.createSpfyModule); return; }
        const s = document.createElement("script");
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
const voiceSelect = $("voice-select");
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
    manifest: null,
    module: null,        // current emscripten Module instance
    moduleLang: null,    // language its FE DLL was booted for
    api: null,           // cwrap'd entry points for state.module
    currentVoice: null,  // id of the voice currently loaded
    loading: false,
    audioCtx: null,
    activeSource: null,
    lastPcm: null,       // Int16Array, the last synthesis
    lastRate: 0,
};

const MB = 1024 * 1024;
const fmtMB = (n) => (n / MB).toFixed(0);

function setStatus(text, cls = "") {
    statusEl.textContent = text;
    statusEl.className = "status" + (cls ? " " + cls : "");
}

// ---------------------------------------------------------------------
// Module bring-up (one instance per language)
// ---------------------------------------------------------------------

async function createModuleForLang(lang) {
    // Drop the old instance first so its wasm memory can be reclaimed.
    state.module = null;
    state.api = null;
    state.moduleLang = null;

    const createSpfyModule = await loadEmscriptenFactory();
    const mod = await createSpfyModule({
        print:    (msg) => console.log("[spfy]", msg),
        printErr: (msg) => console.warn("[spfy]", msg),
    });

    state.api = {
        init:       mod.cwrap("spfy_wasm_init",        "number", ["string", "string"]),
        freeVoice:  mod.cwrap("spfy_wasm_free_voice",  null,     []),
        synth:      mod.cwrap("spfy_wasm_synth",       "number", ["string"]),
        pcmPtr:     mod.cwrap("spfy_wasm_pcm_ptr",     "number", []),
        pcmLen:     mod.cwrap("spfy_wasm_pcm_len",     "number", []),
        sampleRate: mod.cwrap("spfy_wasm_sample_rate", "number", []),
        reset:      mod.cwrap("spfy_wasm_reset",       null,     []),
    };
    state.module = mod;
    state.moduleLang = lang;
    return mod;
}

// ---------------------------------------------------------------------
// Voice fetch → emscripten FS
// ---------------------------------------------------------------------

// Stream one voice's files into /voice on the module FS. Each file may be
// a single object or a list of <100 MB parts; parts are written back to
// back so the reassembled file is byte-identical. Streams chunk-by-chunk
// so even a 253 MB VDB never sits fully in a JS buffer.
async function fetchVoiceIntoFS(voice, onProgress) {
    const FS = state.module.FS;
    try { FS.mkdir("/voice"); } catch (_) { /* already exists */ }

    let done = 0;
    for (const f of voice.files) {
        const stream = FS.open("/voice/" + f.name, "w");
        let pos = 0;
        for (const part of f.parts) {
            // A part is normally a filename relative to voices/<dir>/, but
            // may be an absolute URL — that lets an over-100 MB voice be
            // hosted off-Pages (e.g. a GitHub Release asset or CDN) while
            // the rest ship from the site.
            const url = /^https?:\/\//i.test(part)
                ? part
                : "voices/" + voice.dir + "/" + part;
            const resp = await fetch(url);
            if (!resp.ok) throw new Error(`fetch ${url} → HTTP ${resp.status}`);

            if (resp.body && resp.body.getReader) {
                const reader = resp.body.getReader();
                for (;;) {
                    const { done: rdDone, value } = await reader.read();
                    if (rdDone) break;
                    FS.write(stream, value, 0, value.length, pos);
                    pos += value.length;
                    done += value.length;
                    onProgress(done, voice.totalBytes);
                }
            } else {
                // Fallback for environments without a streaming body.
                const buf = new Uint8Array(await resp.arrayBuffer());
                FS.write(stream, buf, 0, buf.length, pos);
                pos += buf.length;
                done += buf.length;
                onProgress(done, voice.totalBytes);
            }
        }
        FS.close(stream);
    }
}

async function loadVoice(voiceId) {
    if (state.loading) return;
    const voice = state.manifest.voices.find((v) => v.id === voiceId);
    if (!voice) return;

    // Gate the heavy voices behind an explicit confirm.
    if (voice.large) {
        const ok = window.confirm(
            `${voice.display} is a large voice — about ${fmtMB(voice.totalBytes)} MB ` +
            `to download (mostly its unit-selection database). Continue?`);
        if (!ok) {
            if (state.currentVoice) voiceSelect.value = state.currentVoice;
            return;
        }
    }

    state.loading = true;
    voiceSelect.disabled = true;
    speakBtn.disabled = true;
    progressEl.value = 0;

    try {
        // Always mint a fresh module instance for the selected voice. The
        // emulator boots exactly one SWIttsFe DLL per instance and treats
        // the boot as idempotent ("first voice wins"), so a fresh instance
        // — fresh WASM memory, hence a clean copy of the DLL's data segment
        // — is the robust way to (re)boot the right-language FE with no
        // carried-over state. The old instance's memory is dropped for GC.
        setStatus(`Loading engine for ${voice.lang}…`);
        await createModuleForLang(voice.lang);

        const total = voice.totalBytes;
        setStatus(`Downloading ${voice.display} … 0 / ${fmtMB(total)} MB`);
        const t0 = performance.now();
        await fetchVoiceIntoFS(voice, (dl) => {
            progressEl.value = total ? (dl / total) * 100 : 0;
            setStatus(`Downloading ${voice.display} … ${fmtMB(dl)} / ${fmtMB(total)} MB`);
        });

        setStatus(`Loading ${voice.display} …`);
        const rc = state.api.init("/voice", voice.prefix);
        if (rc !== 0) {
            setStatus(`Voice load failed (code ${rc}).`, "error");
            return;
        }

        state.currentVoice = voice.id;
        state.lastRate = state.api.sampleRate();
        const dt = (performance.now() - t0).toFixed(0);
        setStatus(`Ready — ${voice.display} loaded in ${dt} ms.`, "ok");
        progressEl.value = 100;
        synthSec.hidden = false;
        speakBtn.disabled = false;
    } catch (err) {
        console.error(err);
        setStatus(`Error loading ${voice.display}: ${err.message || err}`, "error");
    } finally {
        state.loading = false;
        voiceSelect.disabled = false;
    }
}

function populateVoicePicker(voices) {
    voiceSelect.innerHTML = "";
    for (const v of voices) {
        const opt = document.createElement("option");
        opt.value = v.id;
        const size = `~${fmtMB(v.totalBytes)} MB`;
        opt.textContent = `${v.display} (${v.lang}) — ${size}${v.large ? " ⚠" : ""}`;
        voiceSelect.appendChild(opt);
    }
    voiceSelect.disabled = false;
}

async function boot() {
    setStatus("Loading voice catalog…");

    let resp;
    try {
        resp = await fetch(MANIFEST_URL);
    } catch (e) {
        throw new Error("Could not fetch voices/manifest.json — run `./build.sh` first.");
    }
    if (!resp.ok) throw new Error(`voices/manifest.json → HTTP ${resp.status}`);
    state.manifest = await resp.json();

    const voices = state.manifest.voices || [];
    if (!voices.length) { setStatus("No voices available in the manifest.", "error"); return; }

    populateVoicePicker(voices);
    voiceSelect.addEventListener("change", () => loadVoice(voiceSelect.value));

    // Auto-load the first (default) voice through the same lazy path so the
    // demo is ready to speak on arrival.
    voiceSelect.value = voices[0].id;
    await loadVoice(voices[0].id);
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
    if (!state.currentVoice || !state.api) return;
    const text = textInput.value.trim();
    if (!text) return;

    stopPlayback();
    speakBtn.disabled = true;
    setStatus("Synthesizing…");

    const t0 = performance.now();
    // Marshalling the JS string through the cwrap'd `string` arg copies it
    // into a temporary on the WASM stack; the C side strdups what it needs.
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
    a.download = `spfy_${state.currentVoice || "synth"}.wav`;
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
    setStatus(`Failed to start: ${err.message || err}`, "error");
});
