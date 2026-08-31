// Confirm the SSML pass is live in the WASM build, not merely compiled in.
//
// Building proves src/common/ssml.c reached the emscripten link. It does NOT
// prove the pass runs: the hook lives in spfy_synth_to_sink, and the WASM
// entry point (spfy_wasm_synth) is a different caller from the CLI's main().
// This drives the real module, loads a real voice off dist/voices/, and
// compares rendered sample counts.
//
//   node tools/ssml_wasm_check.mjs            (default voice from the manifest)
//   node tools/ssml_wasm_check.mjs crsmara
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

// The emscripten output is MODULARIZE=1 / EXPORT_NAME=createSpfyModule and
// ends in a CommonJS `module.exports = createSpfyModule`.
//
// ⚠ requiring dist/spfy_wasm.js DIRECTLY returns a non-function. wasm/
// package.json says `"type": "module"`, which makes every .js under it an ES
// module as far as node is concerned - so that CommonJS tail never executes
// and require() hands back an empty namespace. The failure is
// `createSpfyModule is not a function`, which reads like a broken build.
//
// Copying to a .cjs sibling forces CommonJS regardless of package.json, and
// the copy has to live in dist/ because emscripten locates spfy_wasm.wasm
// relative to its own script.
const CJS = path.join(DIST, "spfy_wasm.check.cjs");
fs.copyFileSync(path.join(DIST, "spfy_wasm.js"), CJS);
process.on("exit", () => { try { fs.unlinkSync(CJS); } catch { /* ignore */ } });
const createSpfyModule = require(CJS);

const quiet = process.env.SPFY_WASM_VERBOSE ? console.log : () => {};

// ⚠ Hand the module its own .wasm bytes. The shipped build is
// `-s ENVIRONMENT=web,worker` (CMakeLists.txt:319) - a deliberate size choice -
// so it has NO file reader under node and aborts with "both async and sync
// fetching of the wasm failed". Emscripten still honours Module.wasmBinary,
// which lets this check drive the SHIPPED artifact byte-for-byte instead of
// needing a special node-enabled build that would prove something else.
const mod = await createSpfyModule({
    wasmBinary: fs.readFileSync(path.join(DIST, "spfy_wasm.wasm")),
    print: quiet,
    printErr: quiet,
});

// Stream the voice's three files into the module FS the same way index.js
// does, reassembling any file the staging step split into parts.
mod.FS.mkdir("/voice");
let total = 0;
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
        total += buf.length;
    }
    mod.FS.close(stream);
}

const api = {
    init: mod.cwrap("spfy_wasm_init", "number", ["string", "string"]),
    synth: mod.cwrap("spfy_wasm_synth", "number", ["string"]),
    pcmLen: mod.cwrap("spfy_wasm_pcm_len", "number", []),
    rate: mod.cwrap("spfy_wasm_sample_rate", "number", []),
};

const rc = api.init("/voice", voice.prefix);
if (rc !== 0) {
    console.error(`spfy_wasm_init -> ${rc}`);
    process.exit(1);
}
console.log(`voice ${voice.id} loaded (${(total / 1048576).toFixed(1)} MB), `
          + `${api.rate()} Hz`);

function samples(text) {
    const r = api.synth(text);
    if (r !== 0) {
        throw new Error(`spfy_wasm_synth -> ${r} for ${JSON.stringify(text)}`);
    }
    return api.pcmLen();
}

const BASE = "The national weather service has issued a warning.";
const cases = [
    // name, text, predicate against the control, why
    ["tags are not spoken",
     `<speak>${BASE}</speak>`,
     n => Math.abs(n - CTL) / CTL < 0.02,
     "within 2% of untagged - before the SSML pass this rendered the words "
     + "'speak', 'slash' and 'greater than' out loud and ran far longer"],
    ["prosody rate x-slow lengthens",
     `<prosody rate="x-slow">${BASE}</prosody>`,
     n => n / CTL > 1.5,
     "the WSOLA span reached the sink"],
    ["prosody rate x-fast shortens",
     `<prosody rate="x-fast">${BASE}</prosody>`,
     n => n / CTL < 0.85,
     "and in the other direction"],
    ["break adds silence",
     `The national weather service<break time="1s"/> has issued a warning.`,
     n => (n - CTL) / api.rate() > 0.8,
     "at least 0.8 s of the requested 1 s"],
];

let CTL = samples(BASE);
console.log(`control: ${CTL} samples (${(CTL / api.rate()).toFixed(3)} s)`);
console.log("-".repeat(66));

let fail = 0;
for (const [name, text, ok, why] of cases) {
    const n = samples(text);
    const good = ok(n);
    if (!good) fail++;
    console.log(`${good ? "PASS" : "FAIL"} ${name.padEnd(30)} `
              + `${n} samples (x${(n / CTL).toFixed(2)})`);
    if (!good) console.log(`     expected: ${why}`);
}
console.log("-".repeat(66));
console.log(fail ? `${fail} failed` : "all passed");
process.exit(fail ? 1 : 0);
