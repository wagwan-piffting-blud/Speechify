/* Synthesis Tracer - batch Viterbi/unit-selection view.
 * Powered by spfy_synth_trace.exe (our byte-exact reimplementation) via
 * POST /api/synth — no Frida, no Speechify.exe, concurrency-safe. */

function recColor(name) {
    let hash = 0;
    for (let i = 0; i < name.length; i++) hash = name.charCodeAt(i) + ((hash << 5) - hash);
    const h = ((hash % 360) + 360) % 360;
    return `hsl(${h}, 45%, 42%)`;
}
function recColorBright(name) {
    let hash = 0;
    for (let i = 0; i < name.length; i++) hash = name.charCodeAt(i) + ((hash << 5) - hash);
    const h = ((hash % 360) + 360) % 360;
    return `hsl(${h}, 55%, 62%)`;
}

// Synthesis is powered by spfy_synth_trace.exe (our byte-exact reimplementation)
// via POST /api/synth — no Frida, no Speechify.exe, and safe to run concurrently.

// -- Synthesis --
document.getElementById('synth-btn').addEventListener('click', async () => {
    const text = document.getElementById('synth-text').value.trim();
    if (!text) return;

    const btn = document.getElementById('synth-btn');
    btn.textContent = 'Synthesizing...';
    btn.disabled = true;

    document.getElementById('synth-timeline').innerHTML =
        '<p style="color:#787882;padding:20px">Running synthesis...</p>';
    document.getElementById('synth-details').innerHTML = '';
    document.getElementById('synth-audio').innerHTML = '';

    try {
        const resp = await fetch('/api/synth', {
            method: 'POST',
            headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({text})
        });
        const result = await resp.json();
        if (result.error) {
            document.getElementById('synth-timeline').innerHTML =
                `<p style="color:#e07070;padding:20px">Error: ${result.error}</p>`;
        } else {
            renderSynthResult(result, text);
        }
    } catch (e) {
        document.getElementById('synth-timeline').innerHTML =
            `<p style="color:#e07070;padding:20px">Error: ${e.message}</p>`;
    }
    btn.textContent = 'Synthesize';
    btn.disabled = false;
});

function renderSynthResult(result, text) {
    const units = result.wsola_units || [];
    const hps = result.pre_prune_hps || [];

    if (units.length === 0) {
        document.getElementById('synth-timeline').innerHTML =
            '<p style="color:#d4a55a;padding:20px">No units returned for this text.</p>';
        return;
    }

    // -- Compute stats --
    let recSwitches = 0, prevRec = null;
    const recCounts = {};
    const runs = [];
    let runLen = 1;

    units.forEach((u, i) => {
        if (prevRec !== null && u.rec_name !== prevRec) { recSwitches++; runs.push(runLen); runLen = 1; }
        else if (i > 0) runLen++;
        prevRec = u.rec_name;
        recCounts[u.rec_name] = (recCounts[u.rec_name] || 0) + 1;
    });
    runs.push(runLen);

    const avgRun = runs.reduce((a,b)=>a+b,0) / runs.length;
    const maxRun = Math.max(...runs);
    const ppWsMatch = hps.length > 0
        ? hps.filter((h,i) => i < units.length && h.uid === units[i].uid).length
        : 0;

    // Group halfphones into words using engine G2P phoneme output
    const wordGroups = groupHalfphonesIntoWords(units, result.word_phones || []);

    // -- Waveform + Audio player --
    const audioDiv = document.getElementById('synth-audio');
    audioDiv.innerHTML = '';

    if (result.wav_url) {
        // Create waveform canvas
        const wfContainer = document.createElement('div');
        wfContainer.style.cssText = 'background:var(--bg-light);border:1px solid var(--border);border-radius:4px;margin:0 16px 8px 16px;padding:4px';
        const canvas = document.createElement('canvas');
        canvas.id = 'synth-waveform';
        canvas.height = 120;
        wfContainer.appendChild(canvas);
        audioDiv.appendChild(wfContainer);

        // Audio element with play button
        const audioRow = document.createElement('div');
        audioRow.style.cssText = 'display:flex;align-items:center;gap:8px;margin-top:4px';

        const audio = document.createElement('audio');
        audio.src = result.wav_url;
        audio.style.display = 'none';
        audioRow.appendChild(audio);

        const playBtnEl = document.createElement('button');
        playBtnEl.textContent = '\u25B6';
        playBtnEl.style.padding = '6px 16px';
        playBtnEl.style.margin = '0 16px 8px 16px';
        AudioMgr._allBtns.add(playBtnEl);
        playBtnEl.addEventListener('click', () => AudioMgr.playManaged(audio, playBtnEl));
        audio.addEventListener('ended', () => { playBtnEl.textContent = '\u25B6 Play synthesis'; });
        audioRow.appendChild(playBtnEl);

        audioDiv.appendChild(audioRow);

        // Load and draw waveform from the synthesized WAV
        loadSynthWaveform(result.wav_url, canvas, units, audio, wordGroups);
    }

    // -- Timeline SVG --
    const BAR_W = Math.max(24, Math.min(52, 1400 / units.length));
    const BAR_H = 70;
    const LABEL_H = 22;
    const COST_H = 55;
    const GAP = 6;
    const totalW = units.length * BAR_W + 20;
    const svgH = BAR_H + LABEL_H + GAP + COST_H + 10;
    const maxCost = hps.length > 0 ? Math.max(...hps.map(h => h.total).filter(t => t < 90), 0.1) : 1;

    let svg = `<svg width="100%" viewBox="0 0 ${totalW} ${svgH}" preserveAspectRatio="none" style="font-family:'Cascadia Code','Fira Code',monospace;display:block">`;

    units.forEach((u, i) => {
        const x = i * BAR_W;
        const color = recColor(u.rec_name || '?');
        const isSwitch = i > 0 && u.rec_name !== units[i-1].rec_name;

        if (isSwitch) {
            svg += `<line x1="${x}" y1="0" x2="${x}" y2="${BAR_H}" stroke="#c0976f" stroke-width="2.5" opacity="0.8"/>`;
        }

        svg += `<rect x="${x+1}" y="2" width="${BAR_W-2}" height="${BAR_H-4}" fill="${color}" rx="2" opacity="0.85"/>`;

        // Phone label
        if (BAR_W >= 20) {
            const half = u.half === 0 ? '\u2081' : '\u2082';
            svg += `<text x="${x+BAR_W/2}" y="${BAR_H+15}" text-anchor="middle" fill="${phoneLabelColor(u.phone)}" font-size="${BAR_W >= 32 ? 11 : 9}" font-weight="500">${u.phone}${half}</text>`;
        }

        // Cost bar (below labels)
        if (i < hps.length && hps[i].total < 90) {
            const ch = (hps[i].total / maxCost) * COST_H;
            const costTop = BAR_H + LABEL_H + GAP;
            svg += `<rect x="${x+1}" y="${costTop + COST_H - ch}" width="${BAR_W-2}" height="${ch}" fill="#8aacb8" opacity="0.45" rx="1"/>`;
        }
    });

    svg += `<text x="0" y="${BAR_H + LABEL_H + GAP + 8}" fill="#606068" font-size="8">cost</text>`;
    svg += '</svg>';

    // -- Build full HTML --
    let html = '';

    // Summary banner
    html += `<div class="tracer-summary">
        <div class="tracer-stat"><span class="stat-val">${units.length}</span><span class="stat-label">Halfphones</span></div>
        <div class="tracer-stat"><span class="stat-val">${Object.keys(recCounts).length}</span><span class="stat-label">Recordings used</span></div>
        <div class="tracer-stat"><span class="stat-val">${recSwitches}</span><span class="stat-label">Rec switches</span></div>
        <div class="tracer-stat"><span class="stat-val">${avgRun.toFixed(1)}</span><span class="stat-label">Avg run length</span></div>
        <div class="tracer-stat"><span class="stat-val">${maxRun}</span><span class="stat-label">Max run</span></div>
        ${hps.length > 0 ? `<div class="tracer-stat"><span class="stat-val">${ppWsMatch}/${Math.min(hps.length,units.length)}</span><span class="stat-label">Pre-prune = Viterbi</span></div>` : ''}
    </div>`;

    // Timeline
    html += `<div style="overflow-x:auto;margin:8px 16px">${svg}</div>`;

    document.getElementById('synth-timeline').innerHTML = html;

    // -- Detail table --
    let detail = `<div class="tracer-legend">
        <h3>Unit Selection Detail</h3>
        <p class="legend-help">Each row is one halfphone selected by the Viterbi search. The engine picks from thousands of candidate units to minimize total cost.</p>
    </div>`;

    detail += `<div class="table-wrap" style="max-height:50vh">`;

    wordGroups.forEach((group, gi) => {
        // Word header (collapsible)
        const groupId = `word-group-${gi}`;
        detail += `<div class="word-group">
            <div class="word-header" onclick="var b=document.getElementById('${groupId}');b.classList.toggle('collapsed');document.getElementById('${groupId}-arrow').textContent=b.classList.contains('collapsed')?'\u25B6':'\u25BC'">
                <span class="word-header-arrow" id="${groupId}-arrow">\u25BC</span>
                <span class="word-header-text">${group.word}</span>
                <span class="word-header-phones">${group.phones.join(' ')}</span>
                <span class="word-header-meta">${group.units.length} halfphones</span>
            </div>
            <div id="${groupId}" class="word-group-body">
            <table class="tracer-table sortable"><thead><tr>
                <th data-sort="number" title="Halfphone index in synthesis order">#</th>
                <th data-sort="string" title="ARPAbet phone symbol">Phone</th>
                <th data-sort="number" title="First (1) or second (2) half of the phone">Half</th>
                <th data-sort="number" title="Internal unit ID in the VIN unit table">UID</th>
                <th data-sort="string" title="Source recording in the VDB corpus">Recording</th>
                <th data-sort="number" title="Local Position: sample offset within the recording (1 unit = 1ms, byte_offset = lp*8)">LP</th>
                <th data-sort="number" title="Duration-Like: overlap/window size for WSOLA concatenation (1 unit = 1ms)">DL</th>
                <th data-sort="number" title="Total unit selection cost (lower = better match to target prosody and context)">Cost</th>
                <th data-sort="number" title="Number of candidates considered before pruning">Cands</th>
                <th title="Flags: recording switches, pre-prune mismatches">Flags</th>
                <th title="Play this unit's audio segment from the VDB">Play</th>
            </tr></thead><tbody>`;

        // Build expected phone sequence for this word (2 halfphones per phone)
        const expectedPhones = [];
        group.phones.forEach(ph => { expectedPhones.push(ph + '.1'); expectedPhones.push(ph + '.2'); });

        group.units.forEach(({u, i}, localIdx) => {
            const hp = i < hps.length ? hps[i] : null;
            const isSwitch = i > 0 && u.rec_name !== units[i-1].rec_name;
            const ppMismatch = hp && hp.uid !== u.uid;

            // Check if selected phone matches the target
            const expected = localIdx < expectedPhones.length ? expectedPhones[localIdx] : null;
            const expectedPhone = expected ? expected.split('.')[0] : null;
            const phoneMismatch = expectedPhone && u.phone !== expectedPhone
                                  && u.phone !== 'pau' && expectedPhone !== 'pau';

            const flags = [];
            if (isSwitch) flags.push('<span class="flag-switch">REC-SW</span>');
            if (ppMismatch) flags.push('<span class="flag-mismatch">PP\u2260VIT</span>');
            if (phoneMismatch) flags.push(`<span class="flag-phone-diff" title="Target: ${expectedPhone}">tgt:${expectedPhone}</span>`);

            const color = recColor(u.rec_name || '?');
            const costStr = hp ? hp.total.toFixed(3) : '-';
            const candStr = hp ? hp.n_cand : '-';

            detail += `<tr class="${isSwitch ? 'row-switch' : ''}">
                <td data-sort-key="${i}">${i+1}</td>
                <td style="color:${phoneLabelColor(u.phone)}">${u.phone}</td>
                <td data-sort-key="${u.half}">${u.half === 0 ? '1st' : '2nd'}</td>
                <td class="mono" data-sort-key="${u.uid}">${u.uid}</td>
                <td><span class="rec-dot" style="background:${color}"></span>${u.rec_name || '?'}</td>
                <td class="mono" data-sort-key="${u.local_pos||0}" title="byte offset: ${(u.local_pos||0)*8}">${u.local_pos || 0}</td>
                <td class="mono" data-sort-key="${u.dur_like||0}" title="${((u.dur_like||0)*0.5).toFixed(1)}ms">${u.dur_like || 0}</td>
                <td class="mono ${hp && hp.total > maxCost * 0.8 ? 'cost-high' : ''}" data-sort-key="${hp ? hp.total : 99}">${costStr}</td>
                <td class="mono" data-sort-key="${hp ? hp.n_cand : 0}">${candStr}</td>
                <td>${flags.join(' ')}</td>
                <td class="play-cell" data-uid="${u.uid}"></td>
            </tr>`;

            if (ppMismatch && hp && hp.top && hp.top.length > 1) {
                detail += `<tr class="row-candidates"><td colspan="11">
                    <span class="cand-label">Top candidates:</span>
                    ${hp.top.map((c, ci) => {
                        const sel = c.uid === u.uid ? ' cand-selected' : '';
                        const best = ci === 0 ? ' cand-best' : '';
                        return `<span class="cand${sel}${best}" title="uid ${c.uid}">${c.uid} (${c.total.toFixed(3)})</span>`;
                    }).join(' ')}
                </td></tr>`;
            }
        });

        detail += '</tbody></table></div></div>';
    });

    detail += '</div>';

    // Recording usage breakdown
    const sortedRecs = Object.entries(recCounts).sort((a,b) => b[1] - a[1]);
    detail += `<details class="rec-usage" open><summary>Recording Usage (${Object.keys(recCounts).length} recordings)</summary>
        <div class="rec-usage-grid">`;
    sortedRecs.forEach(([name, count]) => {
        const pct = (count * 100 / units.length).toFixed(1);
        const barW = (count / sortedRecs[0][1]) * 100;
        detail += `<div class="rec-usage-row">
            <span class="rec-dot" style="background:${recColor(name)}"></span>
            <span class="rec-usage-name">${name}</span>
            <div class="rec-usage-bar" style="width:${barW}%;background:${recColor(name)}"></div>
            <span class="rec-usage-count">${count} (${pct}%)</span>
        </div>`;
    });
    detail += '</div></details>';

    document.getElementById('synth-details').innerHTML = detail;

    // Wire up play buttons for each unit
    document.querySelectorAll('.play-cell[data-uid]').forEach(td => {
        const uid = td.dataset.uid;
        if (uid && uid !== '-1' && uid !== 'undefined') {
            td.appendChild(playBtn(`/api/vdb/unit_audio/${uid}.wav`));
        }
    });
}

// Group halfphones into words using the engine's G2P phoneme output.
// Each phone in the .phn file = exactly 2 halfphones in the WSOLA trace.
// Total halfphones = sum(phones_per_word * 2) across all words.
function groupHalfphonesIntoWords(units, wordPhones) {
    if (!wordPhones || wordPhones.length === 0) {
        return [{
            word: '[all]',
            phones: [...new Set(units.map(u => u.phone))],
            units: units.map((u, i) => ({u, i})),
        }];
    }

    const groups = [];
    let unitIdx = 0;

    for (const wp of wordPhones) {
        const groupUnits = [];
        const gPhones = wp.phones.map(p => p.phone);

        // Each phone = 2 halfphones consumed sequentially
        const nHalfphones = wp.phones.length * 2;
        for (let j = 0; j < nHalfphones && unitIdx < units.length; j++) {
            groupUnits.push({u: units[unitIdx], i: unitIdx});
            unitIdx++;
        }

        groups.push({
            word: wp.word,
            phones: gPhones,
            units: groupUnits,
        });
    }

    // Any remaining
    if (unitIdx < units.length) {
        const remaining = [];
        while (unitIdx < units.length) {
            remaining.push({u: units[unitIdx], i: unitIdx});
            unitIdx++;
        }
        groups.push({
            word: '[trailing]',
            phones: [...new Set(remaining.map(r => r.u.phone))],
            units: remaining,
        });
    }

    return groups;
}

// Load synthesized WAV and draw waveform with recording-colored regions
async function loadSynthWaveform(wavUrl, canvas, units, audioElement, wordGroups) {
    try {
        const resp = await fetch(wavUrl);
        const arrayBuf = await resp.arrayBuffer();

        // Decode WAV: skip 44-byte header, read 16-bit LE samples
        const dataView = new DataView(arrayBuf);
        const sampleRate = dataView.getUint32(24, true);
        const dataOffset = 44;
        const numSamples = (arrayBuf.byteLength - dataOffset) / 2;
        const samples = [];
        for (let i = 0; i < numSamples; i++) {
            samples.push(dataView.getInt16(dataOffset + i * 2, true));
        }

        const w = canvas.parentElement.clientWidth - 8;
        const hasWords = wordGroups && wordGroups.length > 0;
        canvas.width = w;
        canvas.height = hasWords ? 150 : 120;
        const durSec = numSamples / sampleRate;

        // Compute per-unit time spans (approximate: equal division since we
        // don't have exact sample positions for the concatenated output)
        const unitDur = durSec / units.length;

        // Store state for cursor redraws
        const state = { canvas, samples, numSamples, sampleRate, units, unitDur, durSec, wordGroups: wordGroups || [], cursorTime: -1 };

        function draw() {
            const ctx = canvas.getContext('2d');
            const w = canvas.width;
            const h = canvas.height;
            const mid = h / 2;
            const maxVal = Math.max(1, ...samples.map(s => Math.abs(s)));

            ctx.fillStyle = '#1e1e24';
            ctx.fillRect(0, 0, w, h);

            // Draw recording-colored regions behind waveform (stop before word zone)
            const hasWordZone = state.wordGroups && state.wordGroups.length > 0;
            const colorBottom = hasWordZone ? h - 26 : h;
            let prevRec = null;
            units.forEach((u, i) => {
                const x1 = (i / units.length) * w;
                const x2 = ((i + 1) / units.length) * w;
                const isSilence = u.phone === 'pau' || u.phone === 'xx';
                const color = isSilence
                    ? 'rgba(100,100,110,0.12)'
                    : recColor(u.rec_name || '?').replace(')', ',0.25)').replace('hsl', 'hsla');
                ctx.fillStyle = color;
                ctx.fillRect(x1, 0, x2 - x1, colorBottom);

                // Recording switch line
                if (i > 0 && u.rec_name !== units[i-1].rec_name) {
                    ctx.strokeStyle = 'rgba(192,151,111,0.5)';
                    ctx.lineWidth = 1;
                    ctx.beginPath();
                    ctx.moveTo(x1, 0);
                    ctx.lineTo(x1, colorBottom);
                    ctx.stroke();
                }
            });

            // Center line
            ctx.strokeStyle = '#32323a';
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(0, mid);
            ctx.lineTo(w, mid);
            ctx.stroke();

            // Waveform
            ctx.strokeStyle = '#9ab8c4';
            ctx.lineWidth = 1;
            ctx.beginPath();
            const step = Math.max(1, Math.floor(samples.length / w));
            for (let px = 0; px < w; px++) {
                const sIdx = Math.floor((px / w) * samples.length);
                let minS = 0, maxS = 0;
                for (let j = sIdx; j < Math.min(sIdx + step, samples.length); j++) {
                    if (samples[j] < minS) minS = samples[j];
                    if (samples[j] > maxS) maxS = samples[j];
                }
                const y1 = mid - (maxS / maxVal) * mid * 0.9;
                const y2 = mid - (minS / maxVal) * mid * 0.9;
                ctx.moveTo(px, y1);
                ctx.lineTo(px, y2);
            }
            ctx.stroke();

            // Time labels
            ctx.fillStyle = '#606068';
            ctx.font = '9px sans-serif';
            const tickInterval = state.durSec > 5 ? 1 : state.durSec > 2 ? 0.5 : 0.2;
            for (let t = 0; t <= state.durSec + 0.001; t += tickInterval) {
                const x = (t / state.durSec) * w;
                ctx.fillText(t.toFixed(1), x + 2, h - 2);
            }

            // Word boundaries
            if (state.wordGroups && state.wordGroups.length > 0) {
                const wordTop = h - 24;

                // Separator line
                ctx.strokeStyle = '#2a2a32';
                ctx.lineWidth = 1;
                ctx.beginPath();
                ctx.moveTo(0, wordTop);
                ctx.lineTo(w, wordTop);
                ctx.stroke();

                let unitOffset = 0;
                const totalUnits = state.units.length;

                state.wordGroups.forEach(group => {
                    if (group.word === '[pause]' || group.word === '[trailing]') {
                        unitOffset += group.units.length;
                        return;
                    }

                    const x1 = (unitOffset / totalUnits) * w;
                    const x2 = ((unitOffset + group.units.length) / totalUnits) * w;

                    // Background
                    ctx.fillStyle = 'rgba(192,151,111,0.12)';
                    ctx.fillRect(x1, wordTop + 1, x2 - x1, 22);

                    // Boundary lines
                    ctx.strokeStyle = '#c0976f';
                    ctx.lineWidth = 1;
                    ctx.beginPath();
                    ctx.moveTo(x1, wordTop + 2);
                    ctx.lineTo(x1, wordTop + 20);
                    ctx.stroke();
                    ctx.beginPath();
                    ctx.moveTo(x2, wordTop + 2);
                    ctx.lineTo(x2, wordTop + 20);
                    ctx.stroke();

                    // Word text
                    ctx.font = 'bold 10px "Cascadia Code", monospace';
                    const tw = ctx.measureText(group.word).width;
                    const cx = (x1 + x2) / 2;
                    if (x2 - x1 > tw + 4) {
                        ctx.fillStyle = '#c0976f';
                        ctx.fillText(group.word, cx - tw / 2, wordTop + 15);
                    } else if (x2 - x1 > 10) {
                        ctx.fillStyle = '#c0976f';
                        ctx.font = '7px monospace';
                        ctx.fillText(group.word.substring(0, 4), x1 + 2, wordTop + 13);
                    }

                    unitOffset += group.units.length;
                });
            }

            // Playback cursor
            if (state.cursorTime >= 0 && state.cursorTime <= state.durSec) {
                const cx = (state.cursorTime / state.durSec) * w;
                ctx.strokeStyle = '#e8c87a';
                ctx.lineWidth = 2;
                ctx.beginPath();
                ctx.moveTo(cx, 0);
                ctx.lineTo(cx, h);
                ctx.stroke();
                ctx.fillStyle = '#e8c87a';
                ctx.beginPath();
                ctx.moveTo(cx - 4, 0);
                ctx.lineTo(cx + 4, 0);
                ctx.lineTo(cx, 6);
                ctx.closePath();
                ctx.fill();
            }
        }

        draw();

        // Attach playback cursor
        function updateCursor() {
            state.cursorTime = audioElement.currentTime;
            draw();
            if (!audioElement.paused && !audioElement.ended) {
                requestAnimationFrame(updateCursor);
            }
        }
        audioElement.addEventListener('play', () => requestAnimationFrame(updateCursor));
        audioElement.addEventListener('pause', () => { state.cursorTime = audioElement.currentTime; draw(); });
        audioElement.addEventListener('ended', () => { state.cursorTime = -1; draw(); });
        audioElement.addEventListener('seeked', () => { state.cursorTime = audioElement.currentTime; draw(); });

    } catch (e) {
        console.error('Synth waveform error:', e);
    }
}

