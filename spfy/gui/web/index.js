// spfy GUI - browser side.
//
// FORKED FROM spfy/wasm/web/index.js, NOT SHARED WITH IT, and that was a
// deliberate call. The WASM demo is deployed elsewhere (GitHub Pages, EAS
// Tools) and has to keep standing on its own; the desktop app must never be
// able to break it. The cost is knowingly accepted: the editor, the palette
// and the highlighting will exist twice.
//
// The other half of the fork is that the backends have nothing in common. The
// demo calls spfy_wasm_synth() inside its own address space and reads int16
// PCM out of HEAP16. This calls into Rust, which spawns spfy_synth and hands
// back a whole WAV file - so playback here is decodeAudioData, not manual PCM
// assembly.

const { invoke } = window.__TAURI__.core;
const { save } = window.__TAURI__.dialog;

const $ = (id) => document.getElementById(id);
const voiceSelect = $("voice-select");
const refreshBtn = $("refresh-btn");
const textInput = $("text-input");
const speakBtn = $("speak-btn");
const stopBtn = $("stop-btn");
const saveBtn = $("save-btn");
const statusEl = $("status");
const tallyEl = $("tally");
const fatalEl = $("fatal");
const noVoicesEl = $("no-voices");
const searchPathEl = $("search-path");
const tabsEl = $("palette-tabs");
const panelsEl = $("palette-panels");
const paletteToggle = $("palette-toggle");
const paletteBody = $("palette-body");

const state = {
    voices: [],
    audioCtx: null,
    activeSource: null,
    lastWav: null,       // Uint8Array of the most recent synthesis
    lastVoice: null,
    busy: false,
};

const MB = 1024 * 1024;

function setStatus(text) {
    statusEl.textContent = text;
}

function setFatal(text) {
    if (!text) {
        fatalEl.hidden = true;
        fatalEl.textContent = "";
        return;
    }
    fatalEl.textContent = text;
    fatalEl.hidden = false;
}

// ---------------------------------------------------------------------
// The insert palette
// ---------------------------------------------------------------------
//
// `insert` templates use two markers:
//   $S  where the current selection goes (empty string when nothing is
//       selected; the caret lands here so you can type straight into the tag)
//   $C  where the caret goes instead, for tags whose interesting field is an
//       ATTRIBUTE rather than the content - <sub alias="…">, <phoneme ph="…">
//
// Everything below is real, working notation for this engine. SSML is
// translated by spfy/src/common/ssml.c; the `\!` forms are the Speechify
// User's Guide embedded tags, handled by spfy_etags_resolve().

const PALETTE = [
    {
        id: "voice",
        label: "Voice",
        groups: [
            {
                legend: "Rate",
                items: [
                    { label: "Slower", insert: '<prosody rate="slow">$S</prosody>' },
                    { label: "Faster", insert: '<prosody rate="fast">$S</prosody>' },
                    { label: "Much slower", insert: '<prosody rate="x-slow">$S</prosody>' },
                    { label: "Much faster", insert: '<prosody rate="x-fast">$S</prosody>' },
                ],
            },
            {
                legend: "Pitch",
                items: [
                    { label: "Lower", insert: '<prosody pitch="-4st">$S</prosody>' },
                    { label: "Higher", insert: '<prosody pitch="+4st">$S</prosody>' },
                    { label: "Much lower", insert: '<prosody pitch="-8st">$S</prosody>' },
                    { label: "Much higher", insert: '<prosody pitch="+8st">$S</prosody>' },
                ],
            },
            {
                legend: "Volume",
                items: [
                    { label: "Softer", insert: '<prosody volume="soft">$S</prosody>' },
                    { label: "Louder", insert: '<prosody volume="loud">$S</prosody>' },
                ],
            },
        ],
    },
    {
        id: "pauses",
        label: "Pauses",
        groups: [
            {
                legend: "Silence",
                items: [
                    { label: "0.2 s", insert: '<break time="200ms"/>' },
                    { label: "0.5 s", insert: '<break time="500ms"/>' },
                    { label: "1 s", insert: '<break time="1s"/>' },
                    { label: "2 s", insert: '<break time="2s"/>' },
                ],
            },
            {
                legend: "Structure",
                items: [
                    { label: "Sentence", insert: "<s>$S</s>" },
                    { label: "Paragraph", insert: "<p>$S</p>" },
                    // \!eos forces a sentence end without typing a full stop -
                    // useful after an abbreviation the front end would
                    // otherwise read straight through.
                    { label: "End sentence", insert: "\\!eos " },
                ],
            },
        ],
    },
    {
        id: "emphasis",
        label: "Emphasis",
        groups: [
            {
                legend: "Emphasis",
                items: [
                    { label: "Strong", insert: '<emphasis level="strong">$S</emphasis>' },
                    { label: "Moderate", insert: '<emphasis level="moderate">$S</emphasis>' },
                    { label: "Reduced", insert: '<emphasis level="reduced">$S</emphasis>' },
                ],
            },
            {
                // One-shot: binds to the NEXT word, then clears. That is the
                // engine's own rule, not a UI simplification.
                legend: "Pitch accent (next word)",
                items: [
                    { label: "H*", insert: "\\![ToBI:H*]", hint: "plain high accent" },
                    { label: "L+H*", insert: "\\![ToBI:L+H*]", hint: "highest available" },
                    { label: "!H*", insert: "\\![ToBI:!H*]", hint: "downstepped high" },
                    { label: "L*", insert: "\\![ToBI:L*]", hint: "lowest available" },
                    { label: "L*+H", insert: "\\![ToBI:L*+H]", hint: "low rising" },
                    { label: "None", insert: "\\![ToBI:NONE]", hint: "suppress the accent" },
                ],
            },
        ],
    },
    {
        id: "sayas",
        label: "Say as",
        groups: [
            {
                legend: "Interpretation",
                items: [
                    { label: "Spell out", insert: '<say-as interpret-as="characters">$S</say-as>' },
                    { label: "Digits", insert: '<say-as interpret-as="digits">$S</say-as>' },
                    { label: "Year", insert: '<say-as interpret-as="date">$S</say-as>' },
                ],
            },
            {
                legend: "Substitute",
                items: [
                    { label: "Say instead", insert: '<sub alias="$C">$S</sub>' },
                    { label: "IPA", insert: '<phoneme alphabet="ipa" ph="$C">$S</phoneme>' },
                    { label: "ARPAbet", insert: '<phoneme alphabet="x-arpabet" ph="$C">$S</phoneme>' },
                ],
            },
        ],
    },
    {
        id: "phones",
        label: "Phones",
        // Rendered as the big-character grid; see renderPhone().
        kind: "phones",
        groups: [
            {
                legend: "Syllable",
                kind: "plain",
                items: [
                    { label: "New word", insert: "\\![.1$C]", hint: "an empty stressed syllable" },
                    // Labelled with the notation itself, not a word for it -
                    // `.1` is literally what goes in the bracket, and the hint
                    // carries the meaning for the accessible name.
                    { label: ".1", insert: ".1", hint: "primary stress", mono: true },
                    { label: ".2", insert: ".2", hint: "secondary stress", mono: true },
                    { label: ".0", insert: ".0", hint: "unstressed", mono: true },
                ],
            },
            {
                legend: "Vowels",
                items: [
                    { spr: "i", arpa: "iy", eg: "fleece" },
                    { spr: "I", arpa: "ih", eg: "kit" },
                    { spr: "E", arpa: "eh", eg: "dress" },
                    { spr: "A", arpa: "ae", eg: "cat" },
                    { spr: "a", arpa: "aa", eg: "father" },
                    { spr: "c", arpa: "ao", eg: "thought" },
                    { spr: "o", arpa: "ow", eg: "goat" },
                    { spr: "U", arpa: "uh", eg: "foot" },
                    { spr: "u", arpa: "uw", eg: "goose" },
                    { spr: "H", arpa: "ah", eg: "cut" },
                    { spr: "x", arpa: "ax", eg: "about" },
                    { spr: "X", arpa: "ix", eg: "roses" },
                    { spr: "R", arpa: "er", eg: "nurse" },
                    { spr: "e", arpa: "ey", eg: "face" },
                    { spr: "Y", arpa: "ay", eg: "price" },
                    { spr: "W", arpa: "aw", eg: "mouth" },
                    { spr: "O", arpa: "oy", eg: "choice" },
                    { spr: "N", arpa: "en", eg: "button" },
                ],
            },
            {
                legend: "Consonants",
                items: [
                    { spr: "p", arpa: "p", eg: "pay" },
                    { spr: "b", arpa: "b", eg: "bee" },
                    { spr: "t", arpa: "t", eg: "tea" },
                    { spr: "d", arpa: "d", eg: "day" },
                    { spr: "k", arpa: "k", eg: "key" },
                    { spr: "g", arpa: "g", eg: "green" },
                    { spr: "F", arpa: "dx", eg: "butter" },
                    { spr: "C", arpa: "ch", eg: "cheese" },
                    { spr: "J", arpa: "jh", eg: "judge" },
                    { spr: "f", arpa: "f", eg: "fee" },
                    { spr: "v", arpa: "v", eg: "view" },
                    { spr: "T", arpa: "th", eg: "thin" },
                    { spr: "D", arpa: "dh", eg: "then" },
                    { spr: "s", arpa: "s", eg: "say" },
                    { spr: "z", arpa: "z", eg: "zoo" },
                    { spr: "S", arpa: "sh", eg: "she" },
                    { spr: "Z", arpa: "zh", eg: "measure" },
                    { spr: "h", arpa: "hh", eg: "house" },
                    { spr: "m", arpa: "m", eg: "may" },
                    { spr: "n", arpa: "n", eg: "now" },
                    { spr: "G", arpa: "ng", eg: "sing" },
                    { spr: "l", arpa: "l", eg: "lay" },
                    { spr: "r", arpa: "r", eg: "red" },
                    { spr: "w", arpa: "w", eg: "we" },
                    { spr: "y", arpa: "y", eg: "yes" },
                ],
            },
        ],
    },
];

// ---------------------------------------------------------------------
// Insertion
// ---------------------------------------------------------------------

/// Is the caret inside an open `\![ ... ]` phoneme block?
///
/// Decides whether a phone chip inserts a bare character (extending the word
/// being built) or a whole new `\![.1x]` block. ToBI blocks share the `\![`
/// opener but are NOT phoneme blocks - dropping a stray `k` into
/// `\![ToBI:H*]` would produce a tag the engine speaks out loud.
function inPhonemeBlock(value, pos) {
    const open = value.lastIndexOf("\\![", pos);
    if (open < 0) return false;
    if (value.startsWith("\\![ToBI:", open)) return false;
    const close = value.indexOf("]", open);
    return close < 0 || close >= pos;
}

function insertText(template, announce) {
    const el = textInput;
    const start = el.selectionStart;
    const end = el.selectionEnd;
    const sel = el.value.slice(start, end);

    // ⚠ The FUNCTION form of replace, not the string form. A replacement
    // string treats `$&`, `$'` and `$1` as substitution patterns, so pasting
    // a selection that happens to contain a dollar sign would silently mangle
    // it. The function form takes the text literally.
    const selAt = template.indexOf("$S");
    let text = template.replace("$S", () => sel);

    let caretAt = text.indexOf("$C");
    if (caretAt >= 0) text = text.replace("$C", () => "");

    el.setRangeText(text, start, end, "end");
    if (caretAt >= 0) {
        el.selectionStart = el.selectionEnd = start + caretAt;
    } else if (!sel && selAt >= 0) {
        // Nothing was selected, so put the caret where the content goes -
        // inside the tag, ready to type into.
        el.selectionStart = el.selectionEnd = start + selAt;
    }

    // Focus returns to the editor, not the chip. Keeps the caret model
    // coherent: you insert, then you keep typing.
    el.focus();
    if (announce) setStatus(`Inserted ${announce}`);
}

function insertPhone(spr) {
    const el = textInput;
    if (inPhonemeBlock(el.value, el.selectionStart)) {
        insertText(spr, spr);
    } else {
        insertText(`\\![.1${spr}]`, `\\![.1${spr}]`);
    }
}

// ---------------------------------------------------------------------
// Palette rendering
// ---------------------------------------------------------------------

function chipFor(item) {
    const b = document.createElement("button");
    b.type = "button";
    b.className = "chip";
    if (item.spr) {
        // THE SIGNATURE. The SPR character is what you actually type, so it is
        // the primary label; ARPAbet is the cross-reference underneath. Every
        // other TTS front end does this the other way round.
        b.classList.add("phone");
        const big = document.createElement("span");
        big.className = "phone-spr";
        big.textContent = item.spr;
        const small = document.createElement("span");
        small.className = "phone-arpa";
        small.textContent = item.arpa;
        b.append(big, small);
        // The visible text is two cryptic codes, so the accessible name has to
        // carry the meaning that sighted users get from the example word.
        b.setAttribute("aria-label",
            `${item.spr}, ARPAbet ${item.arpa}, as in ${item.eg}`);
        b.title = `${item.arpa} — as in "${item.eg}"`;
        b.addEventListener("click", () => insertPhone(item.spr));
    } else {
        b.textContent = item.label;
        if (item.mono) b.classList.add("chip-mono");
        if (item.hint) {
            b.title = item.hint;
            b.setAttribute("aria-label", `${item.label}, ${item.hint}`);
        }
        b.addEventListener("click", () => insertText(item.insert, item.label));
    }
    return b;
}

function buildPalette() {
    PALETTE.forEach((tab, i) => {
        const t = document.createElement("button");
        t.type = "button";
        t.className = "tab";
        t.id = `tab-${tab.id}`;
        t.setAttribute("role", "tab");
        t.textContent = tab.label;
        t.setAttribute("aria-controls", `panel-${tab.id}`);
        t.setAttribute("aria-selected", i === 0 ? "true" : "false");
        // Roving tabindex: one stop for the whole strip, arrows move within
        // it. Tabbing through 60 phone chips to reach the editor would be
        // hostile.
        t.tabIndex = i === 0 ? 0 : -1;
        t.addEventListener("click", () => selectTab(i));
        tabsEl.appendChild(t);

        const p = document.createElement("div");
        p.className = "panel";
        p.id = `panel-${tab.id}`;
        p.setAttribute("role", "tabpanel");
        p.setAttribute("aria-labelledby", `tab-${tab.id}`);
        p.tabIndex = 0;
        p.hidden = i !== 0;

        for (const g of tab.groups) {
            const wrap = document.createElement("div");
            wrap.className = "group";
            const legend = document.createElement("h3");
            legend.className = "micro";
            legend.textContent = g.legend;
            const row = document.createElement("div");
            // Mutually exclusive, NOT `chips` plus a modifier. Both classes
            // set `display`, so relying on source order to pick the winner
            // would break the moment the sheet is reordered.
            const grid = tab.kind === "phones" && g.kind !== "plain";
            row.className = grid ? "chips-phones" : "chips";
            for (const item of g.items) row.appendChild(chipFor(item));
            wrap.append(legend, row);
            p.appendChild(wrap);
        }
        panelsEl.appendChild(p);
    });

    tabsEl.addEventListener("keydown", (e) => {
        const keys = ["ArrowLeft", "ArrowRight", "Home", "End"];
        if (!keys.includes(e.key)) return;
        e.preventDefault();
        const tabs = [...tabsEl.querySelectorAll(".tab")];
        const cur = tabs.findIndex((x) => x.tabIndex === 0);
        let next = cur;
        if (e.key === "ArrowLeft") next = (cur - 1 + tabs.length) % tabs.length;
        else if (e.key === "ArrowRight") next = (cur + 1) % tabs.length;
        else if (e.key === "Home") next = 0;
        else next = tabs.length - 1;
        selectTab(next);
        tabs[next].focus();
    });
}

/// Collapse the palette away when you are just typing prose.
///
/// 43 phone chips are the right thing to have within reach and the wrong
/// thing to look at all day. The choice is remembered per machine.
///
/// ⚠ localStorage is wrapped: it THROWS, not returns null, in a webview with
/// site data blocked, and an exception here would abort the rest of start-up.
function setPaletteOpen(open) {
    paletteToggle.setAttribute("aria-expanded", open ? "true" : "false");
    paletteBody.hidden = !open;
    try { localStorage.setItem("spfy.palette", open ? "1" : "0"); } catch (_) { }
}

function restorePaletteState() {
    let open = true;
    try { open = localStorage.getItem("spfy.palette") !== "0"; } catch (_) { }
    setPaletteOpen(open);
}

function selectTab(index) {
    const tabs = [...tabsEl.querySelectorAll(".tab")];
    tabs.forEach((t, i) => {
        const on = i === index;
        t.setAttribute("aria-selected", on ? "true" : "false");
        t.tabIndex = on ? 0 : -1;
        $(`panel-${PALETTE[i].id}`).hidden = !on;
    });
}

// ---------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------

async function loadVoices() {
    voiceSelect.disabled = true;
    setStatus("Looking for voices…");
    try {
        const data = await invoke("list_voices");
        state.voices = data.voices || [];
        searchPathEl.textContent = data.search_path || "(no search path)";
        setFatal(null);
    } catch (err) {
        setFatal(String(err));
        setStatus("");
        return;
    }

    voiceSelect.innerHTML = "";
    if (state.voices.length === 0) {
        // Not an error: it is the normal first run. Say what to do about it.
        noVoicesEl.hidden = false;
        const opt = document.createElement("option");
        opt.textContent = "No voices installed";
        voiceSelect.appendChild(opt);
        voiceSelect.disabled = true;
        speakBtn.disabled = true;
        setStatus("No voices found.");
        return;
    }

    noVoicesEl.hidden = true;
    for (const v of state.voices) {
        const opt = document.createElement("option");
        opt.value = v.name;
        const size = v.bytes ? ` – ${(v.bytes / MB).toFixed(0)} MB` : "";
        opt.textContent = `${v.name} (${v.lang})${size}`;
        voiceSelect.appendChild(opt);
    }
    voiceSelect.disabled = false;
    speakBtn.disabled = false;
    const n = state.voices.length;
    setStatus(`${n} voice${n === 1 ? "" : "s"} ready.`);
}

// ---------------------------------------------------------------------
// Playback
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
        try { state.activeSource.stop(); } catch (_) { /* already ended */ }
        state.activeSource.disconnect();
        state.activeSource = null;
    }
    stopBtn.disabled = true;
    tallyEl.classList.remove("on");
}

async function speak() {
    if (state.busy) return;
    const voice = voiceSelect.value;
    const text = textInput.value.trim();
    if (!voice || !text) return;

    stopPlayback();
    state.busy = true;
    speakBtn.disabled = true;
    setStatus("Synthesizing…");

    const t0 = performance.now();
    let bytes;
    try {
        // Comes back as an ArrayBuffer: the Rust side returns a binary IPC
        // response rather than a JSON array of samples, so a 30-second
        // utterance is a memcpy and not a 240k-element JSON parse.
        const raw = await invoke("synth", { voice, text });
        bytes = new Uint8Array(raw);
    } catch (err) {
        setStatus("");
        setFatal(String(err));
        state.busy = false;
        speakBtn.disabled = false;
        return;
    }
    setFatal(null);

    state.lastWav = bytes;
    state.lastVoice = voice;
    saveBtn.disabled = false;

    const audio = ensureAudio();
    let buf;
    try {
        // decodeAudioData detaches the buffer it is given, so hand it a copy -
        // state.lastWav has to survive for Save.
        buf = await audio.decodeAudioData(bytes.slice().buffer);
    } catch (err) {
        setStatus("");
        setFatal(`The engine produced audio the player could not decode: ${err}`);
        state.busy = false;
        speakBtn.disabled = false;
        return;
    }

    const ms = Math.round(performance.now() - t0);
    setStatus(`Spoke ${buf.duration.toFixed(2)} s in ${ms} ms.`);

    const src = audio.createBufferSource();
    src.buffer = buf;
    src.connect(audio.destination);
    src.onended = () => {
        if (state.activeSource === src) stopPlayback();
    };
    src.start();
    state.activeSource = src;
    stopBtn.disabled = false;
    tallyEl.classList.add("on");
    state.busy = false;
    speakBtn.disabled = false;
}

// ---------------------------------------------------------------------
// Save
// ---------------------------------------------------------------------

async function saveWav() {
    if (!state.lastWav) return;
    let path;
    try {
        path = await save({
            defaultPath: `spfy_${state.lastVoice || "synth"}.wav`,
            filters: [{ name: "WAV audio", extensions: ["wav"] }],
        });
    } catch (err) {
        setFatal(String(err));
        return;
    }
    if (!path) return;            // user cancelled
    try {
        await invoke("save_wav", { path, bytes: Array.from(state.lastWav) });
        setStatus(`Saved ${path}`);
    } catch (err) {
        setFatal(String(err));
    }
}

// ---------------------------------------------------------------------
// Wire up
// ---------------------------------------------------------------------

// ---------------------------------------------------------------------
// Get voices
// ---------------------------------------------------------------------
//
// The GUI downloads nothing itself. `spfy_synth --list-available --json` and
// `--install-voice NAME` do the work, so the catalog fetch, the checksum and
// the unzip have ONE implementation shared with the CLI rather than a second
// one here that could quietly disagree with it.

const getDialog = $("get-dialog");
const getList = $("get-list");
const getStatus = $("get-status");

async function openGetVoices() {
    getList.innerHTML = "";
    getStatus.textContent = "Fetching the catalog…";
    getDialog.showModal();
    let data;
    try {
        data = await invoke("list_available");
    } catch (err) {
        getStatus.textContent = String(err);
        return;
    }
    const list = data.voices || [];
    getStatus.textContent = `${list.length} voice${list.length === 1 ? "" : "s"} available.`;

    for (const v of list) {
        const row = document.createElement("div");
        row.className = "getrow";

        const label = document.createElement("div");
        const name = document.createElement("strong");
        name.textContent = v.display || v.id;
        const sub = document.createElement("div");
        sub.className = "help";
        // ⚠ The size is shown BEFORE the click, not after. These are 66-232 MB
        // downloads and a bare "Get" invites someone on a metered connection
        // to find out the hard way.
        sub.textContent = v.installed
            ? `${v.lang} · installed`
            : `${v.lang} · ${(v.bytes / MB).toFixed(0)} MB download`;
        label.append(name, sub);

        const btn = document.createElement("button");
        if (v.installed) {
            btn.textContent = "Installed";
            btn.disabled = true;
        } else {
            btn.textContent = "Get";
            btn.setAttribute("aria-label",
                `Download ${v.display || v.id}, ${(v.bytes / MB).toFixed(0)} MB`);
            btn.addEventListener("click", async () => {
                btn.disabled = true;
                btn.textContent = "Getting…";
                // No percentage: spfy_synth writes its progress bar to stderr
                // and streaming a child's stderr live is a lot of machinery
                // for a number. Saying "Getting…" honestly beats a fake bar.
                sub.textContent = `${v.lang} · downloading, this can take a while`;
                try {
                    await invoke("install_voice", { id: v.id });
                    btn.textContent = "Installed";
                    sub.textContent = `${v.lang} · installed`;
                    await loadVoices();
                } catch (err) {
                    btn.disabled = false;
                    btn.textContent = "Get";
                    sub.textContent = String(err);
                }
            });
        }
        row.append(label, btn);
        getList.appendChild(row);
    }
}

$("get-btn").addEventListener("click", () => {
    openGetVoices().catch((err) => { getStatus.textContent = String(err); });
});
$("get-close").addEventListener("click", () => getDialog.close());

speakBtn.addEventListener("click", speak);
stopBtn.addEventListener("click", stopPlayback);
saveBtn.addEventListener("click", saveWav);
refreshBtn.addEventListener("click", loadVoices);

// Ctrl+Enter speaks from anywhere, Escape stops.
//
// ⚠ NOT bare Enter, which the WASM demo uses. There the textarea is a
// one-liner you throw a sentence into; here it is a full editor holding SSML,
// and a newline has to be a newline.
document.addEventListener("keydown", (e) => {
    if (e.key === "Enter" && (e.ctrlKey || e.metaKey)) {
        e.preventDefault();
        speak();
    } else if (e.key === "Escape") {
        stopPlayback();
    }
});

paletteToggle.addEventListener("click", () => {
    setPaletteOpen(paletteToggle.getAttribute("aria-expanded") !== "true");
});

buildPalette();
restorePaletteState();
textInput.value =
    "The National Weather Service has issued a severe thunderstorm warning.";
loadVoices();
