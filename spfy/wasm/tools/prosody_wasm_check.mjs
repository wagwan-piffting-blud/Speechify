// Do the volume and rate controls reach the audio in the WASM build?
//
// ssml_wasm_check.mjs proves the SSML pass runs, but every one of its
// assertions is a SAMPLE COUNT. Rate changes length, so it is covered; volume
// does not change length at all, so a completely dead volume control passes
// that file. This one reads the PCM and measures amplitude.
//
//   node tools/prosody_wasm_check.mjs            (default voice)
//   node tools/prosody_wasm_check.mjs crsmara
//
// Exit 0 when every assertion holds.

import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { createRequire } from "node:module";

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const DIST = path.resolve(__dirname, "..", "dist");
const require = createRequire(import.meta.url);

const manifest = JSON.parse(
    fs.readFileSync(path.join(DIST, "voices", "manifest.json"), "utf8"));
const wanted = process.argv[2] || manifest.default;
const voice = manifest.voices.find(v => v.id === wanted);
if (!voice) {
    console.error(`no such voice '${wanted}' in the manifest`);
    process.exit(2);
}

// See ssml_wasm_check.mjs for why this needs a .cjs copy and an explicit
// wasmBinary: package.json "type":"module" and -s ENVIRONMENT=web,worker.
const CJS = path.join(DIST, "spfy_wasm.prosody.cjs");
fs.copyFileSync(path.join(DIST, "spfy_wasm.js"), CJS);
process.on("exit", () => { try { fs.unlinkSync(CJS); } catch { /* ignore */ } });
const createSpfyModule = require(CJS);
const quiet = process.env.SPFY_WASM_VERBOSE ? console.log : () => {};

const mod = await createSpfyModule({
    wasmBinary: fs.readFileSync(path.join(DIST, "spfy_wasm.wasm")),
    print: quiet,
    printErr: quiet,
});

mod.FS.mkdir("/voice");
for (const f of voice.files) {
    const stream = mod.FS.open("/voice/" + f.name, "w");
    let pos = 0;
    for (const part of f.parts) {
        if (/^https?:\/\//i.test(part)) {
            console.error(`voice '${voice.id}' is hosted off-Pages (${part}); `
                        + `pick a locally staged voice instead`);
            process.exit(2);
        }
        const buf = fs.readFileSync(path.join(DIST, "voices", voice.dir, part));
        mod.FS.write(stream, buf, 0, buf.length, pos);
        pos += buf.length;
    }
    mod.FS.close(stream);
}

const api = {
    init: mod.cwrap("spfy_wasm_init", "number", ["string", "string"]),
    synth: mod.cwrap("spfy_wasm_synth", "number", ["string"]),
    pcmPtr: mod.cwrap("spfy_wasm_pcm_ptr", "number", []),
    pcmLen: mod.cwrap("spfy_wasm_pcm_len", "number", []),
    rate: mod.cwrap("spfy_wasm_sample_rate", "number", []),
};

if (api.init("/voice", voice.prefix) !== 0) {
    console.error("spfy_wasm_init failed");
    process.exit(1);
}

function render(text) {
    const r = api.synth(text);
    if (r !== 0) throw new Error(`spfy_wasm_synth -> ${r} for ${JSON.stringify(text)}`);
    const n = api.pcmLen();
    // Copy out: the next synth reuses the buffer.
    const pcm = new Int16Array(mod.HEAP16.buffer, api.pcmPtr(), n).slice();
    let acc = 0, peak = 0;
    for (let i = 0; i < n; i++) {
        acc += pcm[i] * pcm[i];
        const a = Math.abs(pcm[i]);
        if (a > peak) peak = a;
    }
    return { n, rms: n ? Math.sqrt(acc / n) : 0, peak };
}

const BASE = "The national weather service has issued a warning.";
const ctl = render(BASE);
console.log(`voice ${voice.id}, ${api.rate()} Hz`);
console.log(`control: ${ctl.n} samples, rms ${ctl.rms.toFixed(1)}, peak ${ctl.peak}`);
console.log("-".repeat(72));

// name, text, predicate on {n, rms, peak} relative to the control
const CASES = [
    ["inline \\!vp50 halves",     "\\!vp50 " + BASE,
     r => Math.abs(r.rms / ctl.rms - 0.5) < 0.03],
    ["inline \\!vp0 silences",    "\\!vp0 " + BASE,
     r => r.peak === 0],
    ["inline \\!vp100 is neutral", "\\!vp100 " + BASE,
     r => Math.abs(r.rms / ctl.rms - 1.0) < 0.001 && r.n === ctl.n],
    ["ssml volume soft quieter", `<prosody volume="soft">${BASE}</prosody>`,
     r => Math.abs(r.rms / ctl.rms - 0.5) < 0.03],
    ["ssml volume silent silent", `<prosody volume="silent">${BASE}</prosody>`,
     r => r.peak === 0],
    ["ssml volume loud louder",  `<prosody volume="loud">${BASE}</prosody>`,
     r => r.rms / ctl.rms > 1.2],
    // Rate is length, not amplitude -- kept here so one file covers both
    // controls the user actually reaches for.
    ["inline \\!rp50 lengthens",  "\\!rp50 " + BASE,
     r => r.n / ctl.n > 1.5],
    ["inline \\!rp200 shortens",  "\\!rp200 " + BASE,
     r => r.n / ctl.n < 0.7],
    ["ssml rate x-slow lengthens", `<prosody rate="x-slow">${BASE}</prosody>`,
     r => r.n / ctl.n > 1.5],
];

let fail = 0;
for (const [name, text, ok] of CASES) {
    const r = render(text);
    const good = ok(r);
    if (!good) fail++;
    console.log(`${good ? "PASS" : "FAIL"} ${name.padEnd(28)} `
              + `n=${String(r.n).padStart(6)} rms=${r.rms.toFixed(1).padStart(8)} `
              + `peak=${String(r.peak).padStart(6)} `
              + `(x${(r.rms / ctl.rms).toFixed(3)} amp, x${(r.n / ctl.n).toFixed(3)} len)`);
}
console.log("-".repeat(72));
console.log(fail ? `${fail} failed` : "all passed");
process.exit(fail ? 1 : 0);
