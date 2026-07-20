"""
Speechify VIN/VDB Visualizer - Flask Backend
Run: python viz/app.py
"""
import os
import sys
import time
import json
import subprocess
import platform

# Add project root to path
PROJ_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, PROJ_ROOT)

import re
import struct
from flask import Flask, jsonify, request, send_from_directory, Response
from viz.parsers import vin_parser, vdb_parser

app = Flask(__name__, static_folder='static')

# -- Voice data cache --
_voice_data = {}


def _get_voice(name="tom"):
    """Load and cache VIN/VDB data for a voice."""
    if name in _voice_data:
        return _voice_data[name]

    voice_dir = os.path.join(PROJ_ROOT, "en-US", name)
    vin_path = os.path.join(voice_dir, f"{name}.vin")
    # VDB naming: tom8.vdb, mara8.vdb, etc.
    vdb_candidates = [
        os.path.join(voice_dir, f"{name}8.vdb"),
        os.path.join(voice_dir, f"{name}.vdb"),
    ]
    vdb_path = None
    for p in vdb_candidates:
        if os.path.exists(p):
            vdb_path = p
            break

    if not os.path.exists(vin_path):
        return None

    print(f"  Loading voice '{name}': {vin_path}")
    vin_plain = vin_parser.load_vin(vin_path)
    chunks = vin_parser.list_chunks(vin_plain)
    n_units, unit_data = vin_parser.parse_unit_table(vin_plain)

    vdb_plain = None
    vdb_info = None
    if vdb_path:
        print(f"  Loading VDB: {vdb_path}")
        vdb_plain = vdb_parser.load_vdb(vdb_path)
        vdb_info = vdb_parser.parse_vdb(vdb_plain)

    data = {
        "name": name,
        "vin_plain": vin_plain,
        "chunks": chunks,
        "n_units": n_units,
        "unit_data": unit_data,
        "vdb_plain": vdb_plain,
        "vdb_info": vdb_info,
    }
    _voice_data[name] = data
    print(f"  Loaded: {n_units:,} units, {vdb_info['n_recordings'] if vdb_info else 0} recordings")
    return data


def _voice():
    """Get current voice from query param, default 'tom'."""
    name = request.args.get("voice", "tom")
    v = _get_voice(name)
    if v is None:
        return None, name
    return v, name


# -- Static file serving (no-cache for hot reload) --

@app.route('/')
def index():
    return send_from_directory(app.static_folder, 'index.html',
                               max_age=0)

@app.route('/static/<path:path>')
def serve_static(path):
    resp = send_from_directory(app.static_folder, path)
    resp.headers['Cache-Control'] = 'no-cache, no-store, must-revalidate'
    resp.headers['Pragma'] = 'no-cache'
    resp.headers['Expires'] = '0'
    return resp


# -- API: Voices --

@app.route('/api/voices')
def api_voices():
    voice_root = os.path.join(PROJ_ROOT, "en-US")
    voices = []
    if os.path.exists(voice_root):
        for d in sorted(os.listdir(voice_root)):
            vin = os.path.join(voice_root, d, f"{d}.vin")
            if os.path.exists(vin):
                voices.append({"name": d, "vin": f"en-US/{d}/{d}.vin"})
    return jsonify(voices)


SWITTS_CONFIG = os.path.join(PROJ_ROOT, "config", "SWIttsConfig.xml")
_VOICE_NAME_RE = re.compile(
    r'(<param\s+name="tts\.voice\.name">\s*<value>\s*)([^<\s]+)(\s*</value>)'
)


def _read_config_voice():
    """Return the current tts.voice.name from SWIttsConfig.xml, or None."""
    try:
        with open(SWITTS_CONFIG, 'r', encoding='utf-8') as f:
            m = _VOICE_NAME_RE.search(f.read())
        return m.group(2) if m else None
    except FileNotFoundError:
        return None


def _write_config_voice(name):
    """Update tts.voice.name in SWIttsConfig.xml, preserving formatting."""
    if not os.path.exists(SWITTS_CONFIG):
        return {"error": f"Config not found: {SWITTS_CONFIG}"}
    with open(SWITTS_CONFIG, 'r', encoding='utf-8') as f:
        content = f.read()
    new_content, n = _VOICE_NAME_RE.subn(rf'\g<1>{name}\g<3>', content)
    if n == 0:
        return {"error": "tts.voice.name param not found in config"}
    with open(SWITTS_CONFIG, 'w', encoding='utf-8') as f:
        f.write(new_content)
    return {"ok": True, "voice": name}


@app.route('/api/voices/current')
def api_voices_current():
    return jsonify({"voice": _read_config_voice()})


@app.route('/api/voices/select', methods=['POST'])
def api_voices_select():
    """Update SWIttsConfig.xml and restart Speechify.exe so the engine picks
    up the new voice. Speechify reads .vin/.vdb only at startup, so a restart
    is mandatory on voice change.
    """
    body = request.get_json(force=True) or {}
    name = (body.get('name') or '').strip()
    if not name:
        return jsonify({"error": "No voice name provided"}), 400

    vin_path = os.path.join(PROJ_ROOT, "en-US", name, f"{name}.vin")
    if not os.path.exists(vin_path):
        return jsonify({"error": f"Voice '{name}' not found at {vin_path}"}), 404

    cfg_result = _write_config_voice(name)
    if "error" in cfg_result:
        return jsonify(cfg_result), 500

    mgr = _get_frida_mgr()
    restart_result = None
    if mgr.attached or mgr._is_running():
        restart_result = mgr.restart()
        if restart_result and "error" in restart_result:
            return jsonify({
                "ok": False,
                "voice": name,
                "config_updated": True,
                "restart": restart_result,
                "error": restart_result["error"],
            }), 500

    return jsonify({
        "ok": True,
        "voice": name,
        "config_updated": True,
        "restart": restart_result,
    })


# -- API: VIN --

@app.route('/api/vin/chunks')
def api_vin_chunks():
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    return jsonify(v["chunks"])


@app.route('/api/vin/list_info')
def api_vin_list_info():
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    return jsonify(vin_parser.parse_list_info(v["vin_plain"]))


@app.route('/api/vin/vers')
def api_vin_vers():
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    return jsonify({"version": vin_parser.parse_vers(v["vin_plain"])})


@app.route('/api/vin/cnts')
def api_vin_cnts():
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    return jsonify(vin_parser.parse_cnts(v["vin_plain"]))


@app.route('/api/vin/units')
def api_vin_units():
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    page = request.args.get("page", 0, type=int)
    per_page = request.args.get("per_page", 100, type=int)
    phone = request.args.get("phone", None)
    fidx = request.args.get("fidx", None, type=int)
    result = vin_parser.get_units_page(v["unit_data"], page, per_page, phone, fidx)
    # Enrich with recording names
    if "filenames" not in v:
        v["filenames"] = vin_parser.parse_feat_filenames(v["vin_plain"])
    fns = v["filenames"]
    for item in result["items"]:
        item["rec_name"] = fns.get(item["file_idx"], "?")
    return jsonify(result)


@app.route('/api/vin/unit/<int:uid>')
def api_vin_unit(uid):
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    unit = vin_parser.get_unit(v["unit_data"], uid)
    if unit is None:
        return jsonify({"error": "Unit not found"}), 404
    return jsonify(unit)


@app.route('/api/vin/feat/filenames')
def api_vin_filenames():
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404

    # Cache filenames
    if "filenames" not in v:
        v["filenames"] = vin_parser.parse_feat_filenames(v["vin_plain"])

    q = request.args.get("q", "").lower()
    page = request.args.get("page", 0, type=int)
    per_page = request.args.get("per_page", 50, type=int)

    items = [(sid, name) for sid, name in sorted(v["filenames"].items())]
    if q:
        items = [(sid, n) for sid, n in items if q in n.lower()]

    total = len(items)
    start = page * per_page
    page_items = [{"stored_id": sid, "name": n} for sid, n in items[start:start+per_page]]
    return jsonify({"items": page_items, "total": total, "page": page})


@app.route('/api/vin/f0tr')
def api_vin_f0tr():
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    if "f0tr" not in v:
        v["f0tr"] = vin_parser.parse_f0tr_tree(v["vin_plain"])
    return jsonify(v["f0tr"])


@app.route('/api/vin/durt')
def api_vin_durt():
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    if "durt" not in v:
        v["durt"] = vin_parser.parse_durt_trees(v["vin_plain"])
    return jsonify(v["durt"])


@app.route('/api/vin/durt/<phone>')
def api_vin_durt_phone(phone):
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    if "durt" not in v:
        v["durt"] = vin_parser.parse_durt_trees(v["vin_plain"])
    for tree in v["durt"]["trees"]:
        if tree["phone"] == phone:
            return jsonify(tree)
    return jsonify({"error": f"Phone '{phone}' not found"}), 404


@app.route('/api/vin/hash/stats')
def api_vin_hash_stats():
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    if "hash_stats" not in v:
        v["hash_stats"] = vin_parser.parse_hash_stats(v["vin_plain"])
    return jsonify(v["hash_stats"])


@app.route('/api/vin/hash/lookup')
def api_vin_hash_lookup():
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    left = request.args.get("left", type=int)
    right = request.args.get("right", type=int)
    if left is None or right is None:
        return jsonify({"error": "left and right params required"}), 400
    cost = vin_parser.hash_lookup(v["vin_plain"], left, right)
    return jsonify({"left": left, "right": right, "cost": cost})


@app.route('/api/vin/mean')
def api_vin_mean():
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    if "mean" not in v:
        v["mean"] = vin_parser.parse_mean(v["vin_plain"])
    return jsonify(v["mean"])


@app.route('/api/vin/hist')
def api_vin_hist():
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    if "hist" not in v:
        v["hist"] = vin_parser.parse_hist(v["vin_plain"])
    return jsonify(v["hist"])


@app.route('/api/vin/prsl/stats')
def api_vin_prsl_stats():
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    if "prsl_stats" not in v:
        v["prsl_stats"] = vin_parser.parse_prsl_stats(v["vin_plain"])
    return jsonify(v["prsl_stats"])


@app.route('/api/vin/prsl/lookup')
def api_vin_prsl_lookup():
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    left_hp = request.args.get("left_hp", type=int)
    center_hp = request.args.get("center_hp", type=int)
    right_hp = request.args.get("right_hp", type=int)
    if left_hp is None or center_hp is None or right_hp is None:
        return jsonify({"error": "left_hp, center_hp, right_hp required"}), 400
    context_key = left_hp * 10000 + center_hp * 100 + right_hp
    candidates = vin_parser.prsl_lookup(v["vin_plain"], context_key)
    return jsonify({"context_key": context_key, "candidates": candidates,
                    "n_candidates": len(candidates) if candidates else 0})


@app.route('/api/vin/ccos')
def api_vin_ccos():
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    if "ccos" not in v:
        v["ccos"] = vin_parser.parse_ccos_summary(v["vin_plain"])
    return jsonify(v["ccos"])


@app.route('/api/vin/cklx')
def api_vin_cklx():
    """Get cklx entries with pagination and search.
    Params: group=_WORD_|_SYL_, page=N, per_page=N, q=search"""
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    if "cklx_full" not in v:
        v["cklx_full"] = vin_parser.parse_cklx_full(v["vin_plain"])

    data = v["cklx_full"]
    if data is None:
        return jsonify({"error": "cklx parse failed"}), 500

    group_name = request.args.get("group", "_WORD_")
    page = request.args.get("page", 0, type=int)
    per_page = request.args.get("per_page", 50, type=int)
    q = request.args.get("q", "").lower()

    # Find the requested group
    group = None
    for g in data["groups"]:
        if g["name"] == group_name:
            group = g
            break

    if group is None:
        return jsonify({"error": f"Group '{group_name}' not found"}), 404

    entries = group["entries"]
    if q:
        entries = [e for e in entries if q in e["key"].lower()]

    total = len(entries)
    start = page * per_page
    page_items = entries[start:start + per_page]

    return jsonify({
        "group": group_name,
        "groups": [g["name"] for g in data["groups"]],
        "group_counts": {g["name"]: g["entry_count"] for g in data["groups"]},
        "items": page_items,
        "total": total,
        "page": page,
        "per_page": per_page,
    })


@app.route('/api/vin/recording_tokens/<rec_name>')
def api_vin_recording_tokens(rec_name):
    """Get words and syllables for a recording from ckls."""
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    if "ckls" not in v:
        v["ckls"] = vin_parser.parse_ckls(v["vin_plain"])
    tokens = v["ckls"].get(rec_name) if v["ckls"] else None
    if tokens is None:
        return jsonify({"words": [], "syllables": []})
    return jsonify(tokens)


@app.route('/api/vin/search_words')
def api_vin_search_words():
    """Search for words/syllables in the corpus, return matching recordings."""
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404
    q = request.args.get("q", "").strip()
    if not q:
        return jsonify({"error": "q param required"}), 400
    if "filenames" not in v:
        v["filenames"] = vin_parser.parse_feat_filenames(v["vin_plain"])
    matches = vin_parser.search_words(v["vin_plain"], q)
    matches = vin_parser.resolve_word_recordings(
        v["vin_plain"], matches, v["unit_data"], v["filenames"])
    return jsonify({"query": q, "matches": matches})


@app.route('/api/vin/units_for_recording/<rec_name_or_fidx>')
def api_vin_units_for_recording(rec_name_or_fidx):
    """Get all units belonging to a specific recording.
    Accepts either a file_idx (int) or recording name (string).
    Name lookup goes through feat filenames to find the correct file_idx."""
    v, name = _voice()
    if v is None:
        return jsonify({"error": f"Voice '{name}' not found"}), 404

    if "filenames" not in v:
        v["filenames"] = vin_parser.parse_feat_filenames(v["vin_plain"])

    # Determine file_idx
    try:
        fidx = int(rec_name_or_fidx)
    except ValueError:
        # It's a recording name -- look up file_idx from feat
        fidx = None
        for sid, fname in v["filenames"].items():
            if fname == rec_name_or_fidx:
                fidx = sid
                break
        if fidx is None:
            return jsonify([])

    n_units = v["n_units"]
    ud = v["unit_data"]
    units = []
    for i in range(n_units):
        off = i * vin_parser.UNIT_RECORD_SIZE
        fi = struct.unpack_from('<H', ud, off+4)[0]
        if fi == fidx:
            units.append(vin_parser.get_unit(ud, i))
    return jsonify(units)


# -- API: VDB --

@app.route('/api/vdb/recordings')
def api_vdb_recordings():
    v, name = _voice()
    if v is None or v["vdb_info"] is None:
        return jsonify({"error": "VDB not available"}), 404
    q = request.args.get("q", "").lower()
    page = request.args.get("page", 0, type=int)
    per_page = request.args.get("per_page", 50, type=int)
    sort = request.args.get("sort", "name")  # name, duration, size, index
    sort_dir = request.args.get("sort_dir", "asc")

    recs = list(v["vdb_info"]["recordings"])
    if q:
        recs = [r for r in recs if q in r["name"].lower()]

    # Sort
    sort_key = {"name": "name", "duration": "duration_ms", "size": "size", "index": "index"}.get(sort, "name")
    recs.sort(key=lambda r: r.get(sort_key, 0), reverse=(sort_dir == "desc"))

    total = len(recs)
    start = page * per_page
    page_items = recs[start:start+per_page]
    return jsonify({"items": page_items, "total": total, "page": page})


@app.route('/api/vdb/audio/<rec_name>.wav')
def api_vdb_audio(rec_name):
    v, name = _voice()
    if v is None or v["vdb_plain"] is None:
        return jsonify({"error": "VDB not available"}), 404
    wav_bytes = vdb_parser.get_recording_wav(v["vdb_plain"], v["vdb_info"], rec_name)
    if wav_bytes is None:
        return jsonify({"error": "Recording not found"}), 404
    return Response(wav_bytes, mimetype='audio/wav')


@app.route('/api/vdb/unit_audio/<int:uid>.wav')
def api_vdb_unit_audio(uid):
    v, name = _voice()
    if v is None or v["vdb_plain"] is None:
        return jsonify({"error": "VDB not available"}), 404
    unit = vin_parser.get_unit(v["unit_data"], uid)
    if unit is None:
        return jsonify({"error": "Unit not found"}), 404

    # Resolve file_idx -> recording name -> VDB recording
    if "filenames" not in v:
        v["filenames"] = vin_parser.parse_feat_filenames(v["vin_plain"])
    rec_name = v["filenames"].get(unit["file_idx"])
    if rec_name is None:
        return jsonify({"error": f"No recording for file_idx {unit['file_idx']}"}), 404

    # Find the VDB recording by name
    vdb_rec = None
    for r in v["vdb_info"]["recordings"]:
        if r["name"] == rec_name:
            vdb_rec = r
            break
    if vdb_rec is None:
        return jsonify({"error": f"Recording '{rec_name}' not in VDB"}), 404

    # Extract unit audio segment
    lp = unit["local_pos"]
    dl = unit["dur_like"]
    data_off = v["vdb_info"]["data_offset"]
    rec_start = data_off + vdb_rec["offset"]
    unit_byte_start = lp * 8
    unit_byte_len = max(dl * 8, 80)
    start = rec_start + unit_byte_start
    end = min(start + unit_byte_len, rec_start + vdb_rec["size"])

    if start >= len(v["vdb_plain"]) or end <= start:
        return jsonify({"error": "Audio segment out of bounds"}), 404

    ulaw_data = v["vdb_plain"][start:end]
    pcm = vdb_parser.ulaw_to_pcm16(ulaw_data)
    wav_bytes = vdb_parser.pcm_to_wav_bytes(pcm)
    return Response(wav_bytes, mimetype='audio/wav')


@app.route('/api/vdb/waveform/<rec_name>')
def api_vdb_waveform(rec_name):
    v, name = _voice()
    if v is None or v["vdb_plain"] is None:
        return jsonify({"error": "VDB not available"}), 404
    data = vdb_parser.get_waveform_data(v["vdb_plain"], v["vdb_info"], rec_name)
    if data is None:
        return jsonify({"error": "Recording not found"}), 404
    return jsonify(data)


# -- API: Frida / Synthesis Tracer --
# Supports two modes:
#   1. Local: Frida attaches to local Speechify.exe (Windows dev machine)
#   2. Remote: Proxies to a remote synthesis worker (for VPS deployment)
# Set SYNTH_WORKER_URL env var to enable remote mode.

import urllib.request
import base64

SYNTH_WORKER_URL = os.environ.get('SYNTH_WORKER_URL')  # e.g. https://tts.23171944.xyz
SYNTH_WORKER_TOKEN = os.environ.get('SYNTH_WORKER_TOKEN', '')  # user:pass for HTTP Basic auth
_WORKER_UA = 'SpeechifyViz/1.0'  # Cloudflare-friendly User-Agent

_frida_mgr = None

def _get_frida_mgr():
    global _frida_mgr
    if _frida_mgr is None:
        from viz.frida_hooks.manager import FridaManager
        _frida_mgr = FridaManager()
    return _frida_mgr


def _is_remote_mode():
    return SYNTH_WORKER_URL is not None


def _worker_request(path, data=None, timeout=60):
    """Make an authenticated request to the remote synthesis worker."""
    headers = {'User-Agent': _WORKER_UA}
    if SYNTH_WORKER_TOKEN and ':' in SYNTH_WORKER_TOKEN:
        creds = base64.b64encode(SYNTH_WORKER_TOKEN.encode()).decode()
        headers['Authorization'] = f'Basic {creds}'
    if data is not None:
        headers['Content-Type'] = 'application/json'
        body = json.dumps(data).encode() if isinstance(data, dict) else data
    else:
        body = None
    req = urllib.request.Request(f"{SYNTH_WORKER_URL}{path}", data=body, headers=headers)
    resp = urllib.request.urlopen(req, timeout=timeout)
    return json.loads(resp.read())


def _synth_remote(text, voice="tom"):
    """Proxy synthesis to remote worker's /synth-trace endpoint."""
    try:
        result = _worker_request("/synth-trace", data={"text": text, "voice": voice}, timeout=60)
    except urllib.error.HTTPError as e:
        body = e.read().decode()
        try:
            err = json.loads(body)
            return jsonify({"error": err.get("error", f"Worker error {e.code}")}), 500
        except:
            return jsonify({"error": f"Worker error {e.code}: {body[:200]}"}), 500
    except Exception as e:
        return jsonify({"error": f"Worker unreachable: {e}"}), 500

    if not result.get("ok"):
        return jsonify(result), 500

    # Decode WAV from base64 and save locally for serving
    wav_url = None
    if result.get("wav_b64"):
        wav_bytes = base64.b64decode(result["wav_b64"])
        wav_name = f"synth_{int(time.time())}.wav"
        wav_dir = os.path.join(app.static_folder, "synth_output")
        os.makedirs(wav_dir, exist_ok=True)
        with open(os.path.join(wav_dir, wav_name), 'wb') as f:
            f.write(wav_bytes)
        wav_url = f"/static/synth_output/{wav_name}"

    # Enrich with unit metadata (VIN is local)
    v, name = _voice()
    enriched_result = {
        "ok": True,
        "wav_url": wav_url,
        "pre_prune_hps": result.get("pre_prune_hps", []),
        "wsola_uids": result.get("wsola_uids", []),
        "word_phones": result.get("word_phones", []),
        "n_hps": result.get("n_hps", 0),
        "n_wsola": result.get("n_wsola", 0),
    }

    if v:
        if "filenames" not in v:
            v["filenames"] = vin_parser.parse_feat_filenames(v["vin_plain"])
        fns = v["filenames"]

        # Enrich WSOLA UIDs
        enriched = []
        for uid in enriched_result["wsola_uids"]:
            if isinstance(uid, int) and 0 <= uid < v["n_units"]:
                u = vin_parser.get_unit(v["unit_data"], uid)
                u["rec_name"] = fns.get(u["file_idx"], "?")
                enriched.append(u)
            elif isinstance(uid, dict):
                enriched.append(uid)  # already enriched by worker
            else:
                enriched.append({"uid": uid, "phone": "?", "half": 0,
                                 "rec_name": "?", "file_idx": -1})
        enriched_result["wsola_units"] = enriched

        # Enrich pre-prune HPs
        for hp in enriched_result["pre_prune_hps"]:
            uid = hp.get("uid", -1)
            if isinstance(uid, int) and 0 <= uid < v["n_units"]:
                u = vin_parser.get_unit(v["unit_data"], uid)
                hp["phone"] = u["phone"]
                hp["half"] = u["half"]
                hp["rec_name"] = fns.get(u["file_idx"], "?")
                hp["file_idx"] = u["file_idx"]

    return jsonify(enriched_result)


@app.route('/api/frida/state')
def api_frida_state():
    if _is_remote_mode():
        try:
            data = _worker_request("/synth-trace/status", timeout=5)
            return jsonify({
                "attached": data.get("attached", False),
                "pid": data.get("pid"),
                "frida_available": data.get("available", True),
                "remote": True,
                "voices": data.get("voices", []),
            })
        except Exception as e:
            return jsonify({
                "attached": False,
                "frida_available": True,
                "remote": True,
                "error": str(e),
            })
    return jsonify(_get_frida_mgr().get_state())


@app.route('/api/frida/attach', methods=['POST'])
def api_frida_attach():
    if _is_remote_mode():
        try:
            data = _worker_request("/synth-trace/status", timeout=5)
            return jsonify({"ok": True, "msg": "Remote worker connected", "remote": True,
                            "pid": data.get("pid"), "attached": data.get("attached")})
        except Exception as e:
            return jsonify({"error": f"Worker unreachable: {e}"}), 500
    result = _get_frida_mgr().attach()
    return jsonify(result), 200 if result.get("ok") else 500


@app.route('/api/frida/detach', methods=['POST'])
def api_frida_detach():
    if _is_remote_mode():
        return jsonify({"ok": True, "msg": "Remote mode - nothing to detach"})
    return jsonify(_get_frida_mgr().detach())


def _synth_via_trace(text, voice="tom"):
    """Batch synthesis for the Synthesis Tracer, powered by spfy_synth_trace.exe
    instead of Frida/Speechify. Runs the trace binary, collects its full NDJSON
    event stream, and reshapes it into the batch payload the tracer expects
    (wsola_units, pre_prune_hps, word_phones). Unlike the Frida path this needs
    no closed engine, is fully concurrent, and actually carries *every*
    candidate's target cost (not just a pre-prune snapshot)."""
    exe = _trace_exe()
    if not exe:
        return jsonify({"error": "spfy_synth_trace.exe not found — build spfy "
                                 "(spfy/build.bat) or set SPFY_SYNTH_TRACE_EXE"}), 503
    assets = _voice_assets(voice)
    if not assets:
        return jsonify({"error": f"Voice assets (vin/vdb/vcf) for '{voice}' not found"}), 404
    vin, vdb, vcf = assets
    v = _get_voice(voice)

    wav_dir = os.path.join(app.static_folder, "synth_output")
    os.makedirs(wav_dir, exist_ok=True)
    wav_name = f"synth_{int(time.time())}.wav"
    wav_path = os.path.join(wav_dir, wav_name)

    try:
        if platform.system() == "Windows":
            proc = subprocess.run([exe, "--trace-stream", vin, vdb, vcf, text, wav_path],
                              capture_output=True, text=True, timeout=120)
        else:
            proc = subprocess.run(["wine", exe, "--trace-stream", vin, vdb, vcf, text, wav_path],
                              capture_output=True, text=True, timeout=120)
    except subprocess.TimeoutExpired:
        return jsonify({"error": "Synthesis timed out (120s)"}), 500
    if proc.returncode != 0:
        return jsonify({"error": f"Synthesis failed: {(proc.stderr or '').strip()[:400]}"}), 500

    # Parse the event stream: per-slot candidates/pick, ordered picks, words.
    slots = {}       # slot_idx -> {phone, pool_n, cands:[(uid,tc)], pick}
    picks = []       # ordered [{slot, uid}]
    words = []       # [{word, phones:[{phone,stress}]}]
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line or line[0] != '{':
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        st, d = obj.get("stage"), obj.get("data", {})
        if st == "slot":
            slots[d["slot"]] = {"phone": d.get("phone"), "pool_n": d.get("pool_n", 0),
                                "cands": [], "pick": None}
        elif st == "cand":
            s = slots.get(d["slot"])
            if s is not None:
                s["cands"].append((d["uid"], d["tc"]))
        elif st == "pick":
            s = slots.get(d["slot"])
            if s is not None:
                s["pick"] = d["uid"]
            picks.append({"slot": d["slot"], "uid": d["uid"]})
        elif st == "word":
            words.append({"word": d.get("text", "?"),
                          "phones": [{"phone": p, "stress": 0} for p in d.get("phones", [])]})

    if v and "filenames" not in v:
        v["filenames"] = vin_parser.parse_feat_filenames(v["vin_plain"])
    fns = v["filenames"] if v else {}

    def enrich(uid):
        if v and isinstance(uid, int) and 0 <= uid < v["n_units"]:
            u = vin_parser.get_unit(v["unit_data"], uid)
            if u:
                u["rec_name"] = fns.get(u["file_idx"], "?")
                return u
        return {"uid": uid, "phone": "?", "half": 0, "local_pos": 0, "dur_like": 0,
                "file_idx": -1, "rec_name": "?"}

    # wsola_units = the per-half-phone Viterbi picks, in synthesis order.
    wsola_units = [enrich(p["uid"]) for p in picks]

    # pre_prune_hps = per slot, the greedy best-by-target-cost candidate (what
    # you'd pick ignoring join cost) + the candidate list. A mismatch with the
    # Viterbi pick means the join cost changed the choice — same semantic as the
    # old Frida "pre-prune vs Viterbi" flag.
    pre_prune_hps = []
    for p in picks:
        s = slots.get(p["slot"], {})
        cands = s.get("cands", [])
        if cands:
            best = min(cands, key=lambda c: c[1])
            top = sorted(cands, key=lambda c: c[1])[:6]
        else:
            best, top = (p["uid"], 0.0), [(p["uid"], 0.0)]
        hp = enrich(best[0])
        hp["total"] = best[1]
        hp["n_cand"] = s.get("pool_n", len(cands))
        hp["top"] = [{"uid": u, "total": t} for (u, t) in top]
        pre_prune_hps.append(hp)

    return jsonify({
        "ok": True,
        "wav_url": f"/static/synth_output/{wav_name}",
        "wsola_units": wsola_units,
        "pre_prune_hps": pre_prune_hps,
        "word_phones": words,
        "n_hps": len(pre_prune_hps),
        "n_wsola": len(wsola_units),
    })


@app.route('/api/synth', methods=['POST'])
def api_synth():
    body = request.get_json(force=True)
    text = body.get("text", "")
    if not text:
        return jsonify({"error": "No text provided"}), 400

    if _is_remote_mode():
        return _synth_remote(text, body.get("voice", "tom"))

    # Local: powered by spfy_synth_trace.exe (no Frida, no Speechify.exe).
    return _synth_via_trace(text, body.get("voice", "tom"))


# -- API: Live Tracer (spfy_synth_trace.exe -> NDJSON -> SSE) --
#
# Unlike the Frida tracer (which hooks the closed Speechify.exe and returns a
# single batch result), the live tracer streams events from our own byte-exact
# reimplementation, spfy_synth_trace.exe. That binary is spfy_synth compiled
# with -DSPFY_TRACE=1: it emits one NDJSON line per core lookup (slot, every
# candidate score, Viterbi pick, WSOLA unit) as synthesis runs. We relay those
# lines to the browser over Server-Sent Events at full speed; the browser owns
# all visual pacing, so nothing here (or in the C engine) is slowed down.

def _trace_exe():
    """Locate spfy_synth_trace.exe. Order: env override, project bin/, dev
    build dir. Returns an absolute path or None if not built yet."""
    env = os.environ.get('SPFY_SYNTH_TRACE_EXE')
    if env and os.path.exists(env):
        return env
    for c in (os.path.join(PROJ_ROOT, "bin", "spfy_synth_trace.exe"),
              os.path.join(r"C:\tmp\spfy_build", "src", "cli", "spfy_synth_trace.exe")):
        if os.path.exists(c):
            return c
    return None


def _voice_assets(name):
    """Resolve (vin, vdb, vcf) for a voice. Prefers the 8-bit u-law VDB
    (tom8.vdb) — the 16k VDB decodes wrong through the 8kHz mu-law path.
    Returns None if any of the three is missing."""
    d = os.path.join(PROJ_ROOT, "en-US", name)
    vin = os.path.join(d, f"{name}.vin")
    vcf = os.path.join(d, f"{name}.vcf")
    vdb = None
    for cand in (f"{name}8.vdb", f"{name}.vdb"):
        p = os.path.join(d, cand)
        if os.path.exists(p):
            vdb = p
            break
    if not (os.path.exists(vin) and vdb and os.path.exists(vcf)):
        return None
    return vin, vdb, vcf


def _enrich_uid_event(v, data):
    """Add phone / half / rec_name / file_idx to a unit- or pick-event's data
    dict, resolved from the VIN unit table + feat filenames. Mutates and
    returns `data`. Only used for the low-volume unit/pick events — candidate
    events stay lightweight (uid + tc)."""
    uid = data.get("uid")
    if uid is None or not isinstance(uid, int) or not (0 <= uid < v["n_units"]):
        return data
    if "filenames" not in v:
        v["filenames"] = vin_parser.parse_feat_filenames(v["vin_plain"])
    u = vin_parser.get_unit(v["unit_data"], uid)
    if u:
        data["phone"] = u.get("phone", "?")
        data["half"] = u.get("half", 0)
        data["file_idx"] = u.get("file_idx", -1)
        data["rec_name"] = v["filenames"].get(u.get("file_idx"), "?")
    return data


@app.route('/api/synth/stream/status')
def api_synth_stream_status():
    exe = _trace_exe()
    return jsonify({"available": exe is not None, "exe": exe})


@app.route('/api/synth/stream')
def api_synth_stream():
    """SSE endpoint. Spawns spfy_synth_trace.exe and relays its NDJSON events
    to the browser one `data:` frame at a time, as they arrive. EventSource is
    GET-only, so text/voice come in as query params."""
    text = request.args.get("text", "").strip()
    voice = request.args.get("voice", "tom")
    if not text:
        return jsonify({"error": "No text provided"}), 400

    exe = _trace_exe()
    if not exe:
        return jsonify({"error": "spfy_synth_trace.exe not found — build spfy "
                                 "(spfy/build.bat) or set SPFY_SYNTH_TRACE_EXE"}), 503
    assets = _voice_assets(voice)
    if not assets:
        return jsonify({"error": f"Voice assets (vin/vdb/vcf) for '{voice}' not found"}), 404
    vin, vdb, vcf = assets
    v = _get_voice(voice)   # for uid enrichment; None only if vin vanished

    wav_dir = os.path.join(app.static_folder, "synth_output")
    os.makedirs(wav_dir, exist_ok=True)
    wav_name = f"live_{int(time.time())}.wav"
    wav_path = os.path.join(wav_dir, wav_name)

    def sse(obj):
        # Compact separators — the candidate stream is high-volume, so drop
        # the default ", " / ": " padding.
        return f"data: {json.dumps(obj, separators=(',', ':'))}\n\n"

    def generate():
        try:
            if platform.system() == "Windows":
                # Windows: spawn the trace binary directly
                proc = subprocess.Popen(
                    [exe, "--trace-stream", vin, vdb, vcf, text, wav_path],
                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                    text=True, bufsize=1,
                )
            else:
                # Linux/macOS: spawn via wine
                proc = subprocess.Popen(
                    ["wine", exe, "--trace-stream", vin, vdb, vcf, text, wav_path],
                    stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                    text=True, bufsize=1,
                )
        except Exception as e:
            yield sse({"stage": "error", "data": {"message": str(e)}})
            return

        try:
            for line in proc.stdout:
                line = line.strip()
                if not line or line[0] != '{':
                    continue  # skip any stray banner/diagnostic lines
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if obj.get("stage") in ("unit", "pick") and v:
                    obj["data"] = _enrich_uid_event(v, obj.get("data", {}))
                yield sse(obj)
            proc.wait()
        finally:
            try:
                proc.stdout.close()
            except Exception:
                pass

        # Terminal frame: hand the browser the rendered WAV to play back. The
        # client closes the EventSource on this event so it doesn't auto-
        # reconnect and re-run the synthesis.
        yield sse({"stage": "complete",
                   "data": {"wav_url": f"/static/synth_output/{wav_name}"}})

    return Response(generate(), mimetype='text/event-stream', headers={
        'Cache-Control': 'no-cache',
        'X-Accel-Buffering': 'no',   # disable proxy buffering (nginx/CF)
        'Connection': 'keep-alive',
    })

PORT = int(os.environ.get('SPFY_VIZ_PORT', 5000))  # Flask server port, default 5000

# -- Main --

def main():
    print("=" * 60)
    print("  Speechify VIN/VDB Visualizer")
    print("=" * 60)
    print(f"  Project root: {PROJ_ROOT}")

    # Pre-load Tom on startup
    print("\n  Pre-loading Tom voice...")
    _get_voice("tom")

    print(f"\n  Starting server on http://localhost:{PORT}...")
    print("=" * 60)
    # threaded=True so an open SSE stream (the live tracer) doesn't block
    # the browser's concurrent /static and WAV requests.
    app.run(host='0.0.0.0', port=PORT, debug=True, use_reloader=False, threaded=True)

if __name__ == '__main__':
    main()
