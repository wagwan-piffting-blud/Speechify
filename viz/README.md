# Speechify VIN/VDB Visualizer

A web-based tool for poking around the internals of SpeechWorks Speechify 3.0.5
voice files (`*.vin` / `*.vdb`) and watching the unit-selection engine do its
thing in real time. HTML/CSS/JS frontend, light Flask backend, optional Frida
integration for live synthesis tracing.

## What's in it

Five tabs, all sharing a voice selector in the top bar:

1. **VIN Explorer** — every chunk in the `.vin` file gets a dedicated viewer:
   - Unit table with pagination, phone filtering, and sortable columns
   - `feat` filename list (clickable, cross-links to VDB Explorer)
   - `mean` / `hist` statistics
   - `prsl` context-key lookup widget
   - `hash` lookup (join-cost hash table)
   - `ccos` summary
   - `cklx` word/syllable entries with group tabs and pagination
   - `ckls` file-id index
   - `LIST`/`INFO` metadata
2. **Trees** — SVG renderer for the `f0tr` (pitch) and `durt` (duration) CART
   trees. Leaf-count-weighted layout, viewBox pan/zoom, double-click to reset.
3. **VDB Explorer** — recording browser. Word search, sortable by
   name/duration/index, waveform view with phone-colored regions, word brackets,
   and a playback cursor.
4. **Synthesis Tracer** — type a sentence, watch the engine pick units.
   Timeline, waveform overlay, collapsible per-word halfphone detail tables,
   recording usage chart, per-candidate pruning flags. Batch view, now powered
   by `spfy_synth_trace.exe` (same binary as tab 5) — **no Frida, no
   `Speechify.exe`, concurrency-safe.** It carries every candidate's target
   cost, so the "pre-prune ≠ Viterbi" flag reflects genuine join-cost choices.
5. **Live Tracer** — streamed live from *our* byte-exact reimplementation
   (`spfy_synth_trace.exe`) instead of Frida, and animated as the physical
   story of concatenative synthesis. For each chosen unit the canvas shows the
   full **source recording** from the `.vdb`, trims it down to the little
   `[local_pos, local_pos+dur]` slice actually needed (closing brackets), then
   flies that slice down and stitches it into a growing, recording-colored
   **output waveform** you can play back. A supporting lattice strip shows the
   Viterbi candidate search (green = low cost, red = high, gold = the pick),
   and a ticker logs every core lookup. A **Speed** slider (0.1×–4×) scales the
   animation only; the engine and event stream always run full speed. An
   **Audio** toggle plays the real `.vdb` samples via the Web Audio API — *Blips*
   plays each cut slice as it lands, *Walkthrough* plays the whole source
   recording then the isolated slice (audio-driven pacing) — and the finished
   sentence auto-plays at the end. No Frida, no `Speechify.exe`.

Every tool has its own URL hash (`#vin-explorer`, `#trees`, `#vdb-explorer`,
`#synth-tracer`, `#live-tracer`) so you can bookmark a tab.

## Quick start (local / Windows dev machine)

```bash
pip install -r viz/requirements.txt
python viz/run_viz.py
```

Then open <http://localhost:5000>.

Voices are auto-discovered from `en-US/*/`. The default is `tom`. Switch voices
with the dropdown in the top bar.

Requirements:

- **Python 3.10+** (3.12 is what I use)
- **Flask 3.0+**
- **Frida** (Windows only, for the tracer — the rest works without it)
- The `en-US/` voice directory from the parent repo (e.g. `en-US/tom/tom.vin`
  and `en-US/tom/tom8.vdb`)

If Frida isn't installed, tabs 1–3 work fine; the Synthesis Tracer will tell
you it's unavailable.

## Synthesis tracing mode

Both tracers (tab 4 batch, tab 5 live) are now powered by
`bin/spfy_synth_trace.exe` — our own byte-exact reimplementation. There is
**nothing to attach**: type text and hit Synthesize / Run. No `Speechify.exe`,
no Frida, and because the trace binary is a standalone process you can run
several at once (unlike the single-instance Speechify server).

`POST /api/synth` (tab 4) runs the binary and reshapes its NDJSON event stream
into a batch payload (`wsola_units`, `pre_prune_hps`, `word_phones`); tab 5
streams the same events live over SSE. See **Live tracing mode** below for the
binary and build details.

> The old Frida path (attach to `Speechify.exe`, inject `viterbi_hook.js`, run
> `spfy_dumpwav.exe`) is retired for local use. The `frida_hooks/` code and the
> `/api/frida/*` routes remain only for the optional remote worker
> (`SYNTH_WORKER_URL`) deployment.

## Live tracing mode (spfy_synth_trace)

The **Live Tracer** tab does not touch `Speechify.exe` or Frida at all. It
streams events straight out of our own byte-exact engine reimplementation, so
you see the real unit-selection lookups as structured events with zero
reverse-engineering machinery in the loop.

**How it's wired:**

- `bin/spfy_synth_trace.exe` is `spfy_synth` compiled with `-DSPFY_TRACE=1`
  (a second CMake target over the *same* source — the shipped engine and the
  traced engine can never diverge). When run with `--trace-stream`, the
  instrumentation in `spfy/src/cli/spfy_synth.c` emits one NDJSON line per
  core lookup to stdout: `meta`, `phrase`, `slot`, `cand` (every candidate
  scored), `viterbi`, `pick`, `unit`, `done`.
- In the normal `spfy_synth` build (and the SAPI DLL) `SPFY_TRACE` is
  undefined, so every emit is a no-op macro that compiles away — the audio
  path stays **byte-identical and full-speed**. (Verified: `spfy_synth.exe`,
  `spfy_synth_trace.exe` with the sink off, and `spfy_synth_trace.exe
  --trace-stream` all produce the same SHA-256 WAV.)
- `app.py` exposes `GET /api/synth/stream?text=…&voice=…`, spawns the trace
  binary, and relays each NDJSON line to the browser over **SSE**, enriching
  `unit`/`pick` events with phone / half / recording name from the VIN.
- `live-tracer.js` receives events over `EventSource` into a queue, then plays
  them out on a `requestAnimationFrame` **act player** whose presentation clock
  = realTime × the **Speed** slider (0.1×–4×). Most acts are quick (a candidate
  dot, a pick); each `unit` act is a ~900 ms trim → fly → stitch animation on a
  `<canvas>`, using the source recording's waveform (fetched once per recording
  from `/api/vdb/waveform/<rec>` and cached). Two clocks: compute (C engine +
  relay, full speed, faithful) and presentation (browser-only, adjustable).
  Slowing the animation never slows synthesis — every event is already in the
  browser; we just reveal it slowly.
- Audio (Web Audio API): each recording is fetched once from
  `/api/vdb/audio/<rec>.wav` and decoded into an `AudioBuffer`; any slice is
  then played sample-accurately with `source.start(0, lp/1000, dur/1000)`. In
  *Walkthrough* mode the unit act is audio-driven (its phases are timed to the
  full-recording + slice playback, at natural pitch), so the Speed slider is
  bypassed for that act; *Blips* fires the slice at the cut moment while the
  animation stays on the presentation clock.

**Front-end (FE):** the viz binary is built with the **hosted (engine-exact)
FE** — the real `SWIttsFe-en-US.dll` driven through the `host_emu` software x86
CPU ("EMULATED DLL", 100% engine UID match). So the tracers phonemize exactly
like Speechify — e.g. `Monday` → `… d ey`, not the in-house FE's CMUdict-primary
`… d iy`. (The default `spfy\build.bat` builds the *in-house* pure-C FE, which
is what the ARM/WASM/Android targets use; that FE's `-day` weekday bug was also
fixed in `spfy/tools/cmudict_codegen.py`.)

**Rebuilding the binary** (after any engine change):

```bat
spfy\build_hosted.bat
```

That configures `SPFY_FE_HOSTED=ON`, builds `spfy_synth_trace.exe`, and copies
it into `bin/`. The Flask route auto-discovers the binary in `bin/`, then the
dev build dir, or `%SPFY_SYNTH_TRACE_EXE%` if set. It needs the voice's `.vin` /
`.vdb` / `.vcf` under `en-US/<voice>/`; the FE DLL, FE tables, and hpclass are
all embedded in the binary, so no server is required.

## Layout

```text
viz/
├── app.py                  Flask app: all API routes, voice loading, worker proxy
├── run_viz.py              Entry point (port 5000, host 0.0.0.0)
├── requirements.txt        flask, frida (win32)
├── worker.py               Standalone worker (superseded by monolith integration)
├── parsers/
│   ├── vin_parser.py       VIN RIFF parsing + all chunk decoders
│   └── vdb_parser.py       VDB indx + u-law audio extraction
├── frida_hooks/
│   ├── manager.py          FridaManager: attach/detach/synthesize lifecycle
│   └── viterbi_hook.js     Interceptor payload (prune / USEL / WSOLA hooks)
└── static/
    ├── index.html          SPA shell, tab nav, voice selector
    ├── css/main.css        Dark theme (warm muted palette)
    └── js/
        ├── app.js          Tab routing, fetch helpers, AudioMgr, sortable tables
        ├── vin-explorer.js
        ├── vdb-explorer.js
        ├── tree-viz.js
        ├── synth-tracer.js  Frida batch tracer (tab 4)
        ├── waveform.js
        └── live-tracer.js   SSE client + browser-paced event queue (tab 5)
```

No build step. Edit any file under `static/` and refresh the browser — the
Flask server serves `static/` with `Cache-Control: no-store` so hot-reload
just works.

## Troubleshooting

- **"Frida not available"** — `pip install frida` on the machine running the
  Flask app (or the remote worker, in remote mode).
- **Tracer shows results from a different synthesis** — make sure you're on a
  build that does the fresh detach/reattach per request. Concurrent jobs on
  `Speechify.exe` will otherwise contaminate the hook output.
- **404 on recording audio** — the VDB positional index is *not* the same as
  the VIN `feat` stored_id. All lookups go through recording *name*; if you
  see 404s, you're probably hitting an old code path that used the raw index.
- **VDB explorer shows wrong data for a recording** — double-check the voice
  dropdown. Voice data is cached per-name in `_voice_data`; restart the server
  if you've rebuilt a `.vin`/`.vdb` while it was running.

## Tips

- Every table header is sortable — click to toggle asc/desc.
- Every Play button uses a shared `AudioMgr` so starting one clip stops the
  previous one. The gold playback cursor stays on the waveform through
  pause/resume.
- URL hashes (`#vin`, `#trees`, `#vdb`, `#tracer`) are deep-linkable. Tools
  restore their last-viewed state on reload where it's cheap to do so.
- In the Tracer, collapse word groups you don't care about to keep the
  per-halfphone detail table manageable on long sentences.
