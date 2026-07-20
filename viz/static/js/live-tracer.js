/* Live Synthesis Tracer — concatenation animation
 * ------------------------------------------------
 * Streams core unit-selection events from spfy_synth_trace.exe over SSE and
 * animates the physical story of concatenative synthesis:
 *
 *   1. The engine searches candidate units per half-phone (the lattice strip).
 *   2. Viterbi picks a path.
 *   3. For each chosen unit, we show the FULL source recording from the .vdb,
 *      trim it down to the little [local_pos, local_pos+dur] slice actually
 *      needed (closing brackets), then fly that slice down and stitch it into
 *      a growing, recording-colored output waveform.
 *
 * Two clocks: COMPUTE (C engine + SSE relay — full speed, faithful) and
 * PRESENTATION (this file — a sequential act player advanced by presentation
 * time = realTime * speed). The speed slider (0.1x .. 4x) only scales the
 * presentation clock; synthesis is never slowed. Every event is already in
 * the browser — we just reveal it slowly and beautifully.
 *
 * Event stages (spfy/src/cli/spfy_synth.c): meta, phrase, slot, cand, viterbi,
 * pick, unit, done; Flask appends 'complete' with the wav_url.
 */
(function () {
    "use strict";

    // Palette mirrors css/main.css :root tokens (canvas can't read CSS vars).
    const PAL = {
        bg: "#1e1e24", bgl: "#26262e", card: "#2e2e38",
        accent: "#c0976f", accent2: "#8aacb8", text: "#d4d4d8",
        dim: "#787882", border: "#38383f", ok: "#7dae80", warn: "#d4a55a",
        wave: "#8aacb8", waveDim: "#3a4650",
    };

    const STAGE_H = 570;       // logical canvas height (px)
    const LAT_CANDS_H = 92;    // lattice candidate plot height
    const COST_MAX = 8.0;
    const TICKER_MAX = 260;

    // Presentation-time cost of each act, in ms (before the speed multiplier).
    const ACT_MS = {
        meta: 200, phrase: 500, slot: 10, cand: 5,
        viterbi: 450, pick: 26, unit: 900, done: 0, complete: 250,
    };

    const S = {
        es: null, queue: [], playing: false, streamDone: false,
        running: false, raf: 0, lastT: 0, acc: 0,
        act: null,                 // {stage, data, dur, el, start, tick, end}
        speed: 0.5,
        sampleRate: 8000,
        total: 0, shown: 0,
        // lattice
        slots: {},
        // concatenation stage
        recWave: {},               // rec_name -> {peaks, maxAbs, total, ready}
        anim: null,                // active unit animation {data, p}
        activeRec: null,           // rec_name currently shown up top
        outSlices: [],             // committed output slices
        outMaxSamp: 1,
        outputWave: null,          // real output samples (after 'complete')
        audio: null, audioPlaying: false,
        // Web Audio playback of the actual .vdb samples.
        actx: null, audioBuf: {}, audioBufLoading: {}, audioSources: [],
        audioMode: "blips",        // "off" | "blips" | "walk"
        audioVoice: null,
        seenRec: {},               // rec_name -> has its full block been played once
        runToken: 0,               // bumped on reset; invalidates stale scheduled audio
        skip: false,               // fast-forward: reveal everything instantly, no audio
        words: [],                 // word events {word, phones} — for output boundaries
        counts: { slots: 0, cands: 0, picks: 0, units: 0 },
        viterbiCost: null,
        phraseIdx: 0,
    };

    const $ = (id) => document.getElementById(id);

    function recHue(name) {
        let h = 0;
        for (let i = 0; i < (name || "").length; i++) h = name.charCodeAt(i) + ((h << 5) - h);
        return ((h % 360) + 360) % 360;
    }
    const recColorL = (n) => `hsl(${recHue(n)}, 48%, 46%)`;
    const recColorD = (n) => `hsl(${recHue(n)}, 40%, 30%)`;
    // Per-word palette — the output timeline is colored by WORD (not recording)
    // so it reads as words. Fixed hue cycle keeps adjacent words distinct;
    // starts purple → blue → green → amber …
    const WORD_HUES = [265, 210, 150, 35, 0, 320, 175, 95, 55, 235];
    const wHue = (i) => WORD_HUES[((i % WORD_HUES.length) + WORD_HUES.length) % WORD_HUES.length];
    const wordColorL = (i) => `hsl(${wHue(i)}, 52%, 56%)`;
    const wordColorD = (i) => `hsl(${wHue(i)}, 45%, 33%)`;
    function costColor(tc) {
        const t = Math.max(0, Math.min(1, tc / COST_MAX));
        return `hsl(${130 - t * 130}, 55%, 55%)`;
    }
    const lerp = (a, b, t) => a + (b - a) * t;
    const clamp01 = (t) => Math.max(0, Math.min(1, t));
    const easeInOut = (t) => t < 0.5 ? 2 * t * t : 1 - Math.pow(-2 * t + 2, 2) / 2;

    // -- source recording waveform cache ------------------------------------

    function ensureWave(name) {
        if (!name || name === "?" || S.recWave[name]) return;
        const voice = (document.getElementById("voice-select") || {}).value || "tom";
        S.recWave[name] = { peaks: null, maxAbs: 1, total: 1, ready: false };
        fetch(`/api/vdb/waveform/${encodeURIComponent(name)}?voice=${encodeURIComponent(voice)}`)
            .then((r) => r.ok ? r.json() : null)
            .then((d) => {
                if (!d || !d.samples) { S.recWave[name].failed = true; return; }
                const peaks = d.samples;
                let mx = 1;
                for (let i = 0; i < peaks.length; i++) { const a = Math.abs(peaks[i]); if (a > mx) mx = a; }
                S.recWave[name] = {
                    peaks, maxAbs: mx, total: d.total_samples || (peaks.length),
                    bucket: d.bucket_size || 1, downsampled: !!d.downsampled, ready: true,
                };
            })
            .catch(() => { if (S.recWave[name]) S.recWave[name].failed = true; });
    }

    // Extract the peak slice for [lpMs, lpMs+durMs] from a recording's peaks.
    function slicePeaks(w, lpMs, durMs) {
        if (!w || !w.ready || !w.peaks) return null;
        const s0 = lpMs * 8, s1 = (lpMs + durMs) * 8;
        if (w.downsampled) {
            const b = w.bucket || 1;
            const i0 = Math.max(0, Math.floor(s0 / b) * 2);
            const i1 = Math.min(w.peaks.length, Math.ceil(s1 / b) * 2);
            return w.peaks.slice(i0, i1);
        }
        return w.peaks.slice(Math.max(0, s0 | 0), Math.min(w.peaks.length, s1 | 0));
    }

    // -- canvas -------------------------------------------------------------

    let stage, ctx, dpr = 1, CW = 800, CH = STAGE_H;

    function resizeStage() {
        if (!stage) return;
        // Measure the canvas's own rendered box (governed by CSS width:100%)
        // and match the backing store to it exactly — so the drawing fills the
        // container precisely, never overshooting/clipping on the right. Width
        // is left entirely to CSS; we set only the backing resolution + height.
        const rect = stage.getBoundingClientRect();
        if (rect.width < 2) return;   // tab hidden — wait for the ResizeObserver
        CW = rect.width;
        CH = STAGE_H;
        dpr = window.devicePixelRatio || 1;
        stage.width = Math.round(CW * dpr);
        stage.height = Math.round(CH * dpr);
        stage.style.height = CH + "px";
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    }

    // Draw a waveform (sequence of sample/peak values) filled into a box.
    function drawWave(peaks, maxAbs, x, y, w, h, color) {
        if (!peaks || !peaks.length) return;
        const mid = y + h / 2, n = peaks.length;
        ctx.strokeStyle = color;
        ctx.lineWidth = 1;
        ctx.beginPath();
        const step = Math.max(1, Math.floor(n / Math.max(1, w)));
        for (let px = 0; px < w; px++) {
            const idx = Math.floor((px / w) * n);
            let mn = 0, mx = 0;
            for (let j = idx; j < Math.min(idx + step, n); j++) {
                const v = peaks[j];
                if (v < mn) mn = v; if (v > mx) mx = v;
            }
            const y1 = mid - (mx / maxAbs) * (h / 2) * 0.92;
            const y2 = mid - (mn / maxAbs) * (h / 2) * 0.92;
            ctx.moveTo(x + px, y1);
            ctx.lineTo(x + px, y2 + 0.5);
        }
        ctx.stroke();
    }

    function roundRect(x, y, w, h, r) {
        r = Math.min(r, w / 2, h / 2);
        ctx.beginPath();
        ctx.moveTo(x + r, y);
        ctx.arcTo(x + w, y, x + w, y + h, r);
        ctx.arcTo(x + w, y + h, x, y + h, r);
        ctx.arcTo(x, y + h, x, y, r);
        ctx.arcTo(x, y, x + w, y, r);
        ctx.closePath();
    }

    function drawStage() {
        if (!ctx) return;
        ctx.clearRect(0, 0, CW, CH);
        ctx.fillStyle = PAL.bg;
        ctx.fillRect(0, 0, CW, CH);

        const PAD = 10;
        const innerW = CW - PAD * 2;
        const srcY = 46, srcH = 168;
        const outY = 372, outH = 132;

        // ---- section labels ----
        ctx.textBaseline = "alphabetic";
        ctx.font = "11px 'Segoe UI', sans-serif";
        ctx.fillStyle = PAL.dim;
        ctx.fillText("SOURCE RECORDING  (the .vdb block we cut from)", PAD, 24);
        ctx.fillText("SYNTHESIZED OUTPUT  (chosen slices, stitched)", PAD, outY - 14);

        const wb = computeWordBounds();
        drawSource(PAD, srcY, innerW, srcH);
        drawTransit(PAD, srcY, innerW, srcH, outY, outH);
        drawOutput(PAD, outY, innerW, outH, wb);
        drawLegend(PAD, outY + outH + 16, innerW);
    }

    function drawSource(x, y, w, h) {
        // Card background.
        ctx.fillStyle = PAL.bgl;
        roundRect(x, y, w, h, 6); ctx.fill();
        ctx.strokeStyle = PAL.border; ctx.lineWidth = 1;
        roundRect(x, y, w, h, 6); ctx.stroke();

        const a = S.anim;
        const recName = a ? a.data.rec_name : S.activeRec;
        if (!recName || recName === "?") {
            ctx.fillStyle = PAL.dim;
            ctx.font = "13px 'Segoe UI', sans-serif";
            ctx.fillText(S.streamDone ? "" : "waiting for the first pick…", x + 14, y + h / 2);
            return;
        }
        const w0 = S.recWave[recName];
        const p = a ? a.p : 1;
        // Trim progress: brackets close over [0, 0.45] of the unit anim.
        const trim = a ? easeInOut(clamp01(p / 0.45)) : 1;

        // Header: recording name + phone + total duration. Measure the name
        // width WHILE the monospace font is active (before switching fonts),
        // otherwise the follow-on text overlaps the name.
        ctx.font = "600 13px 'Cascadia Code', monospace";
        ctx.fillStyle = recColorL(recName);
        ctx.fillText(recName, x + 12, y + 20);
        const nameW = ctx.measureText(recName).width;
        if (a) {
            ctx.font = "11px 'Segoe UI', sans-serif";
            ctx.fillStyle = PAL.text;
            const durTot = w0 && w0.total ? (w0.total / 8000).toFixed(2) + "s" : "";
            const sub = a.phase === "full"
                ? `▶ playing full recording…  ${durTot}`
                : `extracting ${a.data.phone || "?"}  ·  ${a.data.lp}–${a.data.lp + a.data.dur} ms  ${durTot ? "of " + durTot : ""}`;
            ctx.fillText(sub, x + 12 + nameW + 18, y + 20);
        }

        const wy = y + 34, wh = h - 46;
        // Full recording waveform (dimmed outside the trim window).
        if (w0 && w0.ready) {
            const totMs = w0.total / 8;
            // Walkthrough "play the whole block" phase: full waveform bright,
            // with a playhead sweeping it in time with the audio.
            if (a && a.phase === "full") {
                drawWave(w0.peaks, w0.maxAbs, x + 8, wy, w - 16, wh, PAL.wave);
                const px = x + 8 + clamp01(a.playhead || 0) * (w - 16);
                ctx.strokeStyle = PAL.warn; ctx.lineWidth = 2;
                ctx.beginPath(); ctx.moveTo(px, wy - 4); ctx.lineTo(px, wy + wh + 4); ctx.stroke();
                return;
            }
            const winX0 = a ? x + 8 + (a.data.lp / totMs) * (w - 16) : x + 8;
            const winX1 = a ? x + 8 + ((a.data.lp + a.data.dur) / totMs) * (w - 16) : x + w - 8;
            // Dimmed full waveform.
            drawWave(w0.peaks, w0.maxAbs, x + 8, wy, w - 16, wh, PAL.waveDim);
            // The kept slice, drawn bright — width interpolates as brackets close.
            if (a) {
                const cx0 = lerp(x + 8, winX0, trim);
                const cx1 = lerp(x + w - 8, winX1, trim);
                const sp = slicePeaks(w0, a.data.lp, a.data.dur);
                // Bright highlight band.
                ctx.save();
                ctx.beginPath(); ctx.rect(cx0, wy - 2, cx1 - cx0, wh + 4); ctx.clip();
                ctx.fillStyle = recColorD(recName) + "";
                ctx.globalAlpha = 0.22 + 0.25 * trim;
                ctx.fillRect(cx0, wy - 2, cx1 - cx0, wh + 4);
                ctx.globalAlpha = 1;
                if (sp) drawWave(sp, w0.maxAbs, cx0 + 1, wy, Math.max(2, cx1 - cx0 - 2), wh, recColorL(recName));
                else drawWave(w0.peaks, w0.maxAbs, x + 8, wy, w - 16, wh, recColorL(recName));
                ctx.restore();
                // Closing brackets.
                drawBracket(cx0, wy - 4, wh + 8, 1);
                drawBracket(cx1, wy - 4, wh + 8, -1);
            } else {
                drawWave(w0.peaks, w0.maxAbs, x + 8, wy, w - 16, wh, PAL.wave);
            }
        } else if (w0 && w0.failed) {
            ctx.fillStyle = PAL.dim; ctx.font = "12px 'Segoe UI'";
            ctx.fillText("(waveform unavailable)", x + 14, wy + wh / 2);
        } else {
            ctx.fillStyle = PAL.dim; ctx.font = "12px 'Segoe UI'";
            ctx.fillText("loading recording…", x + 14, wy + wh / 2);
        }
    }

    function drawBracket(x, y, h, dir) {
        ctx.strokeStyle = PAL.accent; ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.moveTo(x, y); ctx.lineTo(x, y + h);
        ctx.moveTo(x, y); ctx.lineTo(x + dir * 7, y);
        ctx.moveTo(x, y + h); ctx.lineTo(x + dir * 7, y + h);
        ctx.stroke();
    }

    // The slice flying from source window down to its output slot.
    function drawTransit(x, sy, sw, sh, oy, oh) {
        const a = S.anim;
        if (!a || a.p < 0.45) return;
        const fp = easeInOut(clamp01((a.p - 0.45) / 0.4));
        const rec = a.data.rec_name;
        const w0 = S.recWave[rec];
        if (!w0 || !w0.ready) return;
        const totMs = w0.total / 8;
        const srcX = x + 8 + (a.data.lp / totMs) * (sw - 16);
        const srcW = Math.max(6, (a.data.dur / totMs) * (sw - 16));
        const srcYmid = sy + 34 + (sh - 46) / 2;

        const tgtX = x + 8 + (a.data.t / S.outMaxSamp) * (sw - 16);
        const tgtW = Math.max(6, (a.data.dur * 8 / S.outMaxSamp) * (sw - 16));
        const tgtYmid = oy + oh / 2;

        const cx = lerp(srcX, tgtX, fp);
        const cy = lerp(srcYmid, tgtYmid, fp);
        const cw = lerp(srcW, tgtW, fp);
        const chh = lerp(sh - 46, oh, fp);

        ctx.save();
        ctx.globalAlpha = 0.9;
        // Glow.
        ctx.shadowColor = recColorL(rec);
        ctx.shadowBlur = 16 * (1 - fp) + 6;
        ctx.fillStyle = recColorD(rec);
        roundRect(cx, cy - chh / 2, cw, chh, 3); ctx.fill();
        ctx.shadowBlur = 0;
        const sp = slicePeaks(w0, a.data.lp, a.data.dur);
        if (sp) drawWave(sp, w0.maxAbs, cx + 1, cy - chh / 2 + 2, Math.max(2, cw - 2), chh - 4, recColorL(rec));
        ctx.restore();
    }

    // Word → output sample range, from the ENGINE's per-slice word tag: each
    // `unit` event carries `w` = the 0-based spoken-word index (aligned with the
    // `word` events), or -1 for pau/silence. Every slice locks its word at emit
    // time, so this is exact and STABLE as slices land — no re-derivation from
    // partial state, no half-phone counting, no proportional squishing.
    function computeWordBounds() {
        if (!S.words.length || !S.outSlices.length) return [];
        const ext = {};             // wordIdx -> {s0, s1}
        for (const sl of S.outSlices) {
            const wi = sl.wordIdx;
            if (wi == null || wi < 0) continue;
            const a = sl.t, b = sl.t + (sl.durSamp || 0);
            if (!ext[wi]) ext[wi] = { s0: a, s1: b };
            else { if (a < ext[wi].s0) ext[wi].s0 = a; if (b > ext[wi].s1) ext[wi].s1 = b; }
        }
        const out = [];
        for (let i = 0; i < S.words.length; i++) {
            if (ext[i]) out.push({ idx: i, text: S.words[i].word, s0: ext[i].s0, s1: ext[i].s1 });
        }
        return out;
    }

    function drawOutput(x, y, w, h, wb) {
        ctx.fillStyle = PAL.bgl;
        roundRect(x, y, w, h, 6); ctx.fill();
        ctx.strokeStyle = PAL.border; ctx.lineWidth = 1;
        roundRect(x, y, w, h, 6); ctx.stroke();

        const iw = w - 16, ix = x + 8, iy = y + 8, ih = h - 16;
        const total = S.outMaxSamp;

        // Committed slices, colored by WORD (silence/pau = neutral gray) so the
        // timeline reads as words. The thin seam lines still mark slice /
        // recording switches WITHIN each word.
        for (const sl of S.outSlices) {
            const bx = ix + (sl.t / total) * iw;
            const bw = Math.max(2, (sl.durSamp / total) * iw);
            const wi = (sl.wordIdx == null) ? -1 : sl.wordIdx;
            const dark  = wi < 0 ? "hsl(240, 6%, 32%)" : wordColorD(wi);
            const light = wi < 0 ? "hsl(240, 8%, 55%)" : wordColorL(wi);
            ctx.fillStyle = dark;
            ctx.globalAlpha = 0.55; ctx.fillRect(bx, iy, bw, ih); ctx.globalAlpha = 1;
            if (sl.peaks) drawWave(sl.peaks, sl.maxAbs, bx, iy, bw, ih, light);
            ctx.strokeStyle = PAL.bg; ctx.lineWidth = 1;
            ctx.beginPath(); ctx.moveTo(bx, iy); ctx.lineTo(bx, iy + ih); ctx.stroke();
        }

        // Word boundaries + labels (mirrors the Synthesis Tracer's waveform).
        for (let k = 0; k < wb.length; k++) {
            const b = wb[k];
            if (b.s0 == null) continue;
            const bx0 = ix + (b.s0 / total) * iw;
            const bx1 = ix + ((b.s1 != null ? b.s1 : total) / total) * iw;
            ctx.strokeStyle = "rgba(192,151,111,0.45)"; ctx.lineWidth = 1;
            ctx.beginPath(); ctx.moveTo(bx0, iy); ctx.lineTo(bx0, iy + ih); ctx.stroke();
            ctx.font = "600 11px 'Cascadia Code', monospace";
            const tw = ctx.measureText(b.text).width;
            const cx = (bx0 + bx1) / 2;
            if (bx1 - bx0 > tw + 10) {
                const lx = cx - tw / 2;
                ctx.fillStyle = "rgba(30,30,36,0.82)";
                roundRect(lx - 5, y + 3, tw + 10, 15, 3); ctx.fill();
                ctx.fillStyle = wordColorL(b.idx);
                ctx.fillText(b.text, lx, y + 14);
            } else if (bx1 - bx0 > 12) {
                ctx.fillStyle = "rgba(30,30,36,0.82)";
                roundRect(bx0 + 1, y + 3, 32, 15, 3); ctx.fill();
                ctx.fillStyle = wordColorL(b.idx);
                ctx.font = "9px 'Cascadia Code', monospace";
                ctx.fillText(b.text.slice(0, 4), bx0 + 4, y + 14);
            }
        }

        // Playhead during audio playback.
        if (S.audio && !S.audio.paused && S.audio.duration) {
            const ph = ix + (S.audio.currentTime / S.audio.duration) * iw;
            ctx.strokeStyle = PAL.warn; ctx.lineWidth = 2;
            ctx.beginPath(); ctx.moveTo(ph, y + 2); ctx.lineTo(ph, y + h - 2); ctx.stroke();
        }

        if (!S.outSlices.length) {
            ctx.fillStyle = PAL.dim; ctx.font = "12px 'Segoe UI'";
            ctx.fillText("output builds here as slices land…", ix + 6, y + h / 2);
        }
    }

    // Full word key, colored by the SAME index the timeline uses (so colors
    // always match, even while words are still landing).
    function drawLegend(x, y, w) {
        if (!S.words.length) return;
        ctx.font = "10px 'Cascadia Code', monospace";
        ctx.textBaseline = "middle";
        const maxRight = x + w;
        let cx = x, cy = y + 6, line = 0;
        ctx.fillStyle = PAL.dim;
        ctx.fillText("words:", cx, cy); cx += 46;
        for (let i = 0; i < S.words.length; i++) {
            const t = S.words[i].word;
            const tw = ctx.measureText(t).width;
            if (cx + 14 + tw + 16 > maxRight) {
                cx = x; cy += 16; line++;
                if (line >= 2) { ctx.fillStyle = PAL.dim; ctx.fillText(`…+${S.words.length - i} more`, cx, cy); break; }
            }
            ctx.fillStyle = wordColorL(i);
            roundRect(cx, cy - 5, 10, 10, 2); ctx.fill();
            cx += 14;
            ctx.fillStyle = PAL.text;
            ctx.fillText(t, cx, cy); cx += tw + 16;
        }
        ctx.textBaseline = "alphabetic";
    }

    // -- lattice (supporting strip) -----------------------------------------

    function hSlot(d) {
        const col = document.createElement("div");
        col.className = "lt-slot";
        const plot = document.createElement("div");
        plot.className = "lt-plot"; plot.style.height = LAT_CANDS_H + "px";
        col.appendChild(plot);
        const label = document.createElement("div");
        label.className = "lt-slot-label"; label.textContent = "·";
        col.appendChild(label);
        $("live-lattice").appendChild(col);
        col.scrollIntoView({ behavior: "smooth", inline: "end", block: "nearest" });
        S.slots[d.slot] = { col, plot, label, cands: {} };
        S.counts.slots++; setCounts();
    }
    function hCand(d) {
        S.counts.cands++;
        const slot = S.slots[d.slot];
        if (slot) {
            const dot = document.createElement("div");
            dot.className = "lt-cand";
            const tc = d.tc > 100 ? COST_MAX : d.tc;
            dot.style.top = Math.round(clamp01(tc / COST_MAX) * (LAT_CANDS_H - 7)) + "px";
            dot.style.background = costColor(tc);
            dot.title = `uid ${d.uid} · tc ${d.tc.toFixed(3)}`;
            slot.plot.appendChild(dot);
            slot.cands[d.uid] = dot;
        }
        if (S.counts.cands % 12 === 0)
            ticker("cand", ` ${S.counts.cands} candidates scored`);
        setCounts();
    }
    function hPick(d) {
        S.counts.picks++;
        const slot = S.slots[d.slot];
        if (slot) {
            slot.plot.classList.add("lt-resolved");
            const ch = slot.cands[d.uid];
            if (ch) ch.classList.add("lt-chosen");
            const half = d.half === 0 ? "₁" : "₂";
            slot.label.textContent = (d.phone || "?") + half;
            slot.label.classList.add("lt-slot-label-set");
        }
        ticker("pick", ` slot ${d.slot} →&nbsp;<b>${d.phone || "?"}</b>&nbsp;· uid ${d.uid} · ${d.rec_name || "?"}`);
        setCounts();
    }

    // -- misc UI ------------------------------------------------------------

    function ticker(stage, text) {
        const box = $("live-ticker"); if (!box) return;
        const line = document.createElement("div");
        line.className = "lt-line lt-" + stage;
        line.innerHTML = `<span class="lt-tag">${stage}</span>${text}`;
        box.appendChild(line);
        while (box.childElementCount > TICKER_MAX) box.removeChild(box.firstChild);
        box.scrollTop = box.scrollHeight;
    }
    function setCounts() {
        $("live-c-slots").textContent = S.counts.slots;
        $("live-c-cands").textContent = S.counts.cands;
        $("live-c-picks").textContent = S.counts.picks;
        $("live-c-units").textContent = S.counts.units;
        $("live-c-cost").textContent = S.viterbiCost == null ? "–" : S.viterbiCost.toFixed(1);
    }
    function setProgress() {
        const bar = $("live-progress-bar"); if (!bar) return;
        bar.style.width = (S.total ? Math.round((S.shown / S.total) * 100) : 0) + "%";
    }
    function setStatus(text, kind) {
        const el = $("live-status"); if (!el) return;
        el.textContent = text;
        el.className = "lt-status" + (kind ? " lt-status-" + kind : "");
    }
    function reset() {
        stopAllAudio();
        S.runToken++;              // invalidate any pending scheduled slice
        S.seenRec = {}; S.words = [];
        S.slots = {}; S.recWave = {}; S.anim = null; S.activeRec = null;
        S.outSlices = []; S.outMaxSamp = 1; S.outputWave = null;
        S.audio = null; S.audioPlaying = false;
        S.counts = { slots: 0, cands: 0, picks: 0, units: 0 };
        S.viterbiCost = null; S.total = 0; S.shown = 0; S.act = null; S.acc = 0;
        $("live-lattice").innerHTML = "";
        $("live-ticker").innerHTML = "";
        $("live-audio").innerHTML = "";
        $("live-viterbi").textContent = "";
        setCounts(); setProgress();
    }

    // -- act player (the presentation clock) --------------------------------

    function makeAct(ev) {
        const d = ev.data || {};
        const act = { stage: ev.stage, data: d, dur: ACT_MS[ev.stage] || 0, el: 0 };
        switch (ev.stage) {
            case "meta":
                act.start = () => { S.sampleRate = d.sample_rate || 8000;
                    ticker("meta", ` ${(d.n_units || 0).toLocaleString()} units · ${d.n_phrases} phrase(s) · ${d.sample_rate} Hz`); };
                break;
            case "phrase":
                act.start = () => { S.phraseIdx = d.idx;
                    ticker("phrase", ` #${d.idx} — ${d.n_hp} half-phones`); };
                break;
            case "slot": act.start = () => hSlot(d); break;
            case "cand": act.start = () => hCand(d); break;
            case "viterbi":
                act.start = () => { S.viterbiCost = d.total;
                    $("live-viterbi").textContent = `Viterbi path: cost ${d.total.toFixed(1)} over ${d.path_len} slots`;
                    ticker("viterbi", ` total ${d.total.toFixed(1)} · path ${d.path_len}/${d.n_slots}`); setCounts(); };
                break;
            case "pick": act.start = () => hPick(d); break;
            case "unit": {
                const uTick = ` ♪ cut&nbsp;<b>${d.phone || "?"}</b>&nbsp;from ${d.rec_name || "?"} [${d.lp}–${d.lp + d.dur} ms] ×${d.run}`;
                const mode = S.audioMode;
                const buf = S.audioBuf[d.rec_name];
                if (mode === "walk" && buf) {
                    // Audio-DRIVEN: play full recording (first appearance) → the
                    // isolated cut slice → fly + stitch. The act's timeline is
                    // real-time (natural pitch), independent of the Speed slider.
                    const firstApp = !S.seenRec[d.rec_name];
                    const recMs = firstApp ? buf.duration * 1000 : 0;
                    const gapMs = recMs > 0 ? 500 : 0;   // breath between block & slice
                    const sliceMs = Math.max(70, d.dur);
                    const flyMs = 420;
                    act.audio = true;
                    act.totalDur = recMs + gapMs + sliceMs + flyMs;
                    act.start = () => {
                        S.activeRec = d.rec_name; ensureWave(d.rec_name);
                        S.seenRec[d.rec_name] = 1;
                        S.anim = { data: d, p: 0, phase: recMs > 0 ? "full" : "slice", playhead: 0 };
                        ticker("unit", uTick);
                        const tok = S.runToken;
                        if (recMs > 0) playBuf(d.rec_name, 0, buf.duration);
                        act._t = setTimeout(() => {
                            if (tok === S.runToken) playBuf(d.rec_name, d.lp / 1000, sliceMs / 1000);
                        }, recMs + gapMs);
                    };
                    act.update = (ael) => {
                        if (!S.anim) return;
                        if (ael < recMs) {
                            S.anim.phase = "full"; S.anim.playhead = recMs ? ael / recMs : 0; S.anim.p = 0;
                        } else if (ael < recMs + gapMs) {
                            // silent breath: close the brackets onto the slice
                            S.anim.phase = "slice"; S.anim.p = 0.45 * clamp01((ael - recMs) / Math.max(1, gapMs));
                        } else if (ael < recMs + gapMs + sliceMs) {
                            S.anim.phase = "slice"; S.anim.p = 0.45;
                        } else {
                            S.anim.phase = "fly"; S.anim.p = 0.45 + 0.55 * clamp01((ael - recMs - gapMs - sliceMs) / flyMs);
                        }
                    };
                    act.end = () => { if (act._t) clearTimeout(act._t); commitSlice(d); S.anim = null; };
                } else {
                    // Time-driven (blips / off / audio-not-ready): animate at the
                    // slider speed; in blips mode fire the slice sound at the cut.
                    act.dur = ACT_MS.unit;
                    act.start = () => {
                        S.activeRec = d.rec_name; ensureWave(d.rec_name);
                        S.seenRec[d.rec_name] = 1;
                        S.anim = { data: d, p: 0, phase: "trim" };
                        ticker("unit", uTick);
                    };
                    act.tick = (p) => {
                        if (S.anim) S.anim.p = p;
                        if (mode === "blips" && !act.blipped && p >= 0.45) {
                            act.blipped = true;
                            playBuf(d.rec_name, d.lp / 1000, Math.max(0.07, d.dur / 1000));
                        }
                    };
                    act.end = () => { commitSlice(d); S.anim = null; };
                }
                break;
            }
            case "word":
                act.start = () => { S.words.push({ word: d.text, phones: d.phones || [] }); };
                break;
            case "complete": act.start = () => renderAudio(d.wav_url); break;
            case "done": break;
        }
        return act;
    }

    function commitSlice(d) {
        const w0 = S.recWave[d.rec_name];
        const run = d.run || 1;
        const durSamp = (d.dur || 0) * 8;
        const durMs = d.dur || 0;
        // `ws` = the engine's word index for EVERY slot of this WSOLA push
        // (-1 = silence). A run batched across a word boundary (e.g. the two
        // "tea"s of "TTS") therefore reports both words; split it into per-word
        // sub-slices so each half gets its own colour. Fallback to a single
        // word if `ws` is absent.
        const ws = (Array.isArray(d.ws) && d.ws.length) ? d.ws
                 : new Array(run).fill(d.w == null ? -1 : d.w);
        let k = 0;
        while (k < ws.length) {
            const wi = ws[k];
            let k2 = k; while (k2 < ws.length && ws[k2] === wi) k2++;
            const subT   = d.t + Math.round((k  / run) * durSamp);
            const subEnd = d.t + Math.round((k2 / run) * durSamp);
            const sl = {
                rec: d.rec_name, t: subT, durSamp: Math.max(1, subEnd - subT),
                run: (k2 - k), phone: d.phone || "?", wordIdx: wi,
                peaks: null, maxAbs: 1,
            };
            if (w0 && w0.ready) {
                sl.peaks = slicePeaks(w0, d.lp + (k / run) * durMs, ((k2 - k) / run) * durMs);
                sl.maxAbs = w0.maxAbs;
            }
            S.outSlices.push(sl);
            k = k2;
        }
        S.outMaxSamp = Math.max(S.outMaxSamp, d.t + durSamp);
        S.counts.units++; setCounts();   // count the original push once
    }

    function dispatchStart(act) {
        S.shown++;
        if (act.start) act.start();
        setProgress();
    }

    function step(dtPres) {
        S.acc += dtPres;
        let guard = 0;
        while (S.acc > 0 && guard++ < 200000) {
            if (!S.act) {
                if (!S.queue.length) break;
                S.act = makeAct(S.queue.shift());
                S.act.el = 0;
                dispatchStart(S.act);
                // Audio-driven acts (walkthrough units) advance on the real
                // clock via advanceAudioAct(), not the presentation budget.
                if (S.act.audio) { S.acc = 0; return; }
            }
            if (S.act.audio) { S.acc = 0; return; }
            const need = S.act.dur - S.act.el;
            if (S.acc >= need) {
                if (S.act.tick) S.act.tick(1);
                if (S.act.end) S.act.end();
                S.acc -= need;
                S.act = null;
            } else {
                S.act.el += S.acc;
                if (S.act.tick) S.act.tick(S.act.el / S.act.dur);
                S.acc = 0;
            }
        }
    }

    function idle() {
        return !(S.playing && (S.queue.length || S.act || !S.streamDone)) && !S.audioPlaying;
    }

    // Advance an audio-driven (walkthrough) act on the REAL clock — its phases
    // are timed to the audio, so the Speed slider doesn't apply here.
    function advanceAudioAct(act, dtReal) {
        act.ael = (act.ael || 0) + dtReal;
        if (act.update) act.update(act.ael);
        if (act.ael >= act.totalDur) {
            if (act.end) act.end();
            S.act = null;
        }
    }

    function frame(now) {
        S.raf = 0;
        const dt = S.lastT ? Math.min(64, now - S.lastT) : 16;
        S.lastT = now;
        if (S.playing) {
            if (S.skip) forceDrain();
            else if (S.act && S.act.audio) advanceAudioAct(S.act, dt);
            else step(dt * S.speed);
        }
        drawStage();
        if (idle()) {
            S.running = false; S.lastT = 0;
            if (S.streamDone && !S.queue.length && !S.act) {
                setStatus(`Done — ${S.counts.units} units stitched from ${countRecs()} recordings`, "ok");
                const btn = $("live-run-btn"); btn.disabled = false; btn.textContent = "Run Live Trace";
            }
        } else {
            S.raf = requestAnimationFrame(frame);
        }
    }
    function countRecs() { const s = {}; S.outSlices.forEach((x) => s[x.rec] = 1); return Object.keys(s).length; }
    function kick() { if (!S.running) { S.running = true; S.lastT = 0; S.raf = requestAnimationFrame(frame); } }

    // -- web audio: play real .vdb samples ----------------------------------

    // Create/resume the AudioContext. MUST be called from a user gesture
    // (the Run click or an Audio-mode click), or browsers keep it suspended.
    function ensureAudioCtx() {
        if (!S.actx) {
            const AC = window.AudioContext || window.webkitAudioContext;
            if (AC) S.actx = new AC();
        }
        if (S.actx && S.actx.state === "suspended") S.actx.resume();
        return S.actx;
    }

    // Fetch + decode a recording's full audio into an AudioBuffer (cached).
    // Any slice is then played from this one buffer — no per-unit fetches.
    function ensureAudioBuf(name) {
        if (!name || name === "?" || !S.actx) return;
        if (S.audioBuf[name] || S.audioBufLoading[name]) return;
        S.audioBufLoading[name] = true;
        const voice = (document.getElementById("voice-select") || {}).value || "tom";
        fetch(`/api/vdb/audio/${encodeURIComponent(name)}.wav?voice=${encodeURIComponent(voice)}`)
            .then((r) => r.ok ? r.arrayBuffer() : null)
            .then((ab) => ab ? S.actx.decodeAudioData(ab) : null)
            .then((buf) => { if (buf) S.audioBuf[name] = buf; })
            .catch(() => {})
            .finally(() => { delete S.audioBufLoading[name]; });
    }

    // Prefetch decoded audio for every recording still queued / on screen —
    // used when the user switches audio on mid-run.
    function prefetchQueuedAudio() {
        if (!S.actx) return;
        const names = new Set();
        S.queue.forEach((e) => { if (e.data && e.data.rec_name) names.add(e.data.rec_name); });
        S.outSlices.forEach((s) => names.add(s.rec));
        if (S.anim) names.add(S.anim.data.rec_name);
        names.forEach((n) => ensureAudioBuf(n));
    }

    // Play [offsetSec, offsetSec+durSec] of a recording, with short fades to
    // avoid edge clicks. Tracked so reset()/pause can stop everything.
    function playBuf(name, offsetSec, durSec) {
        if (S.skip) return;   // no per-unit sounds when fast-forwarding
        const buf = S.audioBuf[name];
        if (!buf || !S.actx) return;
        try {
            const src = S.actx.createBufferSource();
            src.buffer = buf;
            const g = S.actx.createGain();
            src.connect(g); g.connect(S.actx.destination);
            const t = S.actx.currentTime;
            const d = Math.max(0.03, durSec);
            const off = Math.max(0, Math.min(offsetSec, buf.duration - 0.01));
            g.gain.setValueAtTime(0.0001, t);
            g.gain.exponentialRampToValueAtTime(1, t + 0.010);
            g.gain.setValueAtTime(1, t + Math.max(0.02, d - 0.02));
            g.gain.exponentialRampToValueAtTime(0.0001, t + d);
            src.start(t, off, d);
            S.audioSources.push(src);
            src.onended = () => { const i = S.audioSources.indexOf(src); if (i >= 0) S.audioSources.splice(i, 1); };
        } catch (e) {}
    }
    function stopAllAudio() {
        S.audioSources.forEach((s) => { try { s.stop(); } catch (e) {} });
        S.audioSources = [];
    }

    // -- final-output audio element -----------------------------------------

    function renderAudio(wavUrl) {
        const box = $("live-audio"); box.innerHTML = "";
        if (!wavUrl) return;

        const audio = document.createElement("audio");
        audio.src = wavUrl; audio.style.display = "none";
        S.audio = audio;
        const btn = document.createElement("button");
        btn.textContent = "▶ Play synthesis"; btn.className = "lt-play";
        btn.addEventListener("click", () => {
            if (typeof AudioMgr !== "undefined") AudioMgr.playManaged(audio, btn);
            else (audio.paused ? audio.play() : audio.pause());
        });
        if (typeof AudioMgr !== "undefined") AudioMgr._allBtns.add(btn);
        audio.addEventListener("play", () => { S.audioPlaying = true; btn.textContent = "⏸ Pause"; kick(); });
        audio.addEventListener("pause", () => { S.audioPlaying = false; });
        audio.addEventListener("ended", () => { S.audioPlaying = false; btn.textContent = "▶ Play synthesis"; });
        box.appendChild(audio); box.appendChild(btn);
        ticker("done", ` render complete — ${S.counts.units} units, ${S.counts.cands} candidates scored`);

        // Finale: auto-play the stitched sentence when audio is enabled (the
        // Run click already unlocked playback). Small delay so the last
        // walkthrough/blips slice isn't stepped on. Skipped on fast-forward.
        if (S.audioMode !== "off" && !S.skip) {
            setTimeout(() => {
                try {
                    if (typeof AudioMgr !== "undefined") AudioMgr.playManaged(audio, btn);
                    else audio.play();
                } catch (e) {}
            }, 220);
        }
    }

    // -- lifecycle ----------------------------------------------------------

    function start() {
        if (S.es) { try { S.es.close(); } catch (e) {} S.es = null; }
        const text = ($("live-text").value || "").trim();
        if (!text) return;
        const voice = (document.getElementById("voice-select") || {}).value || "tom";

        // Unlock audio on this user gesture; drop decoded buffers if the voice
        // changed (a new corpus means new recordings).
        ensureAudioCtx();
        if (voice !== S.audioVoice) { S.audioBuf = {}; S.audioBufLoading = {}; S.audioVoice = voice; }

        reset();
        S.playing = true; S.streamDone = false; S.skip = false;
        $("live-run-btn").disabled = true; $("live-run-btn").textContent = "Streaming…";
        $("live-pause-btn").textContent = "⏸ Pause";
        setStatus("Synthesizing — streaming events…", "run");

        const es = new EventSource(`/api/synth/stream?text=${encodeURIComponent(text)}&voice=${encodeURIComponent(voice)}`);
        S.es = es;
        es.onmessage = (e) => {
            let obj; try { obj = JSON.parse(e.data); } catch (err) { return; }
            S.total++;
            // Prefetch source waveforms (and, if audio is on, decoded audio)
            // the moment a recording is named — well before the paced
            // animation reaches it.
            if ((obj.stage === "unit" || obj.stage === "pick") && obj.data && obj.data.rec_name) {
                ensureWave(obj.data.rec_name);
                if (S.audioMode !== "off") ensureAudioBuf(obj.data.rec_name);
            }
            S.queue.push(obj);
            if (obj.stage === "complete" || obj.stage === "error") {
                S.streamDone = true; try { es.close(); } catch (err) {} S.es = null;
            }
            kick();
        };
        es.onerror = () => {
            S.streamDone = true; try { es.close(); } catch (err) {} S.es = null;
            if (S.total === 0) {
                setStatus("Stream failed — is spfy_synth_trace.exe built? (spfy/build.bat)", "err");
                $("live-run-btn").disabled = false; $("live-run-btn").textContent = "Run Live Trace";
                S.playing = false;
            }
            kick();
        };
        kick();
    }

    function togglePause() {
        S.playing = !S.playing;
        $("live-pause-btn").textContent = S.playing ? "⏸ Pause" : "▶ Resume";
        if (!S.playing) {
            stopAllAudio();
            if (S.act && S.act._t) { clearTimeout(S.act._t); S.act._t = null; }
        }
        if (S.playing) kick(); else drawStage();
    }
    // Fire every queued act instantly (commits slices, keeps the visuals);
    // audio acts are force-ended rather than played out.
    function forceDrain() {
        let guard = 0;
        while ((S.queue.length || S.act) && guard++ < 500000) {
            if (S.act && S.act.audio) { if (S.act.end) S.act.end(); S.act = null; }
            else step(1e9);
        }
    }
    function skipToEnd() {
        stopAllAudio();
        S.runToken++;   // cancel any scheduled walkthrough slice
        const active = S.es || S.running || S.queue.length || S.act;
        if (!active) {
            // Nothing running: start a fresh trace, then fast-forward it. start()
            // sets skip=false and schedules the frame loop via rAF, which runs
            // AFTER this returns — so setting skip here makes the first frame
            // fast-forward. Events still arrive over SSE; the loop drains each
            // batch instantly until the stream completes.
            start();
            S.skip = true;
            return;
        }
        S.skip = true;
        S.playing = true;   // Skip implies "run to the end" even if paused
        $("live-pause-btn").textContent = "⏸ Pause";
        forceDrain();
        kick();
    }

    async function probe() {
        try {
            const s = await (await fetch("/api/synth/stream/status")).json();
            if (s.available) setStatus("Ready — spfy_synth_trace.exe found", "ok");
            else setStatus("spfy_synth_trace.exe not built. Run spfy/build.bat.", "err");
        } catch (e) { setStatus("Backend unreachable", "err"); }
    }

    function wire() {
        stage = $("live-stage");
        if (!stage) return;
        ctx = stage.getContext("2d");
        resizeStage();
        window.addEventListener("resize", () => { resizeStage(); drawStage(); });
        // The tab starts hidden (display:none → clientWidth 0), so the canvas
        // can't be measured at load. A ResizeObserver re-measures and redraws
        // the instant the tab is shown (and on any later resize) — this is what
        // makes the stage fill the full available width instead of the 360px
        // fallback it was stuck at.
        if (window.ResizeObserver) {
            new ResizeObserver(() => { resizeStage(); drawStage(); }).observe(stage.parentElement);
        }
        drawStage();

        $("live-run-btn").addEventListener("click", start);
        $("live-text").addEventListener("keydown", (e) => { if (e.key === "Enter") start(); });
        $("live-pause-btn").addEventListener("click", togglePause);
        $("live-instant-btn").addEventListener("click", skipToEnd);
        const sp = $("live-speed");
        const showSpeed = () => { S.speed = parseFloat(sp.value) || 0.5; $("live-speed-val").textContent = S.speed.toFixed(2) + "×"; };
        sp.addEventListener("input", showSpeed);
        showSpeed();

        // Audio-mode segmented control (Off / Blips / Walk). Switching applies
        // to subsequent units, so you can toggle it live mid-run.
        document.querySelectorAll(".lt-aud-btn").forEach((b) => {
            b.classList.toggle("active", b.dataset.aud === S.audioMode);
            b.addEventListener("click", () => {
                ensureAudioCtx();   // this click is a user gesture
                S.audioMode = b.dataset.aud;
                document.querySelectorAll(".lt-aud-btn").forEach((x) => x.classList.toggle("active", x === b));
                if (S.audioMode !== "off") prefetchQueuedAudio();
            });
        });

        probe();
    }

    if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", wire);
    else wire();
})();
