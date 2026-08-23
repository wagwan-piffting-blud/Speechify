/* spfy_synth -- M5 end-to-end text->WAV via FE -> USel -> WSOLA pipeline. */

#include <spfy/spfy.h>
#include "env.h"
#include "voice_find.h"

/* ⚠ GUARDED, and it has to be. This file is not only spfy_synth.exe: the WASM
 * target (spfy/wasm/CMakeLists.txt) and the Android app (app/src/main/cpp/)
 * compile it into their own spfy_synth_lib with SPFY_SYNTH_NO_MAIN, from
 * CMakeLists of their own that never run the version block and never put
 * src/update on the include path. An unconditional #include here builds fine
 * on Windows and breaks both of those on the next push -- and the wasm one
 * breaks in CI, because deploy-wasm-pages.yml triggers on any path under
 * spfy/src.
 *
 * Defined by the spfy_synth / spfy_synth_trace targets only. Without it this
 * file compiles exactly as it did before the update check existed. */
#ifdef SPFY_HAVE_UPDATE_CHECK
#  include <spfy/version.h>
#  include "spfy_update.h"
#endif

#include "../synth/spfy_synth_lib.h"

#include "../voice/voice.h"
#include "../voice/unit_table.h"
#include "../voice/feat_table.h"
#include "../voice/vdb_lookup.h"
#include "../voice/ccos.h"
#include "../voice/voice_runtime.h"
#include "../voice/vcf_matrix.h"
#include "../voice/chunk_table.h"
#include "../usel/anchor_score.h"
#include "../usel/build_graph.h"
#include "../usel/syl_span.h"
#include "../usel/hash.h"
#include "../usel/prsl.h"
#include "../usel/viterbi.h"
#include "../cart/cart.h"
#include "../wsola/ulaw.h"
#include "../wsola/wav.h"
#include "../wsola/wsola.h"
#include "../prosody/pmarks.h"
#include "../prosody/psola_unit.h"
#include "../prosody/contour.h"
#include "../prosody/reselect.h"
#include "../fe/fe.h"
#include "../fe/phoneset.h"
#include "../fe/prosody.h"
#include "../fe/baked_pos.h"

/* Live-trace event emitters. */
#include "../common/le.h"
#include "../common/log.h"

/* In-house FE - used when SPFY_FE_INTERNAL=1 to bypass the SWIttsFe
 * DLL drive entirely. Lets us A/B audit the new path vs the hosted FE. */
#include "../fe_internal/fe_internal.h"
#  include "../fe_host/fe_parse.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(_WIN32)
/* Get/SetEnvironmentVariableA for Speechify 4 mode. */
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

/* ⚠ 169578 is TOM's last unit index, not a universal constant. The engine's
 * trailing-silence unit is the LAST unit of whatever voice is loaded --
 * measured on its own prsl_slot records, which pin every phrase's final
 * half-phone to exactly that index: tom 169578, jill 185474, javier 219500,
 * paulina 663409, felix 259659, each n_units-1. Use SILENCE_UID(v) wherever a
 * voice is in scope; the literal survives only as the no-voice fallback. */
#define SILENCE_SENTINEL_UID 169578u
#define SILENCE_UID(v) ((v)->units.n_units ? (v)->units.n_units - 1u \
                                           : SILENCE_SENTINEL_UID)
#define MAX_CANDS_PER_SLOT   512

/*
 * One switch for the prosody configuration judged closest to Speechify 4.
 *
 * WHAT IT IS. The `f95_k1` settings: the relative contour with a -6 st
 * declination, zero-meaned, plus a soft absolute F0 floor at 95 Hz with a
 * 1 st knee. On "This is a radar indicated threat." this was judged
 * indistinguishable from a 2012 off-air reference by ear AND by waveform,
 * and it is the best-sounding configuration the project has produced.
 *
 * WHY THESE AND NOT OTHERS, briefly, so they are not re-litigated:
 *   ABSOLUTE=0   absolute mode costs 3.93 st/s of natural downstep and buys
 *                no extra authority over the contour.
 *   DECL 0 and FALL 0 -- the contour imposes NO global downward trend and no
 *                phrase-final fall. This is the mode's biggest correction and
 *                it was chosen by ear, twice, independently.
 *
 *                WHY, and it is not "less is better": the units are selected
 *                from real bulletin speech, so they ALREADY carry both. From
 *                the mark dump, with the contour zeroed, Tom's own units
 *                decline at -0.71/-0.89/-0.61 st/s on 1.5/3.7/5.5 s phrases
 *                and already land 0.77-1.32 st under his median at the tail.
 *                The contour was adding a SECOND declination and a SECOND
 *                final fall on top of ones that were already there.
 *
 *                The original DECL_ST=-6 made it worse than double: decl_st
 *                is a total over NORMALISED position, so the same -6 st is
 *                crammed into whatever length the phrase is, and the three
 *                phrases came out at -3.54/-2.57/-1.44 st/s -- the SHORTEST
 *                five times steeper than the voice ever is. Zero-meaning
 *                splits that both ways, so the onset went to +2.8..+5.8 st
 *                above the median while the tail sat 2.5..3.7 below. Two
 *                listeners called it robotic at the top and "the end of a
 *                very long sentence with no breaths taken" at the bottom, and
 *                SPFY_RATE could not help because stretching the phrase
 *                stretches the curve with it.
 *
 *                With both off, the realised tail is -1.19/-0.82/-0.97 st
 *                against the units' own -0.79/-0.77/-1.05: the stage now
 *                REDISTRIBUTES pitch across the phrase and no longer argues
 *                with its overall shape. 94-97% of marks are still warped, so
 *                this is emphatically not the stage doing nothing.
 *
 *                DECL_RATE_ST_S exists for anyone who does want declination:
 *                it is st/SECOND, so slope stops depending on phrase length,
 *                which DECL_ST cannot do. Bounded by DECL_MAX_ST. Both are
 *                off here; the knobs are correct, the need for them was not.
 *   DOWNSTEP_FLOOR 1.0 -- downstep OFF. The third global shape component
 *                turned off for the same reason as the other two: the units
 *                already carry it. Chosen by ear from a ladder whose rungs
 *                were the MEASURED height of the accent on "lowest":
 *                0.90 / 1.50 / 2.10 / 3.00 st. At the old 0.30 floor that
 *                accent was the 5th in the phrase and pinned at 3.0*0.30,
 *                a third the size of the first -- audible as "it is trying
 *                but I can barely hear it".
 *   ALIGN_MS=0   accents are centred on the syllable NUCLEUS now, and the
 *                -25 ms pre-alignment was compensating for the old centring.
 *                Measured with the accent-position dump: at -25 one of the
 *                eight accents lands on a `t` rather than the following `iy`;
 *                at 0 all eight land on a nucleus.
 *   METHOD=lp    LP-PSOLA, not plain TD-PSOLA. This is the one change that
 *                removed the "straining to hit the notes" stutter outright
 *                rather than trading it against something else.
 *
 *                WHY TD-PSOLA COULD NOT BE FIXED. Raising pitch means emitting
 *                more glottal pulses than the recording contains, so grains
 *                get duplicated -- and a grain carries a pulse AND the formant
 *                ring after it, so packing grains closer packs the rings
 *                together and the closed phase disappears. That is a pressed
 *                voice. A dose ladder judged by ear (0.2 / 0.5 / 1.0 / 3.0 st)
 *                came out cleanly monotonic, which is the signature of an
 *                artifact proportional to how far grains move. Everything that
 *                left that distance alone failed: sub-sample and blended grain
 *                placement, asymmetric up-limiting, the F0 floor, a dead zone,
 *                and unit re-selection (which cannot help at all in relative
 *                mode -- the shift is the contour, whatever unit is chosen).
 *
 *                LP-PSOLA moves the grain walk to the LP residual and rebuilds
 *                the formants afterwards with coefficients pinned to the
 *                ORIGINAL time axis. A duplicated residual pulse duplicates
 *                far less signal, and the ring is regenerated rather than
 *                copied. Measured: 0 fallbacks in 94 units, +9.8% synthesis
 *                time, within 0.9 dB of TD in every band. Judged by ear to
 *                remove the stutter while keeping the accent strength, which
 *                is better than predicted -- the artifact and the prosody are
 *                no longer traded against each other.
 *
 *                SPFY_PSOLA_METHOD=td restores plain TD-PSOLA.
 *   MAX_ST=12    the limiter is a tanh, so it bends every ratio a little
 *                rather than clipping a few; at 12 it removes under 0.27 st
 *                at the 95th percentile and never binds hard.
 *   FLOOR 95 Hz  without it 7.4% of a bulletin's marks land under 80 Hz and
 *                the tail creaks. The knee makes it a soft landing so the
 *                floor does not flatten the tail into a plateau.
 *   RATE 1.0     a pass-through. Our untagged duration is already within
 *                0.02 s of the reference, and the variable is listed here
 *                only so the whole recipe is in one place.
 *   SMOOTH 1     sub-sample grain placement. Level 0 rounds every output
 *                pitch mark to a whole sample, which puts a random +-0.25
 *                semitone on every glottal cycle -- measured at 8.7-9.4 cents
 *                mean across three bulletins, and 0.29 samples of jitter
 *                against a 0.026 instrument floor on a synthetic probe.
 *                Level 1 removes it entirely (0.026, i.e. the floor). The
 *                cost is 0.6-1.2 dB in 3.8-4.0 kHz, a band holding 0.5% of
 *                the energy, because a half-sample delay must null at fs/2.
 *                Level 2 additionally blends adjacent grains; it is a
 *                judgment call rather than a defect fix, so it is opt-in.
 *
 * ⚠ NOT INCLUDED: band-aware re-selection (SPFY_PROSODY_RESELECT_BAND).
 * It measurably cuts the work the knee has to do (-27% on the median bend)
 * but was judged by ear to introduce a slight microstutter, and its
 * permissive variant turned the word "threat" into a purr. Measurably
 * better, audibly worse. It stays opt-in and out of this mode.
 *
 * EDIT THESE to change what the mode means. They are applied as env
 * DEFAULTS -- anything already set in the environment wins -- so the mode
 * is a starting point that can be overridden one variable at a time, and
 * `SPFY_4_MODE=1 SPFY_PROSODY_DECL_ST=-3` is a legal way to explore.
 */

#define SPFY4_STAGE          "1"
#define SPFY4_ABSOLUTE       "0"
#define SPFY4_DECL_ST        "0"
#define SPFY4_DECL_RATE_ST_S "0"
#define SPFY4_DECL_MAX_ST    "4"
#define SPFY4_FALL_ST        "0"
#define SPFY4_MAX_ST         "12"
#define SPFY4_ZEROMEAN       "1.0"
#define SPFY4_F0_FLOOR_HZ    "95"
#define SPFY4_F0_KNEE_ST     "1.0"
#define SPFY4_RATE           "1.0"
#define SPFY4_PSOLA_SMOOTH   "1"
#define SPFY4_ALIGN_MS       "0"
#define SPFY4_DOWNSTEP_FLOOR "1.0"
#define SPFY4_PSOLA_METHOD   "lp"

/* The pitch-mark pair is found by dropping the VDB's extension:
 * <voice>/tom8.vdb -> <voice>/tom8{.pmindex,.pmdata}, which is where
 * reveng/spfy4/tools/gen_pitchmarks_real.py writes them. */

/* Remembered VDB path, for `\s4m` - see spfy4_note_vdb_path() in
 * spfy_synth_lib.h for why it cannot just use argv. */
static char g_s4_vdb[512];

void spfy4_note_vdb_path(const char *vdb)
{
    if (!vdb || !*vdb) return;
    snprintf(g_s4_vdb, sizeof g_s4_vdb, "%s", vdb);
}

/* Set `name` to `val` ONLY if it is not already in the environment, so an
 * explicit setting always beats a mode default. */
static void spfy4_env_default(const char *name, const char *val)
{
#if defined(_WIN32)
    /* SetEnvironmentVariableA, NOT _putenv: _putenv updates only the CRT's
     * private copy, and spfy_env() reads the OS block (see common/env.c for
     * why it must). */
    if (GetEnvironmentVariableA(name, NULL, 0) != 0) return;
    SetEnvironmentVariableA(name, val);
#else
    if (getenv(name)) return;
    setenv(name, val, 0);
#endif
}

/* Preselection backoff-ladder census, printed under SPFY_PRSL_STATS.
 * See the increment site for why it separates a KEYS problem from a MEMBERS
 * problem. Counted over half-phone slots only; the pinned first and last of
 * each phrase are not preselected and are excluded. */
static uint64_t g_prsl_rung_total  = 0;
static uint64_t g_prsl_rung_exact  = 0;
static uint64_t g_prsl_rung_1side  = 0;
static uint64_t g_prsl_rung_both   = 0;
static uint64_t g_prsl_rung_empty  = 0;

/* Apply the mode. */
/* Set by --no-s4. */
static int g_s4_forbidden = 0;

/* A `\s4m` that arrived with nothing to say, waiting for the utterance it
 * was meant for. */
typedef struct spfy4_env_scope_s spfy4_env_scope_t;
static int g_s4_pending = 0;

/* Every variable the mode writes, listed once because the per-utterance
 * `\s4m` path has to put every one of them back afterwards. */
static const char *const SPFY4_VARS[] = {
    "SPFY_PROSODY_STAGE", "SPFY_PROSODY_ABSOLUTE", "SPFY_PROSODY_DECL_ST",
    "SPFY_PROSODY_MAX_ST", "SPFY_PROSODY_ZEROMEAN", "SPFY_PROSODY_F0_FLOOR_HZ",
    "SPFY_PROSODY_F0_KNEE_ST", "SPFY_RATE", "SPFY_PROSODY_PM",
    "SPFY_PSOLA_SMOOTH", "SPFY_PROSODY_DECL_RATE_ST_S",
    "SPFY_PROSODY_DECL_MAX_ST", "SPFY_PROSODY_FALL_ST",
    "SPFY_PROSODY_ALIGN_MS", "SPFY_PROSODY_DOWNSTEP_FLOOR",
    "SPFY_PSOLA_METHOD",
};
#define SPFY4_NVARS (sizeof SPFY4_VARS / sizeof *SPFY4_VARS)

/* Snapshot / restore, so `\s4m` can scope the mode to ONE utterance. */
struct spfy4_env_scope_s {
    char          val[SPFY4_NVARS][512];
    unsigned char present[SPFY4_NVARS];
};

static spfy4_env_scope_t g_s4_pending_scope;

static void spfy4_env_save(spfy4_env_scope_t *s)
{
    if (!s) return;
    for (size_t i = 0; i < SPFY4_NVARS; ++i) {
#if defined(_WIN32)
        DWORD n = GetEnvironmentVariableA(SPFY4_VARS[i], s->val[i],
                                          (DWORD)sizeof s->val[i]);
        s->present[i] = (n != 0 && n < sizeof s->val[i]) ? 1u : 0u;
        if (!s->present[i]) s->val[i][0] = '\0';
#else
        const char *v = getenv(SPFY4_VARS[i]);
        s->present[i] = v ? 1u : 0u;
        s->val[i][0] = '\0';
        if (v) snprintf(s->val[i], sizeof s->val[i], "%s", v);
#endif
    }
}

static void spfy4_env_restore(const spfy4_env_scope_t *s)
{
    if (!s) return;
    for (size_t i = 0; i < SPFY4_NVARS; ++i) {
#if defined(_WIN32)
        /* NULL removes the variable, which is what a previously-unset one
         * must go back to -- setting it to "" would leave the prosody stage
         * switched on. */
        SetEnvironmentVariableA(SPFY4_VARS[i],
                                s->present[i] ? s->val[i] : NULL);
#else
        if (s->present[i]) setenv(SPFY4_VARS[i], s->val[i], 1);
        else               unsetenv(SPFY4_VARS[i]);
#endif
    }
    spfy_env_reset();
}

/* Write the defaults. */
static void spfy4_apply_defaults(const char *vdb_path)
{
    spfy4_env_default("SPFY_PROSODY_STAGE",       SPFY4_STAGE);
    spfy4_env_default("SPFY_PROSODY_ABSOLUTE",    SPFY4_ABSOLUTE);
    spfy4_env_default("SPFY_PROSODY_DECL_ST",     SPFY4_DECL_ST);
    spfy4_env_default("SPFY_PROSODY_DECL_RATE_ST_S", SPFY4_DECL_RATE_ST_S);
    spfy4_env_default("SPFY_PROSODY_DECL_MAX_ST",    SPFY4_DECL_MAX_ST);
    spfy4_env_default("SPFY_PROSODY_FALL_ST",     SPFY4_FALL_ST);
    spfy4_env_default("SPFY_PROSODY_MAX_ST",      SPFY4_MAX_ST);
    spfy4_env_default("SPFY_PROSODY_ZEROMEAN",    SPFY4_ZEROMEAN);
    spfy4_env_default("SPFY_PROSODY_F0_FLOOR_HZ", SPFY4_F0_FLOOR_HZ);
    spfy4_env_default("SPFY_PROSODY_F0_KNEE_ST",  SPFY4_F0_KNEE_ST);
    spfy4_env_default("SPFY_RATE",                SPFY4_RATE);
    spfy4_env_default("SPFY_PSOLA_SMOOTH",        SPFY4_PSOLA_SMOOTH);
    spfy4_env_default("SPFY_PROSODY_ALIGN_MS",    SPFY4_ALIGN_MS);
    spfy4_env_default("SPFY_PROSODY_DOWNSTEP_FLOOR", SPFY4_DOWNSTEP_FLOOR);
    spfy4_env_default("SPFY_PSOLA_METHOD",        SPFY4_PSOLA_METHOD);
    /* ⚠ MANDATORY after the putenv()s above. spfy_env() caches per call site,
     * including a cached NULL, so any site that probed one of these variables
     * before this point would go on seeing "unset" forever and S4 mode would
     * silently do nothing. The lazy call from the prosody stage is exactly
     * such a case: by then synthesis has already read SPFY_PROSODY_*. */
    spfy_env_reset();

    /* Fall back to the path noted at voice-load time: a `\s4m` tag reaches
     * here with no argv to derive from. */
    if (!vdb_path || !*vdb_path) vdb_path = g_s4_vdb[0] ? g_s4_vdb : NULL;
    if (vdb_path && *vdb_path && !spfy_env("SPFY_PROSODY_PM")) {
        char stem[512];
        size_t n = strlen(vdb_path);
        const char *dot = NULL;
        for (size_t i = n; i > 0; --i) {
            char c = vdb_path[i - 1];
            if (c == '/' || c == '\\') break;
            if (c == '.') { dot = vdb_path + (i - 1); break; }
        }
        if (!dot) dot = vdb_path + n;
        n = (size_t)(dot - vdb_path);
        if (n < sizeof stem) {
            memcpy(stem, vdb_path, n);
            stem[n] = '\0';
            spfy4_env_default("SPFY_PROSODY_PM", stem);
            spfy_env_reset();
        }
    }
    if (!spfy_env("SPFY_PROSODY_PM"))
        spfy_log_warn("S4 mode: no SPFY_PROSODY_PM and none derivable; "
                      "the prosody stage will not start");
    else
        /* Plain ASCII: this line lands in consoles that decode as cp1252,
         * where a UTF-8 em-dash arrives as mojibake. */
        spfy_log_warn("S4 mode ON - decl %s st, max %s st, floor %s Hz "
                      "(knee %s st), marks '%s'",
                      spfy_env("SPFY_PROSODY_DECL_ST"),
                      spfy_env("SPFY_PROSODY_MAX_ST"),
                      spfy_env("SPFY_PROSODY_F0_FLOOR_HZ"),
                      spfy_env("SPFY_PROSODY_F0_KNEE_ST"),
                      spfy_env("SPFY_PROSODY_PM"));
}

/* One-shot wrapper for the PROCESS-WIDE routes: --s4, -4 and SPFY_4_MODE. */
static void spfy4_mode_apply(const char *vdb_path, int forced)
{
    static int done = 0;
    if (forced < 0) { g_s4_forbidden = 1; done = 1; return; }
    if (done) return;
    if (forced == 0) {
        const char *e = spfy_env("SPFY_4_MODE");
        if (!(e && *e && *e != '0')) return;
    }
    done = 1;
    spfy4_apply_defaults(vdb_path);
}


/* Gates the per-synth status chatter emitted by spfy_synth_to_sink (FE slot
 * count, phrase-boundary marker, PRSL pool sizes, PostScoringAdj tally,
 * F0-curve params, final viterbi cost). */
static int spfy_synth_verbose = -1;

static int synth_is_verbose(void)
{
    if (spfy_synth_verbose < 0)
        spfy_synth_verbose = (spfy_env("SPFY_VERBOSE") != NULL
                              || spfy_env("SPFY_SYNTH_DEBUG") != NULL) ? 1 : 0;
    return spfy_synth_verbose;
}


/* One `pau` sub-unit inside a WsolaUnit, for FUN_08ee1ee0's resize.
 *
 * A unit can be a RUN of consecutive UIDs, and only one of its subs is the
 * pau -- e.g. "dog." batches g1/g2 with the trailing pause because their UIDs
 * are consecutive. The splice happens at THAT SUB's midpoint, not at the
 * run's, so the offset has to travel with it. Sizing the run by truncation
 * instead diverges from the engine across the whole run. */
typedef struct {
    uint32_t off;
    uint32_t nom;
    uint32_t tgt;
} pau_resize_t;

/* FUN_08ee1e70: symmetric V-notch centred ON a splice point, `n` samples each
 * side, gain i/n rising away from the centre -- the signal is faded to ZERO
 * at the join rather than crossfaded, because two uncorrelated stretches of
 * recorded silence have nothing to crossfade.
 *
 * ⚠ `gain` lives in an x87 register: the step is read as float32 but
 * ACCUMULATED in 80-bit, and each sample product is formed in 80-bit before
 * the truncating convert (FUN_08ee8828, which is what a plain C float->int
 * cast already does). Accumulating in float rounds every iteration and leaves
 * +/-1 scattered across the taper. */
static void apply_splice_taper(int16_t *buf, uint32_t blen,
                               uint32_t at, uint32_t n)
{
    if (!n) return;
    float step = (float)(1.0L / (long double)n);
    long double gain = 0.0L;
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t fwd = at + i;
        if (fwd < blen) buf[fwd] = (int16_t)((long double)buf[fwd] * gain);
        if (at >= i)    buf[at - i] = (int16_t)((long double)buf[at - i] * gain);
        gain += (long double)step;
    }
}

/* `pau` / `n_pau`: the pau subs to resize, ascending by `off`. */
static int decode_unit_samples(uint16_t file_idx, uint16_t lp_ms, uint16_t dur_ms,
                                const spfy_feat_table_t *feat,
                                const spfy_vdb_t        *vdb,
                                const spfy_vdb_lookup_t *lookup,
                                uint32_t over_n,
                                const pau_resize_t      *pau, uint32_t n_pau,
                                int16_t **out, size_t *out_n,
                                size_t *out_nominal_n,
                                size_t *out_pre_n)
{
    *out = NULL; *out_n = 0;
    if (out_nominal_n) *out_nominal_n = 0;
    if (out_pre_n) *out_pre_n = 0;
    /* ⚠ A ZERO-DURATION UNIT IS STILL A WsolaUnit.
     *
     * Returning here made it vanish from the stream, so the engine's join for
     * it never happened on our side. The engine builds the same
     * [pre][content][over-read] span with content = 0, runs FUN_08ee3560, and
     * emits the W blend samples; only the body loop finds nothing to copy.
     * Skipping it therefore costs W of output AND leaves the NEXT join
     * correlating against the wrong history, which shifts its lag too.
     *
     * Measured on javier es_084: path uid 198426 (dur_like 0) sits between
     * 125792 and 127766 in the engine's own wsola_in list, and dropping it
     * accounted for exactly the 82-sample deficit -- 80 of missing blend plus
     * the 2 that the following join's lag moved by.
     *
     * SPFY_ZERO_DUR_SKIP=1 restores the drop. */
    static int zd_skip = -1;
    if (zd_skip < 0) zd_skip = (spfy_env("SPFY_ZERO_DUR_SKIP") != NULL);
    if (dur_ms == 0 && zd_skip) return SPFY_OK;
    if (file_idx >= feat->n_entries) return SPFY_E_OOB;
    const spfy_feat_entry_t *fe = &feat->entries[file_idx];
    uint32_t rec_off = 0, rec_size = 0;
    int rc = spfy_vdb_lookup_by_name(lookup, fe->name, fe->name_len,
                                     &rec_off, &rec_size);
    if (rc != SPFY_OK) return rc;
    /* ⚠ SAMPLES vs BYTES. `rec_size` comes from the indx entry offsets, which
     * are BYTE offsets into the data chunk, while everything below counts
     * SAMPLES. Those coincide only for 1-byte µ-law, which is why the 8 kHz
     * path could conflate them; at 16-bit PCM they differ by 2. */
    uint32_t sps = vdb->sample_rate / 1000u;
    if (sps == 0u) sps = 8u;
    uint32_t bps = vdb->bytes_per_sample ? vdb->bytes_per_sample : 1u;
    uint32_t rec_n = rec_size / bps;
    uint32_t off  = lp_ms * sps;
    /* Always decode the unit's FULL span. A pau target is not a truncation:
     * FUN_08ee1ee0 resizes the pau IN THE DECODED BUFFER by splicing at its
     * MIDPOINT -- deleting samples symmetrically about the middle (or
     * inserting zeros there to lengthen) -- so both ENDS of the recorded
     * silence survive and only the middle is dropped. Truncating to the
     * first `target` samples keeps the right COUNT and the wrong SAMPLES. */
    uint32_t nominal = dur_ms * sps;
    if (off >= rec_n) return SPFY_OK;
    /* ⚠ CONTENT cannot outrun the recording, even though the OVER-READ may.
     *
     * The zero-pad below is right for the over-read: a unit at the tail of a
     * recording must still present pre+nominal+over_n to the lag search, and
     * clamping `blen` shortened the last unit of every phrase (6 samples on
     * jill "One."). But that argument covers the READ WINDOW, not the unit's
     * own length, and nothing was bounding the latter.
     *
     * jill.vin holds 31 units with dur_like >= 60000, one of them exactly
     * 0xFFFF: uid 141269, file_idx 4520 ('shortAdd1_099', 7480 samples),
     * local_pos 926 ms = sample 7408. Seventy-two samples of audio exist
     * there; dur_like asks for 524280. We were emitting the 72 and then
     * 524208 samples of memset zero -- 65.535 s of silence inside the word.
     *
     * Measured against the real 3.0.5 engine (bin/Speechify.exe via
     * spfy_dumpwav) on the four words that reach this unit:
     *
     *     say laverdiere again    engine 1301.2 ms   ours 66835.8 ms
     *     say booties again       engine 1084.8 ms   ours 66621.2 ms
     *     say dirtyer again       engine 1222.0 ms   ours 66747.2 ms
     *     say verdyer again       engine 1184.0 ms   ours 66709.2 ms
     *
     * while three controls through the same path were byte-identical, so the
     * engine bounds the content by the recording and we did not.
     *
     * SPFY_NO_DUR_CLAMP=1 restores the unbounded behaviour. */
    static int no_dur_clamp = -1;
    if (no_dur_clamp < 0)
        no_dur_clamp = (spfy_env("SPFY_NO_DUR_CLAMP") != NULL);
    if (!no_dur_clamp && nominal > rec_n - off)
        nominal = rec_n - off;
    /* ⭐ PRE-ROLL, not post-overread.
     *
     * FUN_08ee2960 reads from (local_pos - W) and records the unit's start
     * as being W samples INTO the decoded buffer (this+0x35d0 = W, or the
     * clamped start when the unit sits within W of the recording's head).
     * FUN_08ee3560 then blends from buffer offset `lag`, i.e. inside that
     * pre-roll, and FUN_08ee36e0 starts the body at W + lag.
     *
     * So the crossfade is fed by the audio BEFORE the unit, and the unit's
     * own samples all survive to the output. Measured against the engine's
     * output cursor over the pangram: speech units emit dur - lag, with a
     * median shortfall of 16 samples and a +/-40 spread (= the lag range),
     * NOT dur - 80.
     *
     * We previously over-read AFTER the unit and let the blend eat the
     * unit's first `ola_samples`, losing an extra ~80 per join on top of
     * the lag - which is why our renders stayed long in a way no constant
     * could fix.
     *
     * `pre` is how much history we actually got; it may be short at the very
     * start of a recording, which is exactly the clamped case the engine
     * handles with `if (iVar11 < iVar3)`. SPFY_WSOLA_NO_PREROLL=1 restores
     * the old post-overread. */
    static int no_pre = -1;
    if (no_pre < 0) no_pre = (spfy_env("SPFY_WSOLA_NO_PREROLL") != NULL);
    /* Pre-roll is W (this+4), NOT the over-read. FUN_08ee2960 clamps it to
     * local_pos when the unit sits within W of the recording's head, which
     * is why uid 0 (local_pos 0) gets none at all. */
    uint32_t pre = 0;
    if (!no_pre) {
        /* Pre-roll is W, so it follows the VDB's rate for the same reason
         * the blend does -- 10 ms either way, 160 samples at 16 kHz. */
        uint32_t want = spfy_wsola_w_for_rate(vdb->sample_rate);
        pre = (want < off) ? want : off;
    }
    uint32_t start = off - pre;
    uint32_t cap = pre + nominal + over_n;
    for (uint32_t k = 0; k < n_pau; ++k)
        if (pau[k].tgt > pau[k].nom) cap += (pau[k].tgt - pau[k].nom);
    uint32_t blen = pre + nominal + over_n;
    if (blen == 0) return SPFY_OK;
    int16_t *buf = (int16_t *)malloc(cap * sizeof *buf);
    if (!buf) return SPFY_E_NOMEM;
    /* ⚠ ZERO-PAD past the recording's end; do NOT shrink the span.
     * FUN_08ee2960 allocates the decoded span plus a zero run and memsets it,
     * so a unit near the end of a recording still presents a full-length
     * buffer to the lag search and the body copy. Clamping `blen` instead
     * shortened the last unit of every phrase -- 6 samples on "One.". */
    uint32_t dec_n = blen;
    if (start + dec_n > rec_n) dec_n = rec_n - start;
    /* Sample-domain call: spfy_vdb_decode applies bytes-per-sample and
     * zero-fills any shortfall, so the µ-law/PCM split stays out of here. */
    spfy_vdb_decode(vdb, rec_off, start, dec_n, buf);
    if (dec_n < blen)
        memset(buf + dec_n, 0, (size_t)(blen - dec_n) * sizeof *buf);

    /* Pau resize, FUN_08ee1ee0. Splice symmetrically about the pau's
     * midpoint; the engine then writes the resolved target back into the
     * sub's dur/samples fields, which is what our content length becomes.
     *
     * Splicing leaves a discontinuity, so FUN_08ee1e70 tapers it out: a
     * symmetric V-notch centred ON the splice, `n` samples each side, gain
     * i/n rising away from the centre. The signal is faded to ZERO at the
     * join rather than crossfaded -- two uncorrelated stretches of recorded
     * silence have nothing to crossfade. The decompiler drops the FPU
     * operands; the disassembly is
     *     step = 1/n; gain = 0
     *     loop i: fwd[i] *= gain; bwd[-i] *= gain; gain += step
     * and rounding is FUN_08ee8828 = truncate toward zero, which is what a
     * plain C float->int cast already does. */
    uint32_t content = nominal;
    int32_t  shift   = 0;
    for (uint32_t k = 0; k < n_pau; ++k) {
        uint32_t splice_at = 0, splice_n = 0;
        /* ⚠ `blen` has ALREADY absorbed every earlier splice, so `shift`
         * belongs on the SPAN side of this test -- subtracting it from the
         * buffer as well counts each earlier cut twice and rejects the
         * second pau of any run that already shrank one.
         *
         * jill text_004 ("I.") is the case that exposed it: one run of four
         * carries a leading pau (760 -> 104) and a trailing one (280 -> 200),
         * and only the first ran. The engine's content there is 3760 where
         * ours was 3840, and the first differing sample lands at buffer index
         * 3642 -- the left edge of the taper the skipped splice would have
         * applied (cut_start 3740, half-width 99). Both the length and the
         * position fall out of running it.
         *
         * SPFY_PAU_AVAIL_LEGACY=1 restores the double-count. */
        static int avail_legacy = -1;
        if (avail_legacy < 0)
            avail_legacy = (spfy_env("SPFY_PAU_AVAIL_LEGACY") != NULL);
        int32_t span_end = (int32_t)(pre + nominal)
                         + (avail_legacy ? 0 : shift);
        int32_t avail_end = (int32_t)blen + (avail_legacy ? shift : 0);
        if (!pau[k].nom || pau[k].tgt == pau[k].nom
            || pau[k].off + pau[k].nom > nominal
            || span_end > avail_end)
            continue;
        /* Midpoint of THIS SUB, not of the unit, and after any earlier
         * splice has already moved the samples that precede it. */
        int32_t base = (int32_t)(pre + pau[k].off) + shift;
        if (base < 0) continue;
        uint32_t mid = (uint32_t)base + pau[k].nom / 2u;
        if (pau[k].tgt < pau[k].nom) {
            uint32_t rm = pau[k].nom - pau[k].tgt;
            uint32_t cut_end = mid + rm / 2u;
            if (cut_end <= blen && cut_end >= rm && (cut_end - rm) >= pre) {
                uint32_t cut_start = cut_end - rm;
                memmove(buf + cut_start, buf + cut_end,
                        (size_t)(blen - cut_end) * sizeof *buf);
                blen    -= rm;
                content -= rm;
                shift   -= (int32_t)rm;
                splice_at = cut_start;
                splice_n  = pau[k].tgt / 2u;
                if (splice_n) --splice_n;
            }
        } else {
            uint32_t add = pau[k].tgt - pau[k].nom;
            if (blen + add <= cap && mid <= blen) {
                /* ⚠ ORDER. FUN_08ee1ee0 calls the taper BEFORE the memmove on
                 * this branch (and after it on the shrink branch). So both
                 * sides of the ORIGINAL midpoint are faded to zero and the
                 * inserted zeros go BETWEEN them. Tapering afterwards instead
                 * lands the forward half on the zeros, leaving the samples
                 * after the gap jumping to full amplitude -- visible as a
                 * zero run consistently shorter than the engine's. */
                uint32_t n = pau[k].nom / 2u;
                if (n) --n;
                apply_splice_taper(buf, blen, mid, n);
                memmove(buf + mid + add, buf + mid,
                        (size_t)(blen - mid) * sizeof *buf);
                memset(buf + mid, 0, (size_t)add * sizeof *buf);
                blen    += add;
                content += add;
                shift   += (int32_t)add;
            }
        }
        if (splice_n) apply_splice_taper(buf, blen, splice_at, splice_n);
    }

    *out = buf; *out_n = blen;
    if (out_pre_n) *out_pre_n = pre;
    /* The unit's own length after any resize; the caller offsets past `pre`
     * to reach it. */
    if (out_nominal_n) {
        uint32_t avail = (blen > pre) ? (blen - pre) : 0;
        *out_nominal_n = (content < avail) ? content : avail;
    }
    return SPFY_OK;
}

/* Optional prosody stage (reveng/spfy4/PLAN_PROSODY_STAGE.md). */
typedef struct {
    int              on;
    spfy_pmarks_t    marks;
    spfy_contour_t   contour;
    /* Per-slot output length, from prosody_slot_out_dur(). */
    uint32_t        *slot_dur;
    /* Global output sample count at the start of THIS phrase - for the
     * end-of-phrase sanity check only. */
    uint64_t         out0;
    /* phone_center label of `pau`, or 0xFFFF if the voice has none.
     *
     * The real engine's WsolaVoiceDatabase::getPitchMarks (FUN_08EE4200,
     * reveng/DLL_ANALYSIS.md) guards every sub-unit with
     * `strncmp(sub_unit->name, "pau", 3) != 0` - pauses carry NO marks. We
     * read them, and Tom's pau units supply 15.6% of all marks with periods
     * down to 2 samples (4000 Hz), which is silence being warped by values
     * that are not pitch periods at all. SPFY_PROSODY_PM_PAU=0 restores the
     * old behaviour for A/B. */
    uint16_t         pau_label;
    const uint8_t   *phone_center;
    uint32_t         phone_stride;
    uint32_t         n_units;
} prosody_stage_t;

/* Identifies which units a push covers, so the stage can fetch their marks
 * and sample the contour at the right output time. */
typedef struct {
    uint32_t uid;
    uint32_t run_n;
    /* Position on the contour's timeline where this push starts: the
     * cumulative sum of prosody_slot_out_dur() over the slots already
     * consumed in this phrase. */
    uint64_t nom_pos;
} unit_ref_t;

/* Env helpers for the prosody guards. */
static float env_f(const char *name, float dflt)
{
    const char *e = spfy_env(name);
    return (e && *e) ? (float)atof(e) : dflt;
}

/* True only when the variable is explicitly "0" -- i.e. */
static int env_flag_off(const char *name)
{
    const char *e = spfy_env(name);
    return e && *e == '0';
}

static int append_recording_span(spfy_wsola_streamer_t *ws,
                                 uint32_t file_idx, uint32_t lp, uint32_t dur,
                                 const spfy_feat_table_t *feat,
                                 const spfy_vdb_t        *vdb,
                                 const spfy_vdb_lookup_t *lookup,
                                 int align,
                                 uint8_t f0_tail, uint8_t f0_head,
                                 uint32_t sample_rate,
                                 float vol_gain,
                                 const prosody_stage_t *pros,
                                 const unit_ref_t *ref,
                                 const pau_resize_t *pau, uint32_t n_pau)
{
    int16_t *buf = NULL; size_t n = 0, nominal_n = 0, pre_n = 0;
    /* Over-decode by SPFY_WSOLA_MAX_LAG_DEFAULT (= engine's lag search
     * range = window_size = 80 samples @ 8 kHz) so the lag shift has a
     * look-ahead reservoir. */
    static int no_overread = -1;
    if (no_overread < 0)
        no_overread = (spfy_env("SPFY_WSOLA_NO_OVERREAD") != NULL);
    /* ⚠ 2W, not W. FUN_08ee2960 grows the decoded span by `this+0xc` (= 2W =
     * 160 @ 8 kHz), and that tail is what FUN_08ee2d60 copies into the
     * lag-search history. Over-reading only W left the next join correlating
     * against 80 samples of zero padding. */
    uint32_t over_n = no_overread ? 0u
                    : (spfy_wsola_w_for_rate(vdb->sample_rate) * 2u);
    int rc = decode_unit_samples((uint16_t)file_idx, (uint16_t)lp,
                                 (uint16_t)dur, feat, vdb, lookup,
                                 over_n, pau, n_pau,
                                 &buf, &n, &nominal_n, &pre_n);
    if (rc != SPFY_OK) return rc;
    /* Per-word volume (\!vp/\!vd embedded tags): scale the decoded unit
     * before the OLA push. */
    if (vol_gain != 1.0f && buf) {
        for (size_t i = 0; i < n; ++i) {
            float v = (float)buf[i] * vol_gain;
            if (v >  32767.0f) v =  32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            buf[i] = (int16_t)lrintf(v);
        }
    }
    if (spfy_env("SPFY_TRACE_UNITS")) {
        /* Cumulative sample-count tracker so the user can map output
         * waveform positions back to which unit was being emitted. */
        static uint64_t cum_n = 0;
        /* uid/run are printed so a waveform position can be tied to the
         * SELECTED unit, not just to its source recording: measuring
         * spectral discontinuity AT a join needs to know which sample offset
         * is a join and which is mid-unit, and file/lp/dur alone cannot say
         * whether two consecutive units were a same-rec continuation. */
        fprintf(stderr,
                "[unit] t=%7.1fms  uid=%7u  run=%3u  file=%4u  lp=%5u  dur=%4u  n=%4zu  nom=%4zu  align=%d  f0t=%u f0h=%u  cum=%llu\n",
                (double)cum_n * 1000.0 / 8000.0,
                ref ? ref->uid : 0u, ref ? ref->run_n : 0u,
                file_idx, lp, dur, n, nominal_n, align, f0_tail, f0_head,
                (unsigned long long)cum_n);
        cum_n += nominal_n;
    }
    /* Prosody stage: warp this unit's pitch toward the FE-derived contour,
     * pre-join, using the exact VDB marks. */
    if (pros && pros->on && ref && buf) {
        /* Concatenate the run's per-uid marks, exactly as the engine's
         * getPitchMarks walks consecutive sub-units. */
        int16_t mk[512];
        float   tg[512];
        /* DIAGNOSTIC ONLY, never read by the warp. */
        float   tgraw[512];
        float   stc[512];
        unsigned char why[512];        /* 0 warp 1 pau 2 nat_guard 3 lead-in inherited 4 lead-in orphan 5 no period
 * 6 contour returned 0 */
        /* mk[] concatenates several units' mark arrays, and the LEAD-IN is
         * the first element of EACH unit's array, not just of mk[]. */
        unsigned char lead[512];
        unsigned char paum[512];
        int nm = 0;
        for (uint32_t k = 0; k < ref->run_n && nm < (int)(sizeof mk / sizeof *mk); ++k) {
            const int16_t *pp = NULL;
            /* `pau` sub-units are FLAGGED, not dropped. */
            const int is_pau =
                (pros->pau_label != 0xFFFFu && pros->phone_center
                 && (ref->uid + k) < pros->n_units
                 && pros->phone_center[(size_t)(ref->uid + k)
                                       * pros->phone_stride]
                    == (uint8_t)pros->pau_label);
            int c = spfy_pmarks_get(&pros->marks, ref->uid + k, &pp);
            for (int i = 0; i < c && nm < (int)(sizeof mk / sizeof *mk); ++i) {
                lead[nm] = (i == 0) ? 1u : 0u;
                paum[nm] = is_pau ? 1u : 0u;
                mk[nm++] = pp[i];
            }
        }
        if (nm >= 2) {
            double t = (double)ref->nom_pos;
            /* Only binds in ABSOLUTE mode, where a bad mark no longer
             * cancels out of the warp ratio. */
            /* ⚠ NO LONGER GATED ON ABSOLUTE MODE. The original reasoning was
             * that in relative mode a wrong natural cancels out of the warp
             * RATIO (tg/nat collapses to 2^(st/12)), so a bad mark was
             * harmless. It cancels out of the ratio and NOT out of the PSOLA:
             * the warp still lays this unit's pulses at mk/ratio, so a mark
             * carrying half the true period places pulses at double rate.
             *
             * Measured on 4 items (accent_where.py): 8.66% of emitted marks sit
             * more than 7 st from Tom's 117.6 Hz median, and they are 2.4x
             * over-represented among the highest output pitches -- 22% of
             * z011's top decile against a 9.0% base rate. That is the
             * "struggles to hit it" half of the complaint, and it is separate
             * from the target being too high.
             *
             * tg = 0 leaves such a mark alone rather than warping it on a
             * period that is not a period. SPFY_PROSODY_NAT_GUARD=0 restores
             * the old behaviour; the stage is off by default either way. */
            /* The warp's upward ceiling, so the dumps below report what the
             * engine ACTUALLY applies rather than the symmetric limit. */
            double warp_max_up = 0.0, warp_dz = 0.0;
            {
                const char *mu = spfy_env("SPFY_PROSODY_MAX_UP_ST");
                if (mu && *mu) warp_max_up = strtod(mu, NULL);
                const char *dz = spfy_env("SPFY_PROSODY_DEADZONE_ST");
                if (dz && *dz) warp_dz = strtod(dz, NULL);
            }
            const int nat_guard = !env_flag_off("SPFY_PROSODY_NAT_GUARD");
            const float nat_min_hz = env_f("SPFY_PROSODY_NAT_MIN_HZ", 50.0f);
            const float nat_max_hz = env_f("SPFY_PROSODY_NAT_MAX_HZ", 400.0f);
            const int lead_in_fix = !env_flag_off("SPFY_PROSODY_PM_LEADIN");
            /* Diagnostic only, and gated because spfy_contour_st_at() walks
             * every accent and valley -- worth one getenv, not one extra
             * traversal per mark on the shipped path. */
            static int want_stc = -1;
            if (want_stc < 0)
                want_stc = (spfy_env("SPFY_PROSODY_MARK_DUMP") != NULL);
            for (int i = 0; i < nm; ++i) {
                t += (double)mk[i];
                tgraw[i] = 0.0f;
                stc[i] = 0.0f;
                why[i] = 0u;
                if (want_stc)
                    stc[i] = spfy_contour_st_at(&pros->contour, t);
                if (paum[i]) { tg[i] = 0.0f; why[i] = 1u; continue; }
                /* natural F0 at this mark is the reciprocal of its own
                 * period; in RELATIVE mode the contour rides on it, so
                 * zeroed parameters warp at ratio 1.0 (bit-identical). */
                float nat = (mk[i] > 0)
                          ? (float)((double)sample_rate / (double)mk[i])
                          : 0.0f;
                /* Degenerate pitch marks produce absurd "natural" F0, and
                 * in ABSOLUTE mode that lands straight in the warp ratio. */
                if (nat_guard && nat > 0.0f
                    && (nat < nat_min_hz || nat > nat_max_hz)) {
                    tg[i] = 0.0f;
                    why[i] = 2u;
                    continue;
                }
                tg[i] = spfy_contour_at(&pros->contour, t, nat);
                tgraw[i] = tg[i];
                if (tg[i] <= 0.0f) why[i] = (mk[i] > 0) ? 6u : 5u;
                /* ABSOLUTE F0 BOUNDS, which max_st does not provide. */
                if (tg[i] > 0.0f) {
                    const float knee = env_f("SPFY_PROSODY_F0_KNEE_ST", 2.0f);
                    const float flo  = env_f("SPFY_PROSODY_F0_FLOOR_HZ", 0.0f);
                    const float cei  = env_f("SPFY_PROSODY_F0_CEIL_HZ", 0.0f);
                    if (flo > 0.0f && tg[i] < flo) {
                        double d = 12.0 * log2((double)flo / (double)tg[i]);
                        double c = (knee > 0.0f)
                                 ? (double)knee * (1.0 - exp(-d / knee))
                                 : 0.0;
                        tg[i] = (float)((double)flo / pow(2.0, c / 12.0));
                    } else if (cei > 0.0f && tg[i] > cei) {
                        double d = 12.0 * log2((double)tg[i] / (double)cei);
                        double c = (knee > 0.0f)
                                 ? (double)knee * (1.0 - exp(-d / knee))
                                 : 0.0;
                        tg[i] = (float)((double)cei * pow(2.0, c / 12.0));
                    }
                }
            }
            /* LEAD-IN MARKS. */
            if (lead_in_fix) {
                for (int i = 0; i < nm; ++i) {
                    if (!lead[i] || mk[i] <= 0) continue;
                    int j = i + 1;
                    if (j >= nm || mk[j] <= 0 || tg[j] <= 0.0f) {
                        tg[i] = 0.0f;
                        tgraw[i] = 0.0f;
                        why[i] = 4u;
                        continue;
                    }
                    double nat_j = (double)sample_rate / (double)mk[j];
                    double nat_i = (double)sample_rate / (double)mk[i];
                    tg[i] = (float)(nat_i * ((double)tg[j] / nat_j));
                    tgraw[i] = (tgraw[j] > 0.0f)
                             ? (float)(nat_i * ((double)tgraw[j] / nat_j))
                             : tg[i];
                    why[i] = 3u;
                }
            }
            /* SPFY_PROSODY_WARP_DUMP=1: per-unit requested vs CLAMPED
             * shift, so a "robotic tail" can be attributed rather than
             * guessed. */
            if (spfy_env("SPFY_PROSODY_WARP_DUMP")) {
                /* Report the APPLIED shift beside the requested one. */
                const char *sv = spfy_env("SPFY_PROSODY_SOFT_ST");
                const int soft = !(sv && *sv == '0');
                const double lim = (double)pros->contour.p.max_st;
                double lo = 99.0, hi = -99.0, sum = 0.0, asum = 0.0;
                double alo = 99.0;
                int pin = 0, cnt = 0;
                for (int i = 0; i < nm; ++i) {
                    if (mk[i] <= 0 || tg[i] <= 0.0f) continue;
                    double nat = (double)sample_rate / (double)mk[i];
                    double st  = 12.0 * log2((double)tg[i] / nat);
                    /* Same limiter the WARP uses, asymmetry included. */
                    double ar  = spfy_prosody_deadzone(
                                     spfy_prosody_limit_ratio2(
                                         (double)tg[i] / nat, warp_max_up,
                                         lim, soft), warp_dz);
                    double ast = 12.0 * log2(ar);
                    if (st < lo) lo = st;
                    if (st > hi) hi = st;
                    if (ast < alo) alo = ast;
                    sum += st;
                    asum += ast;
                    if (st < -lim + 1e-6 || st > lim - 1e-6) ++pin;
                    ++cnt;
                }
                if (cnt)
                    fprintf(stderr,
                            "[warp] uid=%6u run=%u marks=%3d  want avg=%+6.2f "
                            "min=%+6.2f max=%+6.2f  got avg=%+6.2f "
                            "min=%+6.2f  pinned=%d/%d%s\n",
                            ref->uid, ref->run_n, cnt, sum / cnt, lo, hi,
                            asum / cnt, alo, pin, cnt,
                            pin ? "  <-- CLAMPED" : "");
            }
            /* SPFY_PROSODY_MARK_DUMP=1: one line per pitch mark, carrying
             * the engine's OWN arithmetic instead of an estimate of it. */
            if (spfy_env("SPFY_PROSODY_MARK_DUMP")) {
                const char *msv = spfy_env("SPFY_PROSODY_SOFT_ST");
                const int msoft = !(msv && *msv == '0');
                const double mlim = (double)pros->contour.p.max_st;
                double tn = (double)ref->nom_pos;
                /* Output-sample anchor. */
                const unsigned long wpos =
                    ws && ws->wav
                    ? (unsigned long)(ws->wav->n_samples_written
                                      + (ws->engine_mode ? ws->hist_n
                                                         : ws->tail_n))
                    : 0UL;
                for (int i = 0; i < nm; ++i) {
                    double nat, ar;
                    tn += (double)mk[i];
                    if (mk[i] <= 0) continue;
                    nat = (double)sample_rate / (double)mk[i];
                    ar = (tg[i] > 0.0f)
                       ? spfy_prosody_deadzone(
                             spfy_prosody_limit_ratio2((double)tg[i] / nat,
                                                       warp_max_up, mlim,
                                                       msoft), warp_dz)
                       : 1.0;
                    /* nominal_n is emitted too because the marks describe
                     * the SOURCE recording's periods, while the engine
                     * emits nominal_n samples for the span. */
                    /* `pau` is emitted so the analysis side does not have
                     * to re-derive which units are silence -- it was
                     * excluding two hard-coded uids while the engine flags
                     * every unit carrying the pau phone. */
                    /* Fields 9-11, APPENDED: the raw contour target (pre
                     * floor/ceiling knee), the target actually used, and
                     * the branch that decided this mark. */
                    fprintf(stderr,
                            "[mark] %u %d %d %.8f %.1f %lu %lu %d "
                            "%.4f %.4f %d %.6f\n",
                            ref->uid, (int)lead[i], (int)mk[i], ar, tn, wpos,
                            (unsigned long)nominal_n, (int)paum[i],
                            (double)tgraw[i], (double)tg[i], (int)why[i],
                            (double)stc[i]);
                }
            }
            spfy_prosody_warp_unit(buf, n, mk, nm, tg,
                                   pros->contour.p.max_st, (int)sample_rate);
        }
    }
    /* Engine-faithful path. */
    static int legacy = -1;
    if (legacy < 0) legacy = (spfy_env("SPFY_WSOLA_LEGACY") != NULL);
    /* Paired with wsola.c's [wsolat] line under the same env var; emitted
     * first so the two interleave as (unit, push) on stderr. */
    static int ws_trace = -1;
    if (ws_trace < 0) ws_trace = (spfy_env("SPFY_WSOLA_TRACE") != NULL);
    if (ws_trace) {
        fprintf(stderr, "[wsolau] uid=%u file=%u lp=%u dur=%u run=%u npau=%u",
                ref ? ref->uid : 0u, file_idx, lp, dur,
                ref ? ref->run_n : 0u, n_pau);
        for (uint32_t k = 0; k < n_pau; ++k)
            fprintf(stderr, " pau%u=[off=%u nom=%u tgt=%u]", k,
                    pau[k].off, pau[k].nom, pau[k].tgt);
        fputc('\n', stderr);
    }
    if (!legacy) {
        rc = spfy_wsola_push_engine(ws, buf, n, pre_n, nominal_n);
    } else {
        spfy_wsola_set_next_pre(ws, (uint32_t)pre_n);
        rc = spfy_wsola_push_unit_psola(ws, buf, n, nominal_n, align,
                                        f0_tail, f0_head, sample_rate);
    }
    free(buf);
    return rc;
}


/* feat-order phone -> ccos labl index. */
static uint32_t phone_to_labl(const spfy_voice_t *v, uint32_t phone)
{
    if (!v || !v->phone_order.feat_to_labl ||
        phone >= v->phone_order.n_phones) return phone;
    uint8_t lab = v->phone_order.feat_to_labl[phone];
    return (lab == SPFY_PHONE_NONE) ? phone : lab;
}

/* Is this slice ctx[2] one of the voice's two `pau` half-phone classes? */
static int ctx_is_silence(const spfy_voice_t *v, uint32_t ctx2)
{
    uint32_t pau = v ? spfy_phone_order_index(&v->phone_order, "pau")
                     : SPFY_PHONE_NONE;
    uint32_t l = (pau == SPFY_PHONE_NONE) ? 64u : pau * 2u;
    return ctx2 == l || ctx2 == l + 1u;
}

/* Per-slot OUTPUT length in samples, mirroring the concat loop's emission
 * rules. */
/* Word/phrase event positions are captured in PRE-stretch sample time,
 * because spfy_wav_close() applies the time-scale after the whole utterance
 * is held. */
static uint32_t out_pos(const spfy_wav_writer_t *w, uint32_t pos)
{
    if (!w || w->stretch <= 0.0f || w->stretch == 1.0f) return pos;
    return (uint32_t)((double)pos / (double)w->stretch + 0.5);
}

static void prosody_slot_out_dur(const spfy_voice_t *v,
                                 const uint32_t *path_uids, uint32_t n_slots,
                                 const uint32_t *hp_to_post,
                                 const uint32_t (*ctx_arr)[5],
                                 const uint32_t *hp_word_idx,
                                 size_t silence_n, int no_run,
                                 uint32_t *out_dur)
{
    memset(out_dur, 0, (size_t)n_slots * sizeof *out_dur);
    uint32_t prev_word_idx = 0xFFFFFFFFu;
    uint32_t last_emitted  = 0xFFFFFFFFu;
    int      prev_have     = 0;
    uint32_t s = 0;
    while (s < n_slots) {
        uint32_t u = path_uids[s];
        if (u == 0xFFFFFFFFu) { ++s; continue; }
        if (u == 0 || u == SILENCE_UID(v) || u >= v->units.n_units) {
            ++s; prev_have = 0; continue;
        }
        spfy_unit_record_t r1;
        if (spfy_unit_record_get(&v->units, u, &r1) != SPFY_OK) {
            ++s; prev_have = 0; continue;
        }
        int this_is_silence = ctx_is_silence(v, ctx_arr[hp_to_post[s]][2]);
        uint32_t this_word_idx = hp_word_idx[s];
        if (silence_n > 0 && prev_have && prev_word_idx != 0xFFFFFFFFu
            && this_word_idx != prev_word_idx && !this_is_silence) {
            if (last_emitted < n_slots)
                out_dur[last_emitted] += (uint32_t)silence_n;
            prev_have = 0;
        }
        if (!this_is_silence) prev_word_idx = this_word_idx;

        uint32_t run_n = 1;
        if (!no_run) {
            while (s + run_n < n_slots) {
                uint32_t v_prev = path_uids[s + run_n - 1];
                uint32_t v_next = path_uids[s + run_n];
                if (v_next == 0 || v_next == SILENCE_UID(v)
                    || v_next >= v->units.n_units) break;
                if (v_next != v_prev + 1u) break;
                spfy_unit_record_t rn, rp;
                if (spfy_unit_record_get(&v->units, v_next, &rn) != SPFY_OK) break;
                if (spfy_unit_record_get(&v->units, v_prev, &rp) != SPFY_OK) break;
                if (rn.file_idx != rp.file_idx) break;
                if (rn.local_pos < rp.local_pos
                    || rn.local_pos > rp.local_pos + rp.dur_like + 64u) break;
                ++run_n;
            }
        }
        for (uint32_t k = 0; k < run_n; ++k) {
            spfy_unit_record_t rk, rn;
            uint32_t ms;
            if (spfy_unit_record_get(&v->units, path_uids[s + k], &rk)
                != SPFY_OK)
                continue;
            if (k + 1 < run_n
                && spfy_unit_record_get(&v->units, path_uids[s + k + 1], &rn)
                   == SPFY_OK
                && rn.local_pos >= rk.local_pos)
                ms = (uint32_t)rn.local_pos - (uint32_t)rk.local_pos;
            else
                ms = (uint32_t)rk.dur_like;
            out_dur[s + k] = ms * 8u;
        }
        last_emitted = s + run_n - 1;
        prev_have = 1;
        s += run_n;
    }
}

typedef struct {
    const spfy_fe_slot_t *slot;
    uint32_t              q5;
    const spfy_voice_t   *voice;
    int                   is_f0tr;  /* if 1, q3/q4/q5/q9 are clamped to 0 per engine's f0tr CART convention
 * (cart_walker_args trace shows these are always 0 when tree=='f0tr'). */
} cart_feat_ctx_t;

static int32_t cart_feat(uint32_t q_type, void *user)
{
    const cart_feat_ctx_t *c = (const cart_feat_ctx_t *)user;
    if (c->is_f0tr) {
        /* engine's f0tr CART is syllable-level - only q1, q2, q7, q8 are
         * populated. */
        if (q_type == 3 || q_type == 4 || q_type == 5 || q_type == 9)
            return 0;
    } else {
        /* engine's durt walker (FUN_08e87d90) executes XOR EBX,EBX before
         * each dispatcher call (verified disasm + cart_walker_args hook
         * - ebx field always 0). For q_type=7 the dispatcher reads EBX,
         * so q7 is forced to 0 in durt walks. (q3/q4/q5/q8/q9 come from
         * stack args populated by the walker, and slot-level sp[]/ctx[]
         * are the correct sources for those.) Without this clamp, our
         * durt walks at q_type=7 nodes use sp[2] and diverge from engine
         * - accounted for all 6 durt-mismatch slots in nat_036
         * (slot 7/10/12/16/18/20). */
        if (q_type == 7)
            return 0;
    }
    /* From src/usel/build_graph.h: q_type -> source mapping decoded from
     * the engine's CART walker (FUN_08e87c90):
     *   1: workspace+0x28 = sylType        = sp[1]
     *   2: workspace+0x2c = sylInPhrase    = sp[0]
     *   3: s_ctx_remap[ctx[1]] = LEFT phone label (with Tom swap)
     *   4: s_ctx_remap[ctx[3]] = RIGHT phone label (with Tom swap)
     *   5: halfphones-in-current-syllable (precomputed in c->q5)
     *   7: workspace+0x34 = sylInWord      = sp[2]
     *   8: workspace+0x38 = wordInPhrase   = sp[3]
     *   9: workspace+0x3c = phoneInSyl     = sp[4]
     */
    int32_t v;
    switch (q_type) {
        case 1: v = (int32_t)c->slot->sp[1]; break;
        case 2: v = (int32_t)c->slot->sp[0]; break;
        case 3: v = (int32_t)phone_to_labl(c->voice,
                                           (uint32_t)c->slot->ctx[1] >> 1); break;
        case 4: v = (int32_t)phone_to_labl(c->voice,
                                           (uint32_t)c->slot->ctx[3] >> 1); break;
        case 5: v = (int32_t)c->q5; break;
        case 7: v = (int32_t)c->slot->sp[2]; break;
        case 8: v = (int32_t)c->slot->sp[3]; break;
        case 9: v = (int32_t)c->slot->sp[4]; break;
        default: v = 0; break;
    }
    return v;
}

/* Compute the q5 (halfphones-in-syllable) array. */
__attribute__((unused)) static
void compute_q5_per_slot(const spfy_fe_utterance_t *utt,
                                 uint32_t *q5_out)
{
    uint32_t i = 0;
    while (i < utt->n_slots) {
        uint32_t j = i;
        uint32_t s2 = utt->slots[i].sp[2];
        uint32_t s3 = utt->slots[i].sp[3];
        while (j + 1 < utt->n_slots
               && utt->slots[j+1].sp[2] == s2
               && utt->slots[j+1].sp[3] == s3) {
            ++j;
        }
        uint32_t run = j - i + 1;
        for (uint32_t k = i; k <= j; ++k) {
            int silence = (utt->slots[k].ctx[2] == 64
                           || utt->slots[k].ctx[2] == 65);
            q5_out[k] = silence ? 1u : run;
        }
        i = j + 1;
    }
}

/* Detect the phrase-terminating punctuation in the input text. */
static char detect_phrase_term(const char *text)
{
    char term = '.';
    for (size_t i = strlen(text); i > 0; --i) {
        char c = text[i - 1];
        if (c == '.' || c == '?' || c == '!' || c == ',') { term = c; break; }
        if (!isspace((unsigned char)c)) break;
    }
    return term;
}

/* Forward decl: is_arpa_vowel is defined in the post-ifdef block but used
 * by BOTH delta_to_fe_utt (non-hosted) AND parsed_to_fe_utt (hosted,
 * max-onset re-syllabification). */
static int is_arpa_vowel(const char *a);

/* Classify a ToBI accent string's phrase-BOUNDARY tone into an F0 ramp
 * endpoint in signed semitones (relative to the syllable's carrier). */
static int boundary_tone_target_st(const char *accent)
{
    if (!accent || !*accent) return 0;
    const char *pct = strchr(accent, '%');
    if (!pct || pct < accent + 2) return 0;
    char hi = pct[-1];
    char lo = pct[-2];
    if (lo == '-' && pct >= accent + 3) lo = pct[-3];
    if (lo == 'L' && hi == 'L') return -6;
    if (lo == 'L' && hi == 'H') return +4;
    if (lo == 'H' && hi == 'L') return -3;
    if (lo == 'H' && hi == 'H') return +6;
    return 0;
}

/* Classify the STARRED pitch-accent part of a ToBI accent string into a
 * nominal F0 bias in signed semitones, relative to a generic H* (so the
 * stock FE's near-universal H* maps to 0 and stays put). */
static int accent_type_st(const char *accent)
{
    if (!accent || !*accent) return 0;
    const char *star = strchr(accent, '*');
    if (!star) return 0;
    const char *semi = strchr(accent, ';');
    if (semi && star > semi) return 0;
    /* Longest-match against the token that ends at '*' (plus the L*+H
     * lookahead). */
    if (strncmp(accent, "H+!H*", 5) == 0) return -2;
    if (strncmp(accent, "L+H*", 4) == 0)  return +2;
    if (strncmp(accent, "L*+H", 4) == 0)  return -3;
    if (strncmp(accent, "!H*", 3) == 0)   return -2;
    if (strncmp(accent, "L*", 2) == 0)    return -5;
    if (strncmp(accent, "H*", 2) == 0)    return 0;
    return 0;
}

/* Is this phone a syllable NUCLEUS -- the thing that actually carries
 * stress? */
static int phone_is_nucleus(const char *n)
{
    static const char *const NUC[] = {
        "aa", "ae", "ah", "ao", "aw", "ax", "axr", "ay", "eh", "er", "ey",
        "ih", "ix", "iy", "ow", "oy", "uh", "uw", "ux", "el", "em", "en",
    };
    if (!n || !*n) return 0;
    for (size_t i = 0; i < sizeof NUC / sizeof *NUC; ++i)
        if (strcmp(n, NUC[i]) == 0) return 1;
    return 0;
}

/* The inline \![ToBI:] vocabulary. */
static const struct {
    const char *name;
    uint8_t     code;
    uint8_t     accented;
    int8_t      bias;
} SPFY_TOBI_TAGS[] = {
    { "H+!H*", 6, 1, -2 },
    { "L*+H",  7, 1, -3 },
    { "L+H*",  4, 1, +2 },
    { "!H*",   5, 1, -2 },
    { "H*",    1, 1,  0 },
    { "L*",    2, 1, -5 },
    { "NONE",  3, 0,  0 },
    { "0",     3, 0,  0 },
};
#define SPFY_N_TOBI_TAGS (sizeof SPFY_TOBI_TAGS / sizeof *SPFY_TOBI_TAGS)

static int spfy_tobi_by_code(uint8_t code, uint8_t *accented, int8_t *bias)
{
    for (size_t i = 0; i < SPFY_N_TOBI_TAGS; ++i) {
        if (SPFY_TOBI_TAGS[i].code != code) continue;
        *accented = SPFY_TOBI_TAGS[i].accented;
        *bias     = SPFY_TOBI_TAGS[i].bias;
        return 1;
    }
    return 0;
}


/* Hosted FE: build spfy_fe_utt_t directly from the parser's per-word
 * structure for a single phrase_id. */
static int parsed_to_fe_utt(const fe_parsed_t *parsed,
                            const char        *original_text,
                            int                phrase_id,
                            spfy_fe_utt_t     *out)
{
    memset(out, 0, sizeof *out);

    int n_words_phr = 0, n_syls_phr = 0, n_segs_phr = 0;
    for (int i = 0; i < parsed->n_words; i++) {
        if (parsed->words[i].phrase_id != phrase_id) continue;
        n_words_phr++;
        n_syls_phr += parsed->words[i].n_syllables;
        n_segs_phr += parsed->words[i].n_phonemes;
    }
    if (n_words_phr == 0) return SPFY_E_INVAL;

    uint32_t total_words = (uint32_t)n_words_phr + 2u;
    uint32_t total_syls  = (uint32_t)n_syls_phr  + 2u;
    uint32_t total_segs  = (uint32_t)n_segs_phr  + 2u;

    out->n_words     = total_words;
    out->n_syls      = total_syls;
    out->n_segs      = total_segs;
    /* Per-phrase terminator captured by the parser. */
    if (phrase_id >= 0 && phrase_id < parsed->n_phrase_terms
        && parsed->phrase_terms[phrase_id] != 0) {
        out->phrase_term = parsed->phrase_terms[phrase_id];
    } else {
        out->phrase_term = detect_phrase_term(original_text);
    }

    out->word_shareds = (uint32_t *)calloc(total_words, sizeof *out->word_shareds);
    out->word_names   = (char    **)calloc(total_words, sizeof *out->word_names);
    out->word_n_syls  = (uint32_t *)calloc(total_words, sizeof *out->word_n_syls);
    out->word_syls    = (uint32_t **)calloc(total_words, sizeof *out->word_syls);
    out->syl_stress   = (int32_t  *)calloc(total_syls,  sizeof *out->syl_stress);
    out->syl_accent   = (uint32_t *)calloc(total_syls,  sizeof *out->syl_accent);
    out->syl_btone    = (int8_t   *)calloc(total_syls,  sizeof *out->syl_btone);
    out->syl_acctype  = (int8_t   *)calloc(total_syls,  sizeof *out->syl_acctype);
    out->syl_cont_prev= (uint8_t  *)calloc(total_syls,  sizeof *out->syl_cont_prev);
    out->syl_n_segs   = (uint32_t *)calloc(total_syls,  sizeof *out->syl_n_segs);
    out->syl_segs     = (uint32_t **)calloc(total_syls,  sizeof *out->syl_segs);
    if (!out->word_shareds || !out->word_names || !out->word_n_syls
        || !out->word_syls || !out->syl_stress || !out->syl_accent
        || !out->syl_btone || !out->syl_acctype || !out->syl_cont_prev
        || !out->syl_n_segs || !out->syl_segs) {
        spfy_fe_utt_free(out); return SPFY_E_NOMEM;
    }

    out->word_shareds[0] = 1;
    out->word_names[0]   = strdup("_NULL_");
    out->word_n_syls[0]  = 1;
    out->word_syls[0]    = (uint32_t *)calloc(1, sizeof **out->word_syls);
    if (!out->word_names[0] || !out->word_syls[0]) {
        spfy_fe_utt_free(out); return SPFY_E_NOMEM;
    }
    out->word_syls[0][0] = 1;
    out->syl_stress[0]   = 0;
    out->syl_accent[0]   = 0;
    out->syl_n_segs[0]   = 1;
    out->syl_segs[0]     = (uint32_t *)calloc(1, sizeof **out->syl_segs);
    if (!out->syl_segs[0]) { spfy_fe_utt_free(out); return SPFY_E_NOMEM; }
    out->syl_segs[0][0]  = 1;

    uint32_t next_word_shared = 2, next_syl_shared = 2, next_seg_shared = 2;
    uint32_t syl_g_idx = 1;
    uint32_t word_out_idx = 1;

    for (int wi = 0; wi < parsed->n_words; wi++) {
        const fe_parsed_word_t *w = &parsed->words[wi];
        if (w->phrase_id != phrase_id) continue;

        out->word_shareds[word_out_idx] = next_word_shared++;
        out->word_names[word_out_idx]   = strdup(w->text);
        out->word_n_syls[word_out_idx]  = (uint32_t)w->n_syllables;
        if (w->n_syllables > 0) {
            out->word_syls[word_out_idx] = (uint32_t *)calloc(
                (size_t)w->n_syllables, sizeof **out->word_syls);
            if (!out->word_names[word_out_idx] || !out->word_syls[word_out_idx]) {
                spfy_fe_utt_free(out); return SPFY_E_NOMEM;
            }
        }

        /* Walk syllables of this word (0..n_syllables-1) and partition its
         * phoneme list by syl_index. */
        for (int si = 0; si < w->n_syllables; si++) {
            uint32_t this_syl_shared = next_syl_shared++;
            out->word_syls[word_out_idx][si] = this_syl_shared;

            int first_pi = -1, last_pi = -1;
            for (int pi = 0; pi < w->n_phonemes; pi++) {
                if (w->phonemes[pi].syl_index == si) {
                    if (first_pi < 0) first_pi = pi;
                    last_pi = pi;
                }
            }
            if (first_pi < 0) {
                out->syl_stress[syl_g_idx] = 0;
                out->syl_accent[syl_g_idx] = 0;
                out->syl_n_segs[syl_g_idx] = 0;
                syl_g_idx++;
                continue;
            }
            const fe_parsed_phoneme_t *ph0 = &w->phonemes[first_pi];
            out->syl_stress[syl_g_idx] = (int32_t)ph0->syl_stress;
            out->syl_accent[syl_g_idx] =
                (ph0->accent[0] && strchr(ph0->accent, '*')) ? 1u : 0u;
            out->syl_acctype[syl_g_idx] = (int8_t)accent_type_st(ph0->accent);
            /* Mark the fr-CA liaison syllable. */
            out->syl_cont_prev[syl_g_idx] =
                (w->first_syl_implicit && si == 0 && word_out_idx > 1) ? 1u : 0u;
            /* Phrase-boundary tone: scan the syllable's phonemes for a ToBI
             * boundary marker (it usually rides the last phoneme). */
            {
                int bt = 0;
                for (int pi = first_pi; pi <= last_pi && bt == 0; pi++)
                    bt = boundary_tone_target_st(w->phonemes[pi].accent);
                out->syl_btone[syl_g_idx] = (int8_t)bt;
            }

            /* first_pi and last_pi are assigned together in the scan above,
             * and the first_pi < 0 case already `continue`d, so the true
             * count is [1, n_phonemes]. */
            int n_seg_signed = last_pi - first_pi + 1;
            if (n_seg_signed < 0)                n_seg_signed = 0;
            if (n_seg_signed > w->n_phonemes)    n_seg_signed = w->n_phonemes;
            uint32_t n_seg_in_syl = (uint32_t)n_seg_signed;
            out->syl_n_segs[syl_g_idx] = n_seg_in_syl;
            out->syl_segs[syl_g_idx] = (uint32_t *)calloc(
                n_seg_in_syl, sizeof **out->syl_segs);
            if (!out->syl_segs[syl_g_idx]) {
                spfy_fe_utt_free(out); return SPFY_E_NOMEM;
            }
            for (uint32_t j = 0; j < n_seg_in_syl; j++) {
                out->syl_segs[syl_g_idx][j] = next_seg_shared++;
            }
            syl_g_idx++;
        }
        word_out_idx++;
    }

    uint32_t tail_w = total_words - 1u;
    uint32_t tail_s = total_syls  - 1u;
    out->word_shareds[tail_w] = next_word_shared++;
    out->word_names[tail_w]   = strdup("_NULL_");
    out->word_n_syls[tail_w]  = 1;
    out->word_syls[tail_w]    = (uint32_t *)calloc(1, sizeof **out->word_syls);
    if (!out->word_names[tail_w] || !out->word_syls[tail_w]) {
        spfy_fe_utt_free(out); return SPFY_E_NOMEM;
    }
    out->word_syls[tail_w][0] = next_syl_shared++;
    out->syl_stress[tail_s]   = 0;
    out->syl_accent[tail_s]   = 0;
    out->syl_n_segs[tail_s]   = 1;
    out->syl_segs[tail_s]     = (uint32_t *)calloc(1, sizeof **out->syl_segs);
    if (!out->syl_segs[tail_s]) { spfy_fe_utt_free(out); return SPFY_E_NOMEM; }
    out->syl_segs[tail_s][0]  = next_seg_shared++;

    /* Speechify-4 deaccenting (the "hat pattern"). */
    {
        static int deacc_mode = -1;
        static int deacc_bias = 0;
        if (deacc_mode < 0) {
            const char *e = spfy_env("SPFY_SPFY4_DEACCENT");
            deacc_mode = 0;
            if (e && *e && strcmp(e, "0") != 0)
                deacc_mode = (strcmp(e, "first") == 0) ? 2 : 1;
            /* Stripping the accent flag alone often loses to coverage: the
             * DB's renditions of NWR template words are accented-sounding
             * and win on join costs regardless of class. */
            const char *b = spfy_env("SPFY_SPFY4_DEACCENT_BIAS");
            deacc_bias = (b && *b) ? atoi(b) : -4;
            if (deacc_bias < -12) deacc_bias = -12;
            if (deacc_bias > 0)   deacc_bias = 0;
        }
        if (deacc_mode > 0) {
            uint32_t first = 0, last = 0, n_acc = 0;
            for (uint32_t g = 1; g + 1 < total_syls; ++g) {
                if (out->syl_accent[g]) {
                    if (!n_acc) first = g;
                    last = g;
                    ++n_acc;
                }
            }
            if (n_acc > (uint32_t)(deacc_mode == 1 ? 2 : 1)) {
                for (uint32_t g = 1; g + 1 < total_syls; ++g) {
                    if (!out->syl_accent[g] || g == first) continue;
                    if (deacc_mode == 1 && g == last) continue;
                    out->syl_accent[g]  = 0;
                    out->syl_acctype[g] = (int8_t)deacc_bias;
                }
            }
        }
    }
    return SPFY_OK;
}

/* Build the ARPAbet segment-name list directly from the hosted parser
 * output for a single phrase. */
static int build_segments_from_parsed(const fe_parsed_t *parsed,
                                      int                phrase_id,
                                      const char       ***out, uint32_t *out_n)
{
    int n_phons = 0;
    for (int wi = 0; wi < parsed->n_words; wi++) {
        if (parsed->words[wi].phrase_id == phrase_id)
            n_phons += parsed->words[wi].n_phonemes;
    }
    uint32_t total = (uint32_t)n_phons + 2u;
    const char **arr = (const char **)calloc(total, sizeof *arr);
    if (!arr) return SPFY_E_NOMEM;
    arr[0]            = "pau";
    arr[total - 1u]   = "pau";
    uint32_t k = 1;
    for (int wi = 0; wi < parsed->n_words; wi++) {
        const fe_parsed_word_t *w = &parsed->words[wi];
        if (w->phrase_id != phrase_id) continue;
        for (int pi = 0; pi < w->n_phonemes; pi++) {
            arr[k++] = w->phonemes[pi].arpabet;
        }
    }
    *out   = arr;
    *out_n = total;
    return SPFY_OK;
}


static int is_arpa_vowel(const char *a)
{
    if (!a) return 0;
    static const char *vs[] = {"aa","ae","ah","ao","aw","ax","ay","eh","er",
                                "ey","ih","ix","iy","ow","oy","uh","uw"};
    for (size_t i = 0; i < sizeof vs / sizeof vs[0]; ++i) {
        if (strcmp(a, vs[i]) == 0) return 1;
    }
    return 0;
}


typedef struct {
    const spfy_hash_t       *hash;
    const spfy_unit_table_t *units;
    float                    miss_default;
    /* Engine-faithful F0-prob curve (VIN `hist` chunk + voice+0xc8). */
    const uint8_t           *curve;
    int32_t                  curve_max_idx;
    int32_t                  curve_sub_off;
    float                    f0_edge_change_weight;
    float                    missing_join_cost;
    /* SPFY_JOIN_W_APPLY: apply `cost = w*cell + off` to hash-HIT cells. */
    int                      apply_join_w;
    float                    join_w;
    float                    join_off;
    /* --- SPFY_DP_F0_CONT: F0-continuity term (NOT engine behaviour) -----
     * Per-uid TRUE F0 from the pitch marks (spfy_reselect_build), so the DP
     * can price the pitch STEP across a join. */
    /* --- SPFY_POW_CONT_W: energy-continuity term (NOT engine behaviour)
     * --- Per-uid log-RMS measured from the VDB, so the DP can price the
     * ENERGY step across a join. */
    const float             *pow;
    uint32_t                 pow_n;
    float                    pow_cont_w;
    float                    pow_cont_dead;
    const float             *f0;
    uint32_t                 f0_n;
    float                    f0_cont_w;
    float                    f0_cont_dead;
    float                    f0_cont_up;
    /* Anti-dodge. */
    float                    f0_cont_break;
    /* Expected semitone step per voiced join; the penalty is on the
     * DEVIATION from it, |dF0 - slope|, not on |dF0|. */
    float                    f0_cont_slope;
    /* Which joins the term is allowed to price. */
    int                      f0_cont_scope;
    /* Extra cost on any join that is NOT a same-rec continuation, i.e. */
    float                    f0_cont_seam;
} join_ctx_t;

/* NB: the VCF's JOIN_COST_WEIGHT / JOIN_COST_OFFSET are deliberately NOT
 * applied to a hash-HIT cell. */

/* Parse VIN `hist` sub-chunks (head + data) and populate the curve params. */
static void load_f0_hist_curve(const spfy_vin_t *vin, join_ctx_t *jc)
{
    jc->curve = NULL;
    jc->curve_max_idx = 0;
    jc->curve_sub_off = 0;
    if (!vin || !vin->hist || vin->hist_n < 16) return;
    /* `head` 8 bytes (max_idx, sub_off) then `data` n*4 bytes. */
    const uint8_t *p   = vin->hist;
    const uint8_t *end = vin->hist + vin->hist_n;
    while (p + 8 <= end) {
        uint32_t fcc = (uint32_t)p[0] | ((uint32_t)p[1] << 8)
                     | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        uint32_t sz  = (uint32_t)p[4] | ((uint32_t)p[5] << 8)
                     | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        const uint8_t *body = p + 8;
        if (body + sz > end) return;
        if (fcc == 0x64616568u  && sz >= 8) {
            uint32_t mx, off;
            memcpy(&mx,  body,     4);
            memcpy(&off, body + 4, 4);
            jc->curve_max_idx = (int32_t)mx;
            jc->curve_sub_off = (int32_t)off;
        } else if (fcc == 0x61746164u ) {
            jc->curve = body;
        }
        p = body + sz;
        if (sz & 1) ++p;
    }
}

/* --- SPFY_POW_CONT_W support: per-uid log-RMS energy, measured -----------
 * NOT engine behaviour. */
/* --- SPFY_POW_TGT_W support: the VIN `mean` chunk ------------------------
 * 92 phone-variants x 8 features: duration, dur_spread, pitch,
 * pitch_spread, voice, voice_spread, power, power_spread. */
static int load_mean_power(const spfy_vin_t *vin, float **out_mean,
                           float **out_sd, uint32_t *out_rows)
{
    *out_mean = NULL; *out_sd = NULL; *out_rows = 0u;
    if (!vin || !vin->mean || vin->mean_n < 8) return SPFY_E_OOB;
    uint32_t n_rows, n_cols;
    memcpy(&n_rows, vin->mean,     4);
    memcpy(&n_cols, vin->mean + 4, 4);
    if (n_rows == 0u || n_cols < 8u) return SPFY_E_OOB;
    if ((size_t)n_rows * n_cols * 4u + 8u != vin->mean_n) return SPFY_E_OOB;
    float *mu = (float *)malloc((size_t)n_rows * sizeof *mu);
    float *sd = (float *)malloc((size_t)n_rows * sizeof *sd);
    if (!mu || !sd) { free(mu); free(sd); return SPFY_E_NOMEM; }
    const uint8_t *d = vin->mean + 8;
    for (uint32_t r = 0; r < n_rows; ++r) {
        const uint8_t *row = d + (size_t)r * n_cols * 4u;
        memcpy(&mu[r], row + 6 * 4, 4);
        memcpy(&sd[r], row + 7 * 4, 4);
    }
    *out_mean = mu; *out_sd = sd; *out_rows = n_rows;
    return SPFY_OK;
}

static int cmp_float_asc(const void *a, const void *b)
{
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}

static float *build_unit_power(const spfy_voice_t *v, uint32_t *out_n,
                               uint32_t *out_measured)
{
    if (out_n)        *out_n        = 0;
    if (out_measured) *out_measured = 0;
    if (!v || v->units.n_units == 0) return NULL;

    uint32_t n   = v->units.n_units;
    float   *pow = (float *)calloc(n, sizeof *pow);
    if (!pow) return NULL;

    uint32_t measured = 0;
    for (uint32_t uid = 0; uid < n; ++uid) {
        spfy_unit_record_t ur;
        if (spfy_unit_record_get(&v->units, uid, &ur) != SPFY_OK) continue;
        if (ur.dur_like == 0) continue;
        if (ur.file_idx >= v->feat.n_entries) continue;
        const spfy_feat_entry_t *fe = &v->feat.entries[ur.file_idx];
        uint32_t rec_off = 0, rec_size = 0;
        if (spfy_vdb_lookup_by_name(&v->lookup, fe->name, fe->name_len,
                                    &rec_off, &rec_size) != SPFY_OK) continue;
        uint32_t sps = v->vdb.sample_rate / 1000u;
        if (sps == 0u) sps = 8u;
        uint32_t bps = v->vdb.bytes_per_sample
                     ? v->vdb.bytes_per_sample : 1u;
        uint32_t rec_n = rec_size / bps;
        uint32_t off = (uint32_t)ur.local_pos * sps;
        if (off >= rec_n) continue;
        uint32_t len = (uint32_t)ur.dur_like * sps;
        /* Clamp rather than zero-pad: padding a short tail with silence
         * would bias that unit's RMS down for a reason that has nothing to
         * do with how loud it was recorded. */
        if (off + len > rec_n) len = rec_n - off;
        if (len == 0) continue;
        /* Decode rather than index the raw bytes: `sq` is a u-law lookup
         * and would be meaningless over PCM. */
        int16_t *sm = (int16_t *)malloc((size_t)len * sizeof *sm);
        if (!sm) continue;
        spfy_vdb_decode(&v->vdb, rec_off, off, len, sm);
        double acc = 0.0;
        for (uint32_t i = 0; i < len; ++i)
            acc += (double)sm[i] * (double)sm[i];
        free(sm);
        double rms = sqrt(acc / (double)len);
        if (rms <= 0.0) continue;
        pow[uid] = (float)log(rms);
        ++measured;
    }

    if (out_n)        *out_n        = n;
    if (out_measured) *out_measured = measured;
    return pow;
}

/* Read candidate i from whichever candidate pool is live for this slot. */
/* SPFY_UID_DUMP=<path>|- - the UID capture harness. */
static FILE *spfy_uid_dump_fp(void)
{
    static FILE *fp = NULL;
    static int   tried = 0;
    if (!tried) {
        tried = 1;
        const char *e = spfy_env("SPFY_UID_DUMP");
        if (e && *e) {
            if (!strcmp(e, "-") || !strcmp(e, "1")) {
                fp = stderr;
            } else {
                fp = fopen(e, "wb");
                if (!fp) {
                    spfy_log_warn("SPFY_UID_DUMP: cannot open '%s', "
                                  "falling back to stderr", e);
                    fp = stderr;
                }
            }
        }
    }
    return fp;
}

/* SPFY_UID_DUMP, emit half - WHERE each slot's audio actually landed. */
static void uid_dump_emit(const spfy_voice_t *v, uint32_t phrase_idx,
                          uint32_t slot0, const uint32_t *path_uids,
                          uint32_t run_n, uint32_t lp_first,
                          uint32_t out_start, const spfy_wav_writer_t *sink)
{
    FILE *uf = spfy_uid_dump_fp();
    if (!uf || !v || !path_uids) return;
    uint32_t sps = v->vdb.sample_rate / 1000u;
    if (sps == 0) sps = 1;
    for (uint32_t k = 0; k < run_n; ++k) {
        spfy_unit_record_t rk;
        uint32_t off = out_start;
        if (spfy_unit_record_get(&v->units, path_uids[slot0 + k], &rk)
                == SPFY_OK && (uint32_t)rk.local_pos >= lp_first)
            off += ((uint32_t)rk.local_pos - lp_first) * sps;
        fprintf(uf, "{\"t\":\"emit\",\"phrase\":%u,\"slot\":%u,\"uid\":%u,"
                    "\"out\":%u,\"run\":%u,\"k\":%u}\n",
                (unsigned)phrase_idx, (unsigned)(slot0 + k),
                (unsigned)path_uids[slot0 + k],
                (unsigned)out_pos(sink, off), (unsigned)run_n, (unsigned)k);
    }
}

/* SPFY_UID_OVERRIDE=<path> - force specific units into the chosen path.
 *
 * THE POINT: a recovered unit id is only a hypothesis until somebody hears
 * it. This renders the substitution so the claim can be judged by ear rather
 * than by an NCC table.
 *
 * It reads exactly the record type SPFY_UID_DUMP writes, so a dump can be
 * edited and fed straight back:
 *
 *   {"t":"pick","phrase":0,"slot":7,"uid":12345}
 *
 * Lines of any other type are ignored, which means an unedited dump
 * containing "cands" and "emit" records is a legal input and reproduces the
 * original path exactly. That is the identity case, and it is worth checking
 * before trusting any substitution.
 *
 * ⚠ THIS ONE DOES CHANGE THE AUDIO - unlike the dump, which is inert. It is
 * still off unless the variable is set, so the shipped path and the audit are
 * untouched, but it must never be left set during a comparison run.
 *
 * A substituted uid is NOT required to have been in the slot's pool: the
 * whole question is what a unit the DP rejected would have sounded like.
 * Run batching is left to re-derive itself downstream, so substituting a
 * uid+1 run reproduces the engine's own single-push behaviour naturally.
 */
struct uid_ovr { uint32_t phrase, slot, uid; };

static struct uid_ovr *g_uid_ovr;
static size_t          g_uid_ovr_n;

static void uid_override_load(void)
{
    static int tried = 0;
    if (tried) return;
    tried = 1;
    const char *path = spfy_env("SPFY_UID_OVERRIDE");
    if (!path || !*path) return;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        spfy_log_warn("SPFY_UID_OVERRIDE: cannot open '%s'", path);
        return;
    }
    char line[8192];
    size_t cap = 0;
    while (fgets(line, sizeof line, fp)) {
        if (!strstr(line, "\"t\":\"pick\"")) continue;
        const char *pp = strstr(line, "\"phrase\":");
        const char *sp = strstr(line, "\"slot\":");
        const char *up = strstr(line, "\"uid\":");
        if (!pp || !sp || !up) continue;
        long ph = strtol(pp + 9, NULL, 10);
        long sl = strtol(sp + 7, NULL, 10);
        long ud = strtol(up + 6, NULL, 10);
        if (ph < 0 || sl < 0 || ud < 0) continue;
        if (g_uid_ovr_n == cap) {
            size_t nc = cap ? cap * 2 : 256;
            struct uid_ovr *nb = (struct uid_ovr *)
                realloc(g_uid_ovr, nc * sizeof *nb);
            if (!nb) break;
            g_uid_ovr = nb; cap = nc;
        }
        g_uid_ovr[g_uid_ovr_n].phrase = (uint32_t)ph;
        g_uid_ovr[g_uid_ovr_n].slot   = (uint32_t)sl;
        g_uid_ovr[g_uid_ovr_n].uid    = (uint32_t)ud;
        ++g_uid_ovr_n;
    }
    fclose(fp);
    spfy_log_warn("SPFY_UID_OVERRIDE: %zu substitutions loaded from '%s'",
                  g_uid_ovr_n, path);
}

static void uid_override_apply(uint32_t phrase_idx, uint32_t *path_uids,
                               uint32_t n_slots)
{
    uid_override_load();
    if (!g_uid_ovr_n || !path_uids) return;
    uint32_t hit = 0;
    for (size_t i = 0; i < g_uid_ovr_n; ++i) {
        if (g_uid_ovr[i].phrase != phrase_idx) continue;
        if (g_uid_ovr[i].slot >= n_slots) continue;
        if (path_uids[g_uid_ovr[i].slot] == g_uid_ovr[i].uid) continue;
        path_uids[g_uid_ovr[i].slot] = g_uid_ovr[i].uid;
        ++hit;
    }
    if (hit && synth_is_verbose())
        spfy_log_warn("SPFY_UID_OVERRIDE: phrase %u, %u slots replaced",
                      (unsigned)phrase_idx, (unsigned)hit);
}

static uint32_t pool_cand(const uint8_t *prsl_pool,
                          const uint32_t *bucket_pool, uint32_t i)
{
    return prsl_pool ? spfy_prsl_cand(prsl_pool, i) : bucket_pool[i];
}

/* Engine-faithful FUN_08e8b620 join cost. Same-rec adjacent
 * (prev_join_key+1 == curr_uid && unit.flag_b) -> 0. Hash hit -> cell
 * cost. Hash miss + curve gate fires -> MISSING_JOIN_COST + F0_EDGE *
 * curve[clamp(curr_c6c - sub_off - prev_c7c, 0, max-1)]. Hash miss + no
 * gate -> MISSING_JOIN_COST + 0. Falls back to legacy miss_default if
 * the caller didn't supply curve params (curve == NULL). */
static float dag_join_cb(uint32_t prev_uid_join_key, uint32_t curr_uid,
                         uint32_t prev_slot, uint32_t prev_idx,
                         uint32_t curr_slot, uint32_t curr_idx,
                         int32_t  prev_c7c,  int32_t  prev_c80,
                         uint32_t curr_c6c,  void    *user)
{
    (void)prev_slot; (void)prev_idx; (void)curr_slot; (void)curr_idx;
    const join_ctx_t *jc = (const join_ctx_t *)user;
    float cost = 0.0f;
    const char *path = "";
    /* Which branch produced `cost`, for the F0-continuity scope test: 0
     * same_rec, 1 hash_hit, 2 miss/no_curve. */
    int path_kind = 2;
    if (curr_uid == prev_uid_join_key + 1u && curr_uid > 0u) {
        spfy_unit_record_t r;
        if (spfy_unit_record_get(jc->units, curr_uid, &r) == SPFY_OK
            && r.flag_b) {
            cost = 0.0f;
            path = "same_rec";
            path_kind = 0;
            goto dump;
        }
    }
    int rc = spfy_hash_lookup(jc->hash, prev_uid_join_key, curr_uid, &cost);
    if (rc == SPFY_OK) {
        if (jc->apply_join_w) {
            cost = jc->join_w * cost + jc->join_off;
            path = "hash_hit_w";
        } else {
            path = "hash_hit";
        }
        path_kind = 1;
        goto dump;
    }
    /* Hash miss - engine-exact (FUN_08e8b620):
     *   miss = MISSING_JOIN_COST + (gate ? F0_EDGE_CHANGE_WEIGHT * curve[idx]
     *                                    : CCOS_DEFAULT)
     * MISSING_JOIN_COST is 1000.0 when the VCF omits it -- which EVERY
     * shipped voice does. Verified in SWIttsUSel.dll: FUN_08e90dc0 stores
     * 0x447a0000 (1000.0) at +0x84 in the constructor, and the config read
     * at 08e9122b is `test eax,eax / jne` PAST the store, so a failed lookup
     * leaves the default standing. This comment used to say "For Tom:
     * MISSING_JOIN_COST=0 (VCF unset)", contradicting the loader 3500 lines
     * below (which has always passed 1000.0f) and inviting the reader to
     * "fix" a value that was already right.
     * CCOS_DEFAULT=0 (_DAT_08e9852c), F0_EDGE_CHANGE_WEIGHT=0.6 for Tom.
     *
     * Gate condition: curr.c6c > 20 AND prev.c80 < 15 AND prev.c7c > 20.
     * Curve idx: clamp((curr.c6c - sub_off) - prev.c7c, 0, max_idx-1). */
    if (!jc->curve) { cost = jc->miss_default; path = "no_curve"; goto dump; }
    {
        float curve_val = 0.0f;
        path = "miss_no_gate";
        if ((int32_t)curr_c6c > 20 && prev_c80 < 15 && prev_c7c > 20) {
            int32_t idx = (int32_t)curr_c6c - jc->curve_sub_off - prev_c7c;
            if (idx < 0) idx = 0;
            else if (idx >= jc->curve_max_idx) idx = jc->curve_max_idx - 1;
            curve_val = jc->f0_edge_change_weight
                      * spfy_le_f32(jc->curve + (size_t)idx * 4u);
            path = "miss_gate";
        }
        cost = jc->missing_join_cost + curve_val;
    }
dump:
    {
        /* F0-continuity surcharge, applied to EVERY path including the free
         * same-rec one. */
        float f0_step = 0.0f, f0_pen = 0.0f;
        /* Scope test: never charge a join the engine gets for free. */
        int in_scope = (jc->f0_cont_scope == 0)
                    || (jc->f0_cont_scope == 1 && path_kind != 0)
                    || (jc->f0_cont_scope >= 2 && path_kind == 2);
        if (in_scope && jc->f0
            && prev_uid_join_key < jc->f0_n && curr_uid < jc->f0_n) {
            float a = jc->f0[prev_uid_join_key];
            float b = jc->f0[curr_uid];
            if (a > 0.0f && b > 0.0f) {
                f0_step = 12.0f * log2f(b / a);
                /* Deviation from the expected step, so a steady decline is
                 * free and wandering is not. */
                float dev = f0_step - jc->f0_cont_slope;
                float mag = fabsf(dev) - jc->f0_cont_dead;
                if (mag > 0.0f) {
                    f0_pen = jc->f0_cont_w * mag;
                    /* Asymmetry: S4's phrase is one rise then a steady
                     * fall, so a prior that charges RISES more than falls
                     * pushes the path toward declination rather than merely
                     * toward flatness. */
                    if (dev > 0.0f) f0_pen *= jc->f0_cont_up;
                    cost += f0_pen;
                }
            } else if ((a > 0.0f) != (b > 0.0f) && jc->f0_cont_break > 0.0f) {
                f0_pen = jc->f0_cont_w * jc->f0_cont_break;
                cost += f0_pen;
            }
        }
        /* Seam price. */
        if (jc->f0 && jc->f0_cont_seam > 0.0f && path_kind != 0) {
            f0_pen += jc->f0_cont_seam;
            cost   += jc->f0_cont_seam;
        }
        /* ENERGY continuity. */
        if (jc->pow && jc->pow_cont_w > 0.0f
            && prev_uid_join_key < jc->pow_n && curr_uid < jc->pow_n) {
            float pa = jc->pow[prev_uid_join_key];
            float pb = jc->pow[curr_uid];
            if (pa > 0.0f && pb > 0.0f) {
                float mag = fabsf(pb - pa) - jc->pow_cont_dead;
                if (mag > 0.0f) cost += jc->pow_cont_w * mag;
            }
        }
        /* ⚠ ONE LINE PER EDGE CONSIDERED, not per join on the chosen path.
         * Resolve the stream ONCE -- this is the DP inner loop, and to stderr
         * (unbuffered) it is a syscall per line. Give SPFY_JOIN_DUMP a PATH. */
        static FILE *jd = NULL;
        static int   jd_init = 0;
        if (!jd_init) { jd = spfy_dump_stream("SPFY_JOIN_DUMP"); jd_init = 1; }
        if (jd) {
            fprintf(jd, "{\"join\":1,\"prev_slot\":%u,\"prev_idx\":%u,"
                            "\"curr_slot\":%u,\"curr_idx\":%u,"
                            "\"prev_jk\":%u,\"curr_uid\":%u,"
                            "\"prev_c7c\":%d,\"prev_c80\":%d,\"curr_c6c\":%u,"
                            "\"cost\":%.6f,\"path\":\"%s\","
                            "\"f0_step_st\":%.3f,\"f0_pen\":%.6f}\n",
                    prev_slot, prev_idx, curr_slot, curr_idx,
                    prev_uid_join_key, curr_uid,
                    prev_c7c, prev_c80, curr_c6c,
                    (double)cost, path,
                    (double)f0_step, (double)f0_pen);
        }
    }
    return cost;
}


static const char *spr_to_arpabet(char c)
{
    static const struct { char spr; const char *arpa; } TABLE[] = {
        {'a',"aa"}, {'A',"ae"}, {'H',"ah"}, {'c',"ao"}, {'W',"aw"},
        {'x',"ax"}, {'Y',"ay"}, {'b',"b"},  {'C',"ch"}, {'d',"d"},
        {'D',"dh"}, {'F',"dx"}, {'E',"eh"}, {'N',"en"}, {'R',"er"},
        {'e',"ey"}, {'f',"f"},  {'g',"g"},  {'h',"hh"}, {'I',"ih"},
        {'X',"ix"}, {'i',"iy"}, {'J',"jh"}, {'k',"k"},  {'l',"l"},
        {'m',"m"},  {'n',"n"},  {'G',"ng"}, {'o',"ow"}, {'O',"oy"},
        {'p',"p"},  {'r',"r"},  {'s',"s"},  {'S',"sh"}, {'t',"t"},
        {'T',"th"}, {'U',"uh"}, {'u',"uw"}, {'v',"v"},  {'w',"w"},
        {'y',"y"},  {'z',"z"},  {'Z',"zh"},
    };
    for (size_t i = 0; i < sizeof TABLE / sizeof TABLE[0]; ++i)
        if (TABLE[i].spr == c) return TABLE[i].arpa;
    return NULL;
}

/* SWIttsSSML.dll, UPSTREAM of the FE - the FE DLL has no tag parser    */

static const char *etag_digit_name(int c)
{
    static const char *D[10] = {"zero","one","two","three","four",
                                "five","six","seven","eight","nine"};
    return (c >= '0' && c <= '9') ? D[c - '0'] : NULL;
}

/* Spoken letter name for \!tsa / \!tsc. */
static const char *etag_letter_name(int c)
{
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c < 'a' || c > 'z') return NULL;
    static const char *L[26] = {
        "ay","bee","cee","dee","ee","eff","jee","aych","aye","jay",
        "kay","ell","emm","en","oh","pea","cue","ar","ess","tee",
        "you","vee","double you","eks","wy","zee"
    };
    return L[c - 'a'];
}

static const char *etag_radio_name(int c)
{
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (c < 'a' || c > 'z') return NULL;
    static const char *R[26] = {
        "alpha","bravo","charlie","delta","echo","foxtrot","golf",
        "hotel","india","juliet","kilo","lima","mike","november",
        "oscar","papa","quebec","romeo","sierra","tango","uniform",
        "victor","whiskey","xray","yankee","zulu"
    };
    return R[c - 'a'];
}

/* Spoken name of a punctuation/symbol char for all-character spellout
 * (\!tsc). */
static const char *etag_symbol_name(int c)
{
    switch (c) {
        case '-':  return "dash";       case ',':  return "comma";
        case '.':  return "period";     case '/':  return "slash";
        case '\\': return "backslash";  case '@':  return "at";
        case '#':  return "pound";      case '$':  return "dollar";
        case '%':  return "percent";    case '&':  return "and";
        case '*':  return "star";       case '+':  return "plus";
        case '=':  return "equals";     case '!':  return "exclamation point";
        case '?':  return "question mark"; case ':': return "colon";
        case ';':  return "semicolon";  case '(':  return "open paren";
        case ')':  return "close paren";case '[':  return "open bracket";
        case ']':  return "close bracket"; case '{': return "open brace";
        case '}':  return "close brace";case '<':  return "less than";
        case '>':  return "greater than"; case '\'': return "apostrophe";
        case '"':  return "quote";      case '_':  return "underscore";
        case '|':  return "bar";        case '~':  return "tilde";
        case '`':  return "backtick";   case '^':  return "caret";
        default:   return NULL;
    }
}

static const char *ETAG_ONES[20] = {
    "zero","one","two","three","four","five","six","seven","eight","nine",
    "ten","eleven","twelve","thirteen","fourteen","fifteen","sixteen",
    "seventeen","eighteen","nineteen"
};
static const char *ETAG_TENS[10] = {
    "","","twenty","thirty","forty","fifty","sixty","seventy","eighty","ninety"
};

static int etag_cardinal_2(int n, char *buf, size_t cap)
{
    if (n < 20)        return snprintf(buf, cap, "%s", ETAG_ONES[n]);
    if (n % 10 == 0)   return snprintf(buf, cap, "%s", ETAG_TENS[n / 10]);
    return snprintf(buf, cap, "%s %s", ETAG_TENS[n / 10], ETAG_ONES[n % 10]);
}

/* Cardinal (quantity) words for a 4-digit number 1000..9999, e.g. */
static int etag_cardinal_4(int n, char *buf, size_t cap)
{
    int th = n / 1000, hu = (n / 100) % 10, rest = n % 100;
    char *o = buf, *eo = buf + cap;
    o += snprintf(o, (size_t)(eo - o), "%s thousand", ETAG_ONES[th]);
    if (hu)   o += snprintf(o, (size_t)(eo - o), " %s hundred", ETAG_ONES[hu]);
    if (rest) {
        o += snprintf(o, (size_t)(eo - o), " ");
        o += etag_cardinal_2(rest, o, (size_t)(eo - o));
    }
    return (int)(o - buf);
}

/* If `p` begins with "\!"<kw> AND the char after <kw> is a valid tag
 * boundary (not alphanumeric - a tag "cannot be followed immediately by an
 * alphanumeric character"), return p advanced past the keyword; else NULL. */
static const char *etag_after(const char *p, const char *kw)
{
    if (p[0] != '\\' || p[1] != '!') return NULL;
    size_t kn = strlen(kw);
    if (strncmp(p + 2, kw, kn) != 0) return NULL;
    if (isalnum((unsigned char)p[2 + kn])) return NULL;
    return p + 2 + kn;
}

/* True when `text` carries an embedded \! */
static int spfy_etags_need_resolve(const char *text)
{
    /* Bare `[ToBI:` - see the resolver for why the `\!` may already be gone
     * by the time the text reaches us (Balabolka splits Speak() calls on
     * it). */
    if (strstr(text, "[ToBI:")) return 1;
    for (const char *q = text; (q = strstr(q, "\\!")) != NULL; q += 2) {
        int c = (unsigned char)q[2];
        if (c == '[') {
            /* `\![ToBI:...]` is ours and must trigger the resolver; every
             * other bracketed tag (\![SPR] ...) still passes through
             * verbatim. */
            if (strncmp(q + 3, "ToBI:", 5) == 0) return 1;
            continue;
        }
        if (c == 'p' && isdigit((unsigned char)q[3])) continue;
        return 1;
    }
    return 0;
}

/* Match `\!<vp|vd|rp|rd>(<digits>|r)` (volume/rate control) at p with a
 * valid tag boundary. */
static const char *etag_vr(const char *p, int *knd, int *rel, int *reset, int *val)
{
    if (p[0] != '\\' || p[1] != '!') return NULL;
    int k = (unsigned char)p[2], r = (unsigned char)p[3];
    if ((k != 'v' && k != 'r') || (r != 'p' && r != 'd')) return NULL;
    const char *q = p + 4;
    if (*q == 'r' && !isalnum((unsigned char)q[1])) {
        *knd = k; *rel = r; *reset = 1; *val = 0; return q + 1;
    }
    if (isdigit((unsigned char)*q)) {
        int n = 0; const char *d = q;
        while (isdigit((unsigned char)*d)) { n = n * 10 + (*d - '0'); d++; }
        if (isalpha((unsigned char)*d)) return NULL;
        *knd = k; *rel = r; *reset = 0; *val = n; return d;
    }
    return NULL;
}

/* Resolve eos / spellout / year embedded tags into plain text the FE
 * handles natively, AND emit parallel per-output-char volume/rate maps from
 * the \!vp/\!vd/\!rp/\!rd tags (consumed post-FE via each word's
 * char_start). */
static char *spfy_etags_resolve(const char *text,
                                uint16_t **out_vol, uint16_t **out_rate,
                                uint8_t **out_acc)
{
    size_t len = strlen(text);
    size_t cap = len * 16 + 256;
    char *out = (char *)malloc(cap);
    uint16_t *mvol = (uint16_t *)calloc(cap, sizeof *mvol);
    uint16_t *mrate = (uint16_t *)calloc(cap, sizeof *mrate);
    /* Per-char pitch-accent override from \![ToBI:...]. */
    uint8_t *macc = (uint8_t *)calloc(cap, sizeof *macc);
    if (!out || !mvol || !mrate || !macc) {
        free(out); free(mvol); free(mrate); free(macc); return NULL;
    }
    char *o = out, *eo = out + cap - 64;

    int sp = '0';
    int ny = '1';
    int last = 0;

    /* Volume/rate state. */
    int port_vol = 100, base_vol = 100, port_rate = 100, base_rate = 100;
    int pv = 100, pr = 100;
    /* Accent override is ONE-SHOT: it binds to the next word and then
     * clears, unlike volume and rate which stay in effect until changed. */
    int pa = 0, pa_started = 0;
    size_t filled = 0;
#define ETAG_FLUSH() do { size_t _e = (size_t)(o - out); \
        for (size_t _i = filled; _i < _e; _i++) { mvol[_i] = (uint16_t)pv; \
            mrate[_i] = (uint16_t)pr; macc[_i] = (uint8_t)pa; } \
        filled = _e; } while (0)

    const char *p = text;
    while (*p && o < eo) {
        const char *a;
        {
            int k, rel, rst, val;
            const char *a2 = etag_vr(p, &k, &rel, &rst, &val);
            if (a2) {
                ETAG_FLUSH();
                int basis = (rel == 'p') ? (k == 'v' ? port_vol : port_rate)
                                         : (k == 'v' ? base_vol : base_rate);
                int nv = rst ? basis : (val * basis / 100);
                if (k == 'v') { if (nv < 0) nv = 0; pv = nv; }
                else { if (nv < 33) nv = 33; if (nv > 300) nv = 300; pr = nv; }
                p = a2; continue;
            }
        }
        /* ---- \![ToBI:...] pitch-accent override, consumed, no output ----
         * Forces the FE's accent decision for the NEXT word. */
        /* BOTH SPELLINGS: `\![ToBI:...]` and a bare `[ToBI:...]`. */
        const char *tobi_body = NULL;
        if (p[0] == '\\' && p[1] == '!' && p[2] == '[' &&
            strncmp(p + 3, "ToBI:", 5) == 0)
            tobi_body = p + 8;
        else if (p[0] == '[' && strncmp(p + 1, "ToBI:", 5) == 0)
            tobi_body = p + 6;
        if (tobi_body) {
            const char *q = tobi_body;
            int val = 0;
            /* Longest-match over the whole vocabulary, not a first-character test. */
            for (size_t ti = 0; ti < SPFY_N_TOBI_TAGS; ++ti) {
                size_t n = strlen(SPFY_TOBI_TAGS[ti].name);
                if (strncmp(q, SPFY_TOBI_TAGS[ti].name, n) != 0) continue;
                val = SPFY_TOBI_TAGS[ti].code;
                q += n;
                break;
            }
            if (val && *q == ']') {
                ETAG_FLUSH();
                pa = val;
                pa_started = 0;
                p = q + 1;
                continue;
            }
            /* Unrecognised body: fall through and let it pass verbatim, the
             * same way \![SPR] does. */
        }
        if      ((a = etag_after(p, "ts0"))) { sp = '0'; p = a; continue; }
        else if ((a = etag_after(p, "tsc"))) { sp = 'c'; p = a; continue; }
        else if ((a = etag_after(p, "tsa"))) { sp = 'a'; p = a; continue; }
        else if ((a = etag_after(p, "tsr"))) { sp = 'r'; p = a; continue; }
        else if ((a = etag_after(p, "ny0"))) { ny = '0'; p = a; continue; }
        else if ((a = etag_after(p, "ny1"))) { ny = '1'; p = a; continue; }

        if ((a = etag_after(p, "eos"))) {
            if (last != '.' && last != '!' && last != '?') {
                if (o > out && o[-1] == ' ') o--;
                *o++ = '.'; last = '.';
            }
            p = a; continue;
        }

        if (p[0] == '\\' && p[1] == '!' && p[2] == '[') {
            const char *close = strchr(p + 3, ']');
            const char *e = close ? close + 1 : p + strlen(p);
            while (p < e && o < eo) *o++ = *p++;
            last = 'x'; continue;
        }
        if (p[0] == '\\' && p[1] == '!' && p[2] == 'p'
            && isdigit((unsigned char)p[3])) {
            *o++ = *p++; if (o < eo) *o++ = *p++;
            while (o < eo && (*p == 'p' || isdigit((unsigned char)*p))) *o++ = *p++;
            last = 'x'; continue;
        }
        if (p[0] == '<') {
            const char *gt = strchr(p, '>');
            const char *e = gt ? gt + 1 : p + strlen(p);
            while (p < e && o < eo) *o++ = *p++;
            last = 'x'; continue;
        }

        if (p[0] == '\\' && p[1] == '!') {
            p += 2;
            while (*p && !isspace((unsigned char)*p) && *p != '[') p++;
            if (*p == '[') { const char *c = strchr(p, ']'); p = c ? c + 1 : p + strlen(p); }
            continue;
        }

        if (isspace((unsigned char)*p)) {
            /* A one-shot accent expires HERE, at the separator that ends
             * the word it bound to -- not at the bottom of the loop, which
             * this `continue` never reaches. */
            if (pa && pa_started) {
                ETAG_FLUSH();
                pa = 0;
                pa_started = 0;
            }
            *o++ = *p++;
            continue;
        }

        int c = (unsigned char)*p;

        if (sp != '0') {
            const char *name = NULL;
            if (sp == 'c') {
                if (isdigit(c))      name = etag_digit_name(c);
                else if (isalpha(c)) name = etag_letter_name(c);
                else                 name = etag_symbol_name(c);
            } else if (sp == 'a') {
                if (isdigit(c))      name = etag_digit_name(c);
                else if (isalpha(c)) name = etag_letter_name(c);
            } else {
                if (isdigit(c))      name = etag_digit_name(c);
                else if (isalpha(c)) name = etag_radio_name(c);
            }
            if (name) {
                size_t nl = strlen(name);
                if (o + nl + 2 >= eo) break;
                if (o > out && o[-1] != ' ') *o++ = ' ';
                memcpy(o, name, nl); o += nl; *o++ = ' ';
                last = (unsigned char)name[nl - 1];
                p++; continue;
            }
            /* alnum-mode punctuation / unmapped symbol: emit verbatim so
             * the FE interprets it normally (may trigger a phrase break). */
            *o++ = (char)c; last = c; p++; continue;
        }

        if (ny == '0' && isdigit(c)) {
            int nd = 0; while (isdigit((unsigned char)p[nd])) nd++;
            int prev_alnum = (o > out && isalnum((unsigned char)o[-1]));
            int nxt = (unsigned char)p[nd];
            if (nd == 4 && !prev_alnum && nxt != '.' && nxt != ',') {
                int val = (p[0]-'0')*1000 + (p[1]-'0')*100
                        + (p[2]-'0')*10   + (p[3]-'0');
                if (val >= 1000) {
                    if (o > out && o[-1] != ' ') *o++ = ' ';
                    int w = etag_cardinal_4(val, o, (size_t)(eo - o));
                    if (w > 0 && o + w < eo) {
                        o += w; last = o[-1]; p += 4; continue;
                    }
                }
            }
        }

        /* Mark that the bound word has actually started. */
        if (pa) pa_started = 1;
        *o++ = (char)c; last = c; p++;
    }
    ETAG_FLUSH();
#undef ETAG_FLUSH
    *o = '\0';
    /* Collapse the per-char accent map to one entry per WHITESPACE-SEPARATED
     * TOKEN, taking each token's first character.
     *
     * ⚠ WHY NOT PER-CHAR. The consumer keys words by
     * `fe_parsed_word_t.char_start`, and on the host-emulator FE path that
     * field is 0 for EVERY word -- dumped: pi=0 and pi=1 both report
     * char_start=0. So every word read map[0] and a tag on the first word
     * covered all 48 halfphones, while a tag anywhere else did nothing at
     * all. Token ordinal is the key that actually survives this FE.
     *
     * LIMIT: ordinal matches the parsed word index only while the FE neither
     * splits nor merges tokens. Expansions like "3.5" -> "three point five"
     * or a spelled-out tag will shift everything after them, so a ToBI tag
     * placed after such a word can land on the wrong one. */
    /* macc stays PER-CHARACTER, like mvol and mrate. */
    /* SPFY_ETAG_DUMP=1: what the resolver actually produced. */
    if (spfy_env("SPFY_ETAG_DUMP")) {
        size_t n = (size_t)(o - out);
        fprintf(stderr, "[etag] resolved (%zu ch): %s\n", n, out);
        for (size_t i = 0; i < n; ) {
            if (!macc[i]) { ++i; continue; }
            size_t j = i;
            while (j < n && macc[j] == macc[i]) ++j;
            fprintf(stderr, "[etag] acc=%u over chars %zu..%zu = \"%.*s\"\n",
                    (unsigned)macc[i], i, j - 1, (int)(j - i), out + i);
            i = j;
        }
    }
    *out_vol = mvol;
    *out_rate = mrate;
    *out_acc = macc;
    return out;
}

/* Reconstruct fe_parsed_word_t.char_start by matching each word's own text
 * back into the text the FE was fed.
 *
 * ⚠ WHY THIS IS NEEDED. The FE emits `?d` -- "unspecified" -- instead of a
 * character offset on the plain-text feedConfigA path that fe_host.c and
 * fe_host_emu.c both use, and fe_parse.c stores that as 0. So char_start is 0
 * for EVERY word here (dumped: pi=0 and pi=1 both reporting 0), and every
 * consumer keyed on it reads position 0 for the whole utterance. Measured
 * consequences before this existed:
 *
 *   \!vp / \!vd   a tag at the START of the text changed every word; a tag
 *                 anywhere else was silently ignored. Volume was an
 *                 all-or-nothing utterance control that looked per-word only
 *                 if you never tested it past position 0.
 *   \![ToBI:]     a tag on the first word overrode 48 halfphones instead of 6.
 *
 * Words the FE invented -- "123" -> "wun hundred twenty three", where only the
 * first carries a source span -- will not be found in the input; they inherit
 * the previous word's offset, which is the span they came from. Matching is
 * case-insensitive and requires non-alphanumeric boundaries, so a short word
 * cannot match inside a longer one. */
static void fe_fill_char_starts(void *parsed_v, const char *text)
{
    fe_parsed_t *p = (fe_parsed_t *)parsed_v;
    if (!p || !text) return;
    size_t n = strlen(text), cur = 0;
    int last = 0;
    for (int i = 0; i < p->n_words; ++i) {
        fe_parsed_word_t *w = &p->words[i];
        size_t wl = strlen(w->text);
        if (wl == 0 || wl > n) { w->char_start = last; continue; }
        size_t found = (size_t)-1;
        for (size_t s = cur; s + wl <= n; ++s) {
            if (s > 0 && isalnum((unsigned char)text[s - 1])) continue;
            if (s + wl < n && isalnum((unsigned char)text[s + wl])) continue;
            size_t k = 0;
            while (k < wl
                   && tolower((unsigned char)text[s + k])
                      == tolower((unsigned char)w->text[k])) ++k;
            if (k == wl) { found = s; break; }
        }
        if (found == (size_t)-1) { w->char_start = last; continue; }
        w->char_start = (int)found;
        last = (int)found;
        cur = found + wl;
    }
}

/* Parse `\\![SPR]` and emit an FE tagged-output string. */
static int spr_inline_to_tagged(const char *text, char *out, size_t out_n,
                                int phrase_final)
{
    const char *p = strchr(text, '[');
    if (!p) return 0;
    p++;
    const char *end_br = strrchr(p, ']');
    if (!end_br) return 0;
    int max_stress = 0;
    const char *last_syl = NULL;
    for (const char *q = p; q < end_br; ++q)
        if (*q == '.' && q + 1 < end_br
            && q[1] >= '0' && q[1] <= '9') {
            if ((q[1] - '0') > max_stress) max_stress = q[1] - '0';
            last_syl = q;
        }
    char *o = out;
    char *eo = out + out_n - 1;
    int n = snprintf(o, (size_t)(eo - o),
        "#{. pau(p25) <SPR (0,%d) undef,%d [",
        (int)(end_br - p), max_stress);
    if (n < 0) return 0;
    o += n;
    int first_syl = 1;
    for (const char *q = p; q < end_br && o < eo; ) {
        if (*q == ' ' || *q == '\t') { ++q; continue; }
        if (*q == '.' && q + 1 < end_br
            && q[1] >= '0' && q[1] <= '9') {
            const char *marker = q;
            int stress = q[1] - '0';
            q += 2;
            /* ⚠ THE BOUNDARY TONE DOES NOT REPLACE THE PITCH ACCENT.
             *
             * This used to hand a phrase-final syllable `;L-L%` alone, on the
             * theory that the tone "wins the accent slot". The real FE emits
             * BOTH on a one-syllable phrase-final word -- `Aye.` tags as
             * `.1,H*;L-L%`, and so does every one-syllable word in
             * spfy_fe_text2tagged's output. Dropping the `,H*` cost the
             * syllable its accent, and slot_ctx's pass A/B both key off
             * `syl_accent != 0`: without it sp[1] falls back to the stress
             * default (2, or 1 at stress 0) instead of 7, and sp[2] stays at
             * its init 1 instead of 3.
             *
             * That is the whole of tom's and jill's 12-slot SLOT residual --
             * every one is a SINGLE-syllable `\![.Nx]`, because a
             * multi-syllable literal has first_syl != last_syl and so kept
             * its `,H*` by accident. `I.` and `Aye.` are the controls: same
             * shape, text mode, and they already matched at 7/3.
             *
             * SPFY_SPR_NO_FINAL_ACCENT=1 restores the replacing form. */
            static int no_fin_acc = -1;
            if (no_fin_acc < 0)
                no_fin_acc = (spfy_env("SPFY_SPR_NO_FINAL_ACCENT") != NULL);
            const char *accent;
            if (phrase_final && marker == last_syl)
                accent = (first_syl && !no_fin_acc) ? ",H*;L-L%" : ";L-L%";
            else
                accent = first_syl ? ",H*" : "";
            n = snprintf(o, (size_t)(eo - o),
                "%s.%d%s ", first_syl ? "" : " ", stress, accent);
            if (n < 0) break;
            o += n;
            first_syl = 0;
            continue;
        }
        const char *arpa = spr_to_arpabet(*q);
        ++q;
        if (!arpa) continue;
        n = snprintf(o, (size_t)(eo - o), "%s(p100) ", arpa);
        if (n < 0) break;
        o += n;
    }
    n = snprintf(o, (size_t)(eo - o), "] > pau(p50) } %%%%");
    if (n < 0) return 0;
    o += n;
    *o = '\0';
    return (int)(o - out);
}

/* Inline-markup segment kinds. */
enum { SEG_PLAIN = 0, SEG_SPR, SEG_PRON, SEG_PAUSE };

/* `lt` points at a '<'. */
static int is_pron_open(const char *lt)
{
    static const char kw[] = "pron";
    if (lt[0] != '<') return 0;
    for (int i = 0; i < 4; i++) {
        int c = (unsigned char)lt[1 + i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != kw[i]) return 0;
    }
    int d = (unsigned char)lt[5];
    return d == '\0' || isspace(d) || d == '/' || d == '>';
}

/* One-past-end of a `<pron ...>` construct starting at `lt`: past `/>` for
 * the self-closing form, past `</pron>` for `<pron ...>annotation</pron>`,
 * or just past the open tag's `>` if no close tag follows. */
static const char *pron_construct_end(const char *lt)
{
    const char *gt = strchr(lt, '>');
    if (!gt) return NULL;
    if (gt > lt && gt[-1] == '/') return gt + 1;
    const char *close = strstr(gt + 1, "</pron>");
    if (close) { const char *cg = strchr(close, '>'); if (cg) return cg + 1; }
    return gt + 1;
}

/* Find the next inline-markup token (`\![...]` SPR escape, `\!pN` pause, or
 * `<pron ...>` tag) at or after `s`. */
static const char *find_inline_token(const char *s, int *kind,
                                     const char **tok_end)
{
    for (const char *q = s; *q; ++q) {
        if (q[0] == '\\' && q[1] == '!' && q[2] == '[') {
            const char *close = strchr(q + 3, ']');
            if (close) { *kind = SEG_SPR; *tok_end = close + 1; return q; }
        } else if (q[0] == '\\' && q[1] == '!' && q[2] == 'p'
                   && isdigit((unsigned char)q[3])) {
            const char *e = q + 3;
            while (isdigit((unsigned char)*e)) ++e;
            *kind = SEG_PAUSE; *tok_end = e; return q;
        } else if (q[0] == '<' && is_pron_open(q)) {
            const char *e = pron_construct_end(q);
            if (e) { *kind = SEG_PRON; *tok_end = e; return q; }
        }
    }
    return NULL;
}

/* True when `text` carries inline markup the DLL FE can't read - an
 * `\![...]` SPR escape, a `\!pN` pause, or a `<pron ...>` tag - and so must
 * be routed through build_inline_mixed_tagged (which also covers the
 * lone-... */
static int spfy_text_has_inline_markup(const char *text)
{
    int kind; const char *te;
    return find_inline_token(text, &kind, &te) != NULL;
}

/* Remove the single `;L-L%` phrase-final boundary tone an in-house-FE core
 * carries when a `<pron>` word is phonemized in isolation, for use when the
 * word is NOT phrase-final in the merged sentence (keeps mid-sentence... */
static void strip_boundary_tone(char *s)
{
    char *b = strstr(s, ";L-L%");
    if (b) memmove(b, b + 5, strlen(b + 5) + 1);
}

/* Locate the inner word/pause "core" of one tagged-output block: the span
 * AFTER the leading pad `pau(...)` and BEFORE the trailing pad `pau(...)`. */
static int spr_tagged_core(const char *block, const char **start, size_t *len)
{
    const char *first = strstr(block, "pau(");
    if (!first) return 0;
    const char *after = strchr(first, ')');
    if (!after) return 0;
    after++;

    const char *last = NULL, *q = block;
    while ((q = strstr(q, "pau(")) != NULL) { last = q; q += 4; }
    if (!last || last <= after) return 0;

    while (after < last && (unsigned char)*after <= ' ') after++;
    const char *e = last;
    while (e > after && (unsigned char)e[-1] <= ' ') e--;
    if (e <= after) return 0;
    *start = after;
    *len = (size_t)(e - after);
    return 1;
}

/* Map a character to the tagged-output phrase-terminator marker it implies,
 * or 0 if it is not phrase-breaking punctuation. */
static char spr_break_marker(int c)
{
    switch (c) {
        case ',': case ';':
        case '.': case '!': case '?': return (char)c;
        case ':':                     return ';';
        default:                      return 0;
    }
}

/* Build ONE flowing tagged-output utterance from text that mixes plain
 * words with inline markup - `\![...]` SPR escapes, `\!pN` pause tags,
 * and/or `<pron ...>` tags - none of which the DLL FE can read (it would
 * spell... */
static char *build_inline_mixed_tagged(spfy_fe_t *fe, const char *text)
{
    size_t tlen   = strlen(text);
    /* DLL-FE output expands plain text by a large factor (each word grows
     * to `<word (s,l) pos,k [.k,acc phon(pNNN) ...]>`); size generously and
     * bound-check on append so we never overflow. */
    size_t segcap = tlen * 80 + 65536;
    size_t outcap = segcap + 1024;
    char *seg = (char *)malloc(tlen + 1);
    char *segbuf = (char *)malloc(segcap);
    char *acc = (char *)malloc(outcap);
    if (!seg || !segbuf || !acc) { free(seg); free(segbuf); free(acc); return NULL; }

    size_t acc_len = 0;
    acc_len += (size_t)snprintf(acc, outcap, "#{. pau(p25) ");

    const char *p = text;
    int ok = 0, any_core = 0;
    char pend_break = 0;
    int pend_pause = 0;
    while (*p) {
        int kind = SEG_PLAIN;
        const char *tok_end = NULL;
        const char *tok = find_inline_token(p, &kind, &tok_end);
        const char *seg_start = p;
        const char *seg_end;
        if (tok == p) {
            seg_end = tok_end;
        } else if (tok) {
            seg_end = tok; kind = SEG_PLAIN;
        } else {
            seg_end = p + strlen(p); kind = SEG_PLAIN;
        }
        int is_markup = (kind != SEG_PLAIN);
        size_t seg_len = (size_t)(seg_end - seg_start);
        p = seg_end;
        if (seg_len == 0) continue;

        /* `\!pN` pause: not a phonemizable run - record the duration and
         * whether it sits immediately before punctuation, then apply it at
         * the next junction (or before the closing pad). */
        if (kind == SEG_PAUSE) {
            int n = atoi(seg_start + 3);
            if (n < 1)     n = 1;
            if (n > 32767) n = 32767;
            pend_pause = n;
            continue;
        }

        /* Skip whitespace-only plain runs (the space between a word and a
         * markup token) - no phonemes, and the FE would emit an empty utt. */
        if (!is_markup) {
            int ws_only = 1;
            for (size_t i = 0; i < seg_len; i++)
                if (!isspace((unsigned char)seg_start[i])) { ws_only = 0; break; }
            if (ws_only) continue;
        }

        /* Phrase-break punctuation adjacent to THIS junction: the break
         * char trailing the previous seg, or leading this one (first non-ws
         * char). */
        char lead = 0;
        {
            const char *s = seg_start;
            while (s < seg_end && isspace((unsigned char)*s)) s++;
            if (s < seg_end) lead = spr_break_marker((unsigned char)*s);
        }

        /* A markup run is phrase-final when the next non-whitespace content
         * is phrase-breaking punctuation or end-of-text - i.e. */
        int seg_final = 0;
        if (is_markup) {
            const char *la = p;
            while (*la && isspace((unsigned char)*la)) la++;
            seg_final = (*la == '\0' || spr_break_marker((unsigned char)*la) != 0);
        }

        memcpy(seg, seg_start, seg_len);
        seg[seg_len] = '\0';

        const char *block;
        if (kind == SEG_SPR) {
            if (spr_inline_to_tagged(seg, segbuf, segcap, seg_final) <= 0)
                goto done;
            block = segbuf;
        } else if (kind == SEG_PRON) {
            /* The in-house FE owns the <pron sym=...> parser. */
            if (spfy_fe_internal_text_to_tagged(seg, segbuf, segcap) < 0)
                goto done;
            if (!seg_final) strip_boundary_tone(segbuf);
            block = segbuf;
        } else {
            if (spfy_fe_text_to_tagged(fe, seg, segbuf, segcap) <= 0) continue;
            block = segbuf;
        }

        const char *core; size_t core_len;
        if (!spr_tagged_core(block, &core, &core_len)) continue;

        /* Separator: a real phrase break when a comma/period/... */
        if (acc_len + core_len + 64 >= outcap) goto done;
        if (any_core) {
            char br = pend_break ? pend_break : lead;
            if (pend_pause) {
                /* \!pN renders as a phrase break carrying a `pau(uN)` USER
                 * pause that the synth loop turns into N ms of injected
                 * silence (the FE pipeline does not size silence from the
                 * structural `pau(pN)` value). */
                char term = br ? br : ',';
                acc_len += (size_t)snprintf(acc + acc_len, outcap - acc_len,
                                            " pau(p50) } {%c pau(u%d) ", term, pend_pause);
                pend_pause = 0;
            } else if (br) {
                acc_len += (size_t)snprintf(acc + acc_len, outcap - acc_len,
                                            " pau(p50) } {%c pau(p100) ", br);
            } else {
                acc[acc_len++] = ' ';
            }
        }
        memcpy(acc + acc_len, core, core_len);
        acc_len += core_len;
        any_core = 1;

        /* Remember break punct trailing this seg (last non-ws char) for the
         * NEXT junction - covers `word, \![...]` as well as `\![...],
         * word`. */
        pend_break = 0;
        {
            const char *e = seg_end;
            while (e > seg_start && isspace((unsigned char)e[-1])) e--;
            if (e > seg_start) pend_break = spr_break_marker((unsigned char)e[-1]);
        }
    }

    if (!any_core) goto done;
    acc_len += (size_t)snprintf(acc + acc_len, outcap - acc_len,
                                " pau(p50) } %%");
    ok = 1;

done:
    free(seg);
    free(segbuf);
    if (!ok) { free(acc); return NULL; }
    return acc;
}

/* Per-call synth: text -> FE -> USel -> WSOLA -> sink. */
int spfy_synth_to_sink(spfy_voice_t *v, const char *text,
                       spfy_wav_writer_t *sink,
                       const spfy_synth_callbacks_t *cb,
                       spfy_synth_stats_t *out_stats)
{
    /* `\s4m` - Speechify 4 mode, for hosts that cannot pass a CLI flag or
     * set an environment variable. */
    /* ---- REJOIN A TAG A HOST CUT IN HALF -------------------------------
     * Balabolka hands tagged text to the engine in pieces, splitting so
     * that one Speak() call ENDS with a bare `\!` and the next begins with
     * the rest of... */
    static int g_split_pending = 0;
    char *split_buf = NULL;
    if (text) {
        size_t n = strlen(text), end = n;
        int need_prefix = g_split_pending;
        g_split_pending = 0;
        while (end && (text[end - 1] == ' ' || text[end - 1] == '\t'
                       || text[end - 1] == '\r' || text[end - 1] == '\n'))
            --end;
        int dangling = (end >= 2 && text[end - 2] == '\\'
                        && text[end - 1] == '!');
        if (need_prefix || dangling) {
            size_t keep = dangling ? end - 2 : n;
            split_buf = (char *)malloc(keep + 3u);
            if (split_buf) {
                char *o = split_buf;
                if (need_prefix) { *o++ = '\\'; *o++ = '!'; }
                memcpy(o, text, keep);
                o[keep] = '\0';
                text = split_buf;
            }
            if (dangling) g_split_pending = 1;
        }
    }

    spfy4_env_scope_t s4_scope;
    int s4_scoped = 0;
    if (text && !g_s4_forbidden) {
        const char *p = text, *q = NULL;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
        if (p[0] == '\\') {
            /* Ordered so a short string fails on the NUL before any later
             * index is evaluated -- && short-circuits, so this cannot read
             * past the terminator. */
            if (p[1] == '!' && (p[2] == 's' || p[2] == 'S')
                && p[3] == '4' && (p[4] == 'm' || p[4] == 'M'))
                q = p + 5;
            else if ((p[1] == 's' || p[1] == 'S')
                     && p[2] == '4' && (p[3] == 'm' || p[3] == 'M'))
                q = p + 4;
        }
        if (q && !isalnum((unsigned char)*q)) {
            /* Snapshot BEFORE applying, or it captures the values the mode
             * just wrote and the restore is a no-op. */
            if (g_s4_pending) s4_scope = g_s4_pending_scope;
            else              spfy4_env_save(&s4_scope);
            spfy4_apply_defaults(NULL);
            while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') ++q;
            text = q;
            /* Words after the tag? */
            for (const char *r = text; *r; ++r)
                if (isalnum((unsigned char)*r)) { s4_scoped = 1; break; }
            if (s4_scoped) {
                g_s4_pending = 0;
            } else {
                g_s4_pending_scope = s4_scope;
                g_s4_pending = 1;
            }
        } else if (g_s4_pending) {
            /* No tag of its own, but a tag-only utterance is waiting to be spent. */
            for (const char *r = text; *r; ++r)
                if (isalnum((unsigned char)*r)) { s4_scoped = 1; break; }
            if (s4_scoped) {
                s4_scope = g_s4_pending_scope;
                g_s4_pending = 0;
            }
        }
    }

    spfy_prosody_hints_t hints = {0};
    spfy_fe_utt_t fe_utt = {0};
    spfy_slot_tree_t tree = {0};
    spfy_slice_ctx_table_t slice_ctx = {0};
    spfy_sp_target_table_t sp_tab = {0};
    spfy_slot_preds_table_t preds_tab = {0};
    /* SPFY_DP_F0_CONT state: per-uid true F0 for the DP continuity term. */
    spfy_pmarks_t   dp_marks = {0};
    /* Energy tables; NULL unless SPFY_POW_CONT_W (join) or SPFY_POW_TGT_W
     * (target) is set. */
    float          *unit_pow = NULL;
    uint32_t        unit_pow_n = 0u;
    float           pow_join_w = 0.0f, pow_join_dead = 0.0f;
    float          *pow_mean = NULL, *pow_sd = NULL;
    spfy_reselect_t dp_f0    = {0};
    int             dp_f0_on = 0;
    float           dp_f0_w = 0.0f, dp_f0_dead = 0.0f, dp_f0_up = 1.0f;
    float           dp_f0_break = 0.0f, dp_f0_slope = 0.0f;
    int             dp_f0_scope = 1;
    float           dp_f0_seam = 0.0f;
    uint32_t *q5_per_slot = NULL;
    uint8_t  *q5_has = NULL;
    uint32_t *hp_to_post = NULL;
    uint32_t *post_to_hp = NULL;
    uint32_t *hp_word_idx = NULL;
    /* Per-hp SSML / Balabolka prosody overrides (signed; 0 = neutral). */
    int8_t   *hp_pitch_st = NULL;
    int8_t   *hp_rate_pct = NULL;
    uint16_t *syl_vol = NULL;      /* per-syllable volume % from \!vp/\!vd (0 = 100), indexed by fe_shared-1
 * (reliable, unlike the tree-word -> parsed-word count) */
    spfy_viterbi_slot_t *vslots = NULL;
    uint32_t **cbuf = NULL;
    float    **tbuf = NULL;
    uint8_t **cand_c68 = NULL;
    uint8_t **cand_c6c = NULL;
    uint8_t **cand_c70 = NULL;
    uint8_t **cand_c78 = NULL;
    uint8_t **anchor_c68 = NULL;
    uint8_t **anchor_c6c = NULL;
    uint8_t **anchor_c70 = NULL;
    uint8_t **anchor_c78 = NULL;
    uint32_t **anchor_cands  = NULL;
    uint32_t **anchor_jks    = NULL;
    float    **anchor_target = NULL;
    uint32_t  *anchor_n      = NULL;
    spfy_viterbi_dag_slot_t *dag_slots = NULL;
    uint32_t  n_slots = 0;
    /* Per-hp boundary-tone target (signed ST; 0 = none). */
    int8_t   *hp_btone = NULL;
    int8_t   *hp_acctype = NULL;  /* per-hp accent-TYPE st bias (H*=0, L*=-5, !H*=-2 ...),
 * SPFY_SPFY4_ACCTYPE_GAIN-gated */
    uint8_t  *hp_accent = NULL;   /* per-hp accent PRESENCE (syl_accent[]); hp_acctype can't carry this -- 0
 * means both "H*" and "unaccented". */
    uint32_t *hp_syl = NULL;      /* per-hp syllable id, so the contour can keep abutting accented syllables
 * apart */
    uint8_t  *hp_nuc = NULL;      /* per-hp "is a syllable nucleus", so accents centre on the vowel, not the
 * syllable */
    int rc;
    char *etags_text = NULL;
    uint16_t *etag_vol = NULL;
    uint16_t *etag_rate = NULL;
    uint8_t  *etag_acc = NULL;
    uint8_t  *hp_tobi = NULL;
    size_t etag_maps_n = 0;
    spfy_prosody_hints_init(&hints);

    /* Embedded \!-tag pre-pass (Speechify User's Guide ch. */
    if (spfy_etags_need_resolve(text)) {
        etags_text = spfy_etags_resolve(text, &etag_vol, &etag_rate,
                                        &etag_acc);
        if (etags_text) { text = etags_text; etag_maps_n = strlen(etags_text); }
    }
    if (spfy_env("SPFY_ETAGS_DUMP"))
        fprintf(stderr, "[etags] resolved: %s\n", text);

    if (spfy_env("SPFY_VOICING_DUMP")) {
        /* Diagnostic: dump voicing[] table to verify per-hp_class
         * voiced/voiceless mapping. */
        fprintf(stderr, "{\"voicing\":1,\"n\":%u,\"vals\":[",
                v->av.voicing_n);
        for (uint32_t i = 0; i < v->av.voicing_n; ++i)
            fprintf(stderr, "%s%u", i ? "," : "",
                    v->av.voicing ? v->av.voicing[i] : 0u);
        fprintf(stderr, "]}\n");
    }

    /* Hosted FE: drive the public synth_text API; the parsed result is
     * stashed on `v->fe` and retrieved via spfy_fe_get_parsed. */
    {
        spfy_fe_utterance_t *utt_unused = NULL;
        const char *tagged_file = spfy_env("SPFY_TAGGED_FILE");
        if (tagged_file) {
            /* Experiment hook: synth from a tagged-output file verbatim,
             * bypassing the FE text pass entirely. */
            FILE *tf = fopen(tagged_file, "rb");
            if (!tf) {
                fprintf(stderr, "SPFY_TAGGED_FILE: cannot open %s\n",
                        tagged_file);
                rc = SPFY_E_INVAL; goto fail;
            }
            fseek(tf, 0, SEEK_END);
            long tsz = ftell(tf);
            fseek(tf, 0, SEEK_SET);
            if (tsz < 0) { fclose(tf); rc = SPFY_E_INVAL; goto fail; }
            char *tagged = (char *)malloc((size_t)tsz + 1u);
            if (!tagged) { fclose(tf); rc = SPFY_E_NOMEM; goto fail; }
            size_t trd = fread(tagged, 1, (size_t)tsz, tf);
            fclose(tf);
            tagged[trd] = '\0';
            fprintf(stderr, "[spfy] FE bypass: tagged text from %s "
                    "(%zu bytes)\n", tagged_file, trd);
            rc = spfy_fe_synth_tagged(v->fe, tagged, &hints, &utt_unused);
            free(tagged);
        } else if (spfy_text_has_inline_markup(text)) {
            /* Inline pronunciation markup - `\![...]` SPR escapes and/or
             * `<pron ...>` tags - that the DLL FE can't read. */
            char *mixed = build_inline_mixed_tagged(v->fe, text);
            if (!mixed) {
                fprintf(stderr, "build_inline_mixed_tagged: bad inline markup\n");
                rc = SPFY_E_INVAL; goto fail;
            }
            if (spfy_env("SPFY_ETAGS_DUMP"))
                fprintf(stderr, "[etags] tagged: %s\n", mixed);
            /* Refinement must be ON for THIS parse - same reasoning as the
             * SPFY_FE_INTERNAL branch below, and the same symptom.
             *
             * The SPR runs here come from spr_inline_to_tagged, which emits
             * the symbols VERBATIM. The engine flaps them: fed
             * `\![.1pa.0tx.0wa.0tu.0mi]` its own --g2p answers
             * `p aa dx ax w aa dx uw m iy` -- input `t` comes back as SPR
             * `F` (dx). Our parse inherited ESPR's process-global
             * refine=0 (set at voice load because ESPR output is already
             * reduced) and so kept a hard `t`.
             *
             * Measured on spr_002: ctx differed at exactly the two `t`
             * positions, phone 36 (t) where the engine has 11 (dx), which
             * dragged slot fidelity to 6/24 and broke the audio too.
             *
             * ⚠ FLAP_ONLY, not full refinement. Inline SPR has already NAMED
             * every phone, so the vowel passes must not fire -- the engine's
             * own --g2p leaves the `x` (ax) exactly as written while flapping
             * the `t`. Turning the full set on fixed the flap but then
             * reduced that ax to ix, putting phone 21 where the engine has 5;
             * slot fidelity went 6/24 -> 14/24 instead of 24/24.
             *
             * Save/restore so a later hosted-FE call still sees ESPR's
             * choice. */
            const int refine_prev_inline = fe_parse_get_refine();
            fe_parse_set_refine(FE_REFINE_FLAP_ONLY);
            rc = spfy_fe_synth_tagged(v->fe, mixed, &hints, &utt_unused);
            fe_parse_set_refine(refine_prev_inline);
            free(mixed);
        } else if (spfy_env("SPFY_FE_INTERNAL")) {
            /* In-house FE path: text → fe_internal → tagged-text →
             * spfy_fe_synth_tagged → slot tree. */
            static int logged = 0;
            if (!logged) {
                fprintf(stderr,
                    "[spfy] FE backend: IN-HOUSE pure-C "
                    "(SPFY_FE_INTERNAL forced override)\n");
                logged = 1;
            }
            /* 64 KB tagged buffer - accommodates long passages (the sioux
             * pangram in the audit corpus runs ~30 KB tagged). */
            static char tagged_buf_[65536];
            int frc = spfy_fe_internal_text_to_tagged(
                text, tagged_buf_, sizeof tagged_buf_);
            if (frc < 0) {
                fprintf(stderr, "spfy_fe_internal_text_to_tagged failed\n");
                rc = SPFY_E_INVAL; goto fail;
            }
            /* Refinement must be ON for THIS parse, whatever the hosted
             * backend decided. */
            const int refine_prev = fe_parse_get_refine();
            fe_parse_set_refine(1);
            rc = spfy_fe_synth_tagged(v->fe, tagged_buf_, &hints, &utt_unused);
            fe_parse_set_refine(refine_prev);
        } else {
            static int logged = 0;
            if (!logged) {
#if defined(SPFY_FE_EMU)
                fprintf(stderr,
                    "[spfy] FE backend: EMULATED DLL "
                    "(SWIttsFe-<lang> via host_emu, portable to "
                    "arm64/wasm). The image is picked from the voice's "
                    "VCF language -- see the [fe_host] line above.\n");
#else
                fprintf(stderr,
                    "[spfy] FE backend: IN-HOUSE pure-C "
                    "(no DLL loader on this build)\n");
#endif
                logged = 1;
            }
            rc = spfy_fe_synth_text(v->fe, text, &hints, &utt_unused);
        }
        if (rc != SPFY_OK) {
            fprintf(stderr, "spfy_fe_synth_text failed: %s\n",
                    spfy_strerror(rc));
            goto fail;
        }
        spfy_fe_utterance_free(utt_unused);
    }
    const fe_parsed_t *parsed =
        (const fe_parsed_t *)spfy_fe_get_parsed(v->fe);
    if (!parsed || parsed->n_words == 0) {
        /* Valid input that produces no audio (e.g. */
        rc = SPFY_OK; goto cleanup;
    }

    /* Multi-utterance synthesis. */
    uint32_t max_phrase_id = 0;
    for (int i = 0; i < parsed->n_words; i++) {
        if ((uint32_t)parsed->words[i].phrase_id > max_phrase_id)
            max_phrase_id = (uint32_t)parsed->words[i].phrase_id;
    }
    uint32_t n_phrases = max_phrase_id + 1u;
    int first_phrase_only = (spfy_env("SPFY_FIRST_PHRASE_ONLY") != NULL);
    if (first_phrase_only) n_phrases = 1u;

    /* [live-trace] one-shot header: sample rate (for waveform decode) plus
     * corpus/phrase counts so the viz can size its lattice up front. */
    spfy_trace_eventf("meta",
        "{\"sample_rate\":%u,\"n_units\":%u,\"n_phrases\":%u}",
        v->vdb.sample_rate, v->units.n_units, n_phrases);

    /* [live-trace] global spoken-word counter (across ALL phrases), aligned
     * with the emitted `word` events. */
#ifdef SPFY_TRACE
    int g_wseq = -1;
#endif

    /* Inter-phrase silence - DEFAULT OFF as of 2026-05-19 evening. */
    int inter_phrase_ms_override = -1;
    {
        const char *e = spfy_env("SPFY_INTERPHRASE_MS");
        if (e) {
            inter_phrase_ms_override = atoi(e);
            if (inter_phrase_ms_override < 0)   inter_phrase_ms_override = 0;
            if (inter_phrase_ms_override > 500) inter_phrase_ms_override = 500;
        }
    }

    /* Sink is caller-owned (CLI opened a file, SAPI a callback). */
    spfy_wsola_streamer_t ws;
    spfy_wsola_init(&ws, sink);

    size_t total_played = 0, total_skipped = 0, total_paired_same = 0,
           total_paired_cross = 0, total_interword_pauses = 0;

    /* sentence_idx_in_para tracks engine's `*param_1` value in
     * FUN_08e8c7d0 - starts at 0, increments for each non-end-punct
     * phrase ('.', '?', '!' reset to 0; ',', ';', etc increment).
     * Affects the slot's sp3 (wordInPhrase) code for words at
     * syl_idx=0 - phrase-initial words get code 5 only when
     * sentence_idx_in_para==0 AND voice config_d4==0; otherwise code 1.
     * SPFY_NO_SENTENCE_IDX_PARA=1 reverts to the legacy hardcoded 0. */
    uint32_t sentence_idx_in_para = 0;

    /* --- SPFY_DP_F0_CONT: load the per-uid true-F0 table
     * ------------------ OFF unless SPFY_DP_F0_CONT is set to a non-zero
     * weight, so the byte-exact S3 audit is untouched. */
    {
        const char *e = spfy_env("SPFY_DP_F0_CONT");
        /* Set-but-zero is deliberately NOT "off": it builds the table and
         * runs the diagnostic while adding exactly 0.0f to every edge, so
         * it is the baseline arm of the sweep and a control in one. */
        if (e && *e) {
            const char *pm_stem = spfy_env("SPFY_PROSODY_PM");
            dp_f0_w = (float)atof(e);
            dp_f0_dead = 1.0f;
            {
                const char *d = spfy_env("SPFY_DP_F0_DEAD");
                if (d && *d) dp_f0_dead = (float)atof(d);
                const char *u = spfy_env("SPFY_DP_F0_UP");
                if (u && *u) dp_f0_up = (float)atof(u);
                const char *b = spfy_env("SPFY_DP_F0_BREAK");
                if (b && *b) dp_f0_break = (float)atof(b);
                const char *s = spfy_env("SPFY_DP_F0_SLOPE");
                if (s && *s) dp_f0_slope = (float)atof(s);
                const char *sc = spfy_env("SPFY_DP_F0_SCOPE");
                if (sc && *sc) dp_f0_scope = atoi(sc);
                const char *sm = spfy_env("SPFY_DP_F0_SEAM");
                if (sm && *sm) dp_f0_seam = (float)atof(sm);
            }
            if (dp_f0_seam < 0.0f) dp_f0_seam = 0.0f;
            if (dp_f0_scope < 0) dp_f0_scope = 0;
            if (dp_f0_scope > 2) dp_f0_scope = 2;
            if (dp_f0_dead  < 0.0f) dp_f0_dead  = 0.0f;
            if (dp_f0_up    < 0.0f) dp_f0_up    = 0.0f;
            if (dp_f0_break < 0.0f) dp_f0_break = 0.0f;
            if (!pm_stem || !*pm_stem) {
                spfy_log_warn("dp-f0: SPFY_DP_F0_CONT set but SPFY_PROSODY_PM "
                              "is not; term DISABLED");
            } else if (spfy_pmarks_load(pm_stem, &dp_marks) != SPFY_OK) {
                spfy_log_warn("dp-f0: no pitch marks at '%s'; term DISABLED",
                              pm_stem);
            } else if (spfy_reselect_build(&dp_f0, &v->units, &dp_marks)
                       != SPFY_OK) {
                spfy_log_warn("dp-f0: could not build the F0 table; "
                              "term DISABLED");
                spfy_pmarks_free(&dp_marks);
                dp_marks = (spfy_pmarks_t){0};
            } else {
                uint32_t voiced = 0;
                for (uint32_t i = 0; i < dp_f0.n_units; ++i)
                    if (dp_f0.f0[i] > 0.0f) ++voiced;
                dp_f0_on = 1;
                spfy_log_warn("dp-f0: ON - w=%.3f/st, dead=%.2f st, "
                              "up=%.2fx, break=%.2f st, slope=%+.3f st/join, "
                              "scope=%d, seam=%.3f, "
                              "%u/%u units voiced (%.1f%%)",
                              (double)dp_f0_w, (double)dp_f0_dead,
                              (double)dp_f0_up, (double)dp_f0_break,
                              (double)dp_f0_slope, dp_f0_scope,
                              (double)dp_f0_seam,
                              voiced, dp_f0.n_units,
                              dp_f0.n_units
                                  ? 100.0 * voiced / dp_f0.n_units : 0.0);
            }
        }
    }

    /* --- SPFY_POW_CONT_W / SPFY_POW_TGT_W: measure per-unit energy
     * -------- Built HERE, before the phrase loop, for the same reason the
     * F0 table is: it walks the whole inventory, and rebuilding it per
     * phrase would make a... */
    {
        const char *e_join = spfy_env("SPFY_POW_CONT_W");
        const char *e_tgt  = spfy_env("SPFY_POW_TGT_W");
        int want_join = (e_join && *e_join);
        int want_tgt  = (e_tgt  && *e_tgt);
        if (want_join || want_tgt) {
            uint32_t pm = 0;
            unit_pow = build_unit_power(v, &unit_pow_n, &pm);
            if (!unit_pow) {
                spfy_log_warn("pow: could not measure unit energy; both "
                              "energy terms DISABLED");
                unit_pow_n = 0u;
            } else {
                /* Percentiles of the measured energies. */
                float *srt = (float *)malloc((size_t)pm * sizeof *srt);
                double p05 = 0.0, p50 = 0.0, p95 = 0.0;
                if (srt && pm) {
                    uint32_t k = 0;
                    for (uint32_t i = 0; i < unit_pow_n; ++i)
                        if (unit_pow[i] > 0.0f) srt[k++] = unit_pow[i];
                    qsort(srt, k, sizeof *srt, cmp_float_asc);
                    if (k) {
                        p05 = srt[(uint32_t)(0.05 * (k - 1))];
                        p50 = srt[(uint32_t)(0.50 * (k - 1))];
                        p95 = srt[(uint32_t)(0.95 * (k - 1))];
                    }
                }
                free(srt);
                spfy_log_warn("pow: %u/%u units measured (%.1f%%), "
                              "log-RMS p05/p50/p95 = %.3f/%.3f/%.3f",
                              pm, unit_pow_n,
                              unit_pow_n ? 100.0 * pm / unit_pow_n : 0.0,
                              p05, p50, p95);
            }
        }
        if (unit_pow && want_join) {
            pow_join_w = (float)atof(e_join);
            const char *d = spfy_env("SPFY_POW_CONT_DEAD");
            if (d && *d) pow_join_dead = (float)atof(d);
            if (pow_join_w    < 0.0f) pow_join_w    = 0.0f;
            if (pow_join_dead < 0.0f) pow_join_dead = 0.0f;
            spfy_log_warn("pow-cont (join): ON - w=%.3f/log-unit, dead=%.3f",
                          (double)pow_join_w, (double)pow_join_dead);
        }
        if (unit_pow && want_tgt) {
            uint32_t rows = 0;
            if (load_mean_power(&v->vin, &pow_mean, &pow_sd, &rows)
                != SPFY_OK) {
                spfy_log_warn("pow-tgt: no usable `mean` chunk; term "
                              "DISABLED");
            } else {
                /* Calibration from tools/mean_ident.py: the `mean` power
                 * column equals 0.9813 * our_log_rms - 3.0232 (r=0.931,
                 * residual 0.337 against a shipped spread of 0.330). */
                v->av.pow_a = 0.9813f;
                v->av.pow_b = -3.0232f;
                const char *cal_a = spfy_env("SPFY_POW_TGT_A");
                const char *cal_b = spfy_env("SPFY_POW_TGT_B");
                if (cal_a && *cal_a) v->av.pow_a = (float)atof(cal_a);
                if (cal_b && *cal_b) v->av.pow_b = (float)atof(cal_b);
                v->av.unit_pow   = unit_pow;
                v->av.unit_pow_n = unit_pow_n;
                v->av.pow_mean   = pow_mean;
                v->av.pow_sd     = pow_sd;
                v->av.pow_rows   = rows;
                v->av.w_pow_t    = (float)atof(e_tgt);
                if (v->av.w_pow_t < 0.0f) v->av.w_pow_t = 0.0f;
                spfy_log_warn("pow-tgt: ON - w=%.4f, %u rows, "
                              "scale %.4f*x%+.4f, mean[0..2]=%.3f/%.3f/%.3f",
                              (double)v->av.w_pow_t, rows,
                              (double)v->av.pow_a, (double)v->av.pow_b,
                              rows > 0 ? (double)pow_mean[0] : 0.0,
                              rows > 1 ? (double)pow_mean[1] : 0.0,
                              rows > 2 ? (double)pow_mean[2] : 0.0);
            }
        }
    }

    for (uint32_t phrase_idx = 0; phrase_idx < n_phrases; ++phrase_idx) {


    {
        int phrase_has_words = 0;
        for (int i = 0; i < parsed->n_words; i++) {
            if ((uint32_t)parsed->words[i].phrase_id == phrase_idx) {
                phrase_has_words = 1; break;
            }
        }
        if (!phrase_has_words) continue;
    }

    if (spfy_env("SPFY_MULTI_DEBUG")) {
        int n_w = 0, n_p = 0;
        for (int i = 0; i < parsed->n_words; i++) {
            if ((uint32_t)parsed->words[i].phrase_id == phrase_idx) {
                n_w++; n_p += parsed->words[i].n_phonemes;
            }
        }
        fprintf(stderr, "[multi] phrase %u: n_word=%d n_phon=%d (hosted)\n",
                phrase_idx, n_w, n_p);
    }

    /* Build the trace-format spfy_fe_utt_t from the FE output, then drive
     * the validated slot-tree pipeline (BuildGraph + LinkGraph +
     * derive_slice_ctx + derive_sp_targets + derive_q5_table). */
    const char **seg_names = NULL;
    uint32_t n_segs_arr = 0;
    if ((rc = parsed_to_fe_utt(parsed, text, (int)phrase_idx, &fe_utt)) != SPFY_OK
        || (rc = build_segments_from_parsed(parsed, (int)phrase_idx,
                                            &seg_names, &n_segs_arr)) != SPFY_OK) {
        goto fail;
    }
    /* delta stays alive across the whole phrase loop; we restore its
     * contents from delta_backup_tokens at the start of each iteration. */

    if ((rc = spfy_build_graph(&fe_utt, &tree)) != SPFY_OK) {
        free(seg_names); goto fail;
    }
    /* Pass THIS voice's phone inventory: ctx[] is in engine phone-id
     * (feat["name"]) numbering, which differs per voice. */
    if ((rc = spfy_derive_slice_ctx(&tree, seg_names, n_segs_arr,
                                    v->phone_order.phone_names,
                                    v->phone_order.n_phones, &slice_ctx))
        != SPFY_OK
        /* voice_d4_flag was a hardcoded 0 -- Tom's value, and Tom is the
         * only voice it was ever checked against. */
        || (rc = spfy_derive_sp_targets(&tree, &fe_utt,
                spfy_env("SPFY_NO_SENTENCE_IDX_PARA") ? 0 : sentence_idx_in_para,
                spfy_env("SPFY_VOICE_D4_FLAG")
                    ? atoi(spfy_env("SPFY_VOICE_D4_FLAG")) : 0, &sp_tab))
           != SPFY_OK) {
        free(seg_names); goto fail;
    }
    /* SPFY_SP_OVERRIDE - force the prosodic-position target on chosen
     * half-phones. */
    {
        const char *ov = spfy_env("SPFY_SP_OVERRIDE");
        if (ov && *ov) {
            uint32_t hp = 0;
            /* Half-phone ordinal -> tree slot, so the caller indexes the
             * same thing the dumps print instead of raw tree slots (which
             * include word and syllable anchors and would silently
             * mis-target). */
            for (uint32_t s = 0; s < tree.n_slots; ++s) {
                if (tree.slots[s].kind != SPFY_SK_HALFPHONE) continue;
                const char *p = ov;
                while (*p) {
                    char *end = NULL;
                    unsigned long want = strtoul(p, &end, 10);
                    if (end == p) break;
                    if (*end == ':' && want == (unsigned long)hp) {
                        const char *q = end + 1;
                        for (int k = 0; k < 5 && *q; ++k) {
                            if (*q != '-') {
                                char *e2 = NULL;
                                unsigned long val = strtoul(q, &e2, 10);
                                if (e2 != q) {
                                    sp_tab.sp[s][k] = (uint32_t)val;
                                    q = e2;
                                }
                            } else {
                                ++q;
                            }
                            if (*q == ',') ++q;
                        }
                        fprintf(stderr, "[sp_override] hp=%u slot=%u -> "
                                "[%u,%u,%u,%u,%u]\n", hp, s,
                                sp_tab.sp[s][0], sp_tab.sp[s][1],
                                sp_tab.sp[s][2], sp_tab.sp[s][3],
                                sp_tab.sp[s][4]);
                    }
                    p = strchr(end, ';');
                    if (!p) break;
                    ++p;
                }
                ++hp;
            }
        }
    }
    /* SPFY_SP_TARGET_DUMP=1: emit our per-HP sp[0..4] for offline diff
     * against engine's inner_scorer.sp_target. */
    if (spfy_env("SPFY_SP_TARGET_DUMP")) {
        for (uint32_t s = 0; s < tree.n_slots; ++s) {
            if (tree.slots[s].kind != SPFY_SK_HALFPHONE) continue;
            if (!sp_tab.has[s]) continue;
            fprintf(stderr,
                "{\"sp_target\":1,\"slot\":%u,\"sp\":[%u,%u,%u,%u,%u]}\n",
                s, sp_tab.sp[s][0], sp_tab.sp[s][1], sp_tab.sp[s][2],
                sp_tab.sp[s][3], sp_tab.sp[s][4]);
        }
    }
    q5_per_slot = (uint32_t *)calloc(tree.n_slots, sizeof *q5_per_slot);
    q5_has      = (uint8_t  *)calloc(tree.n_slots, sizeof *q5_has);
    if (!q5_per_slot || !q5_has) {
        rc = SPFY_E_NOMEM; free(seg_names); goto fail;
    }
    if ((rc = spfy_derive_q5_table(&tree, (const char **)fe_utt.word_names,
                                    fe_utt.n_words, q5_per_slot, q5_has))
        != SPFY_OK) {
        free(seg_names); goto fail;
    }

    /* Engine-faithful predecessor topology (FUN_08e8c700 LinkGraph). For
     * each slot S, preds = exit_chain(left-sibling of nearest ancestor
     * with a left sibling) = [P, P.last_child, ..., leaf]. This produces
     * the parallel-path DAG where Word/Syl anchor slots are alternative
     * routes that bypass the per-HP path through their span. */
    if ((rc = spfy_link_graph(&tree, &preds_tab)) != SPFY_OK) {
        free(seg_names); goto fail;
    }
    if (spfy_env("SPFY_DUMP_TREE")) {
        for (uint32_t s = 0; s < tree.n_slots; ++s) {
            const spfy_slot_node_t *n = &tree.slots[s];
            const char *ks = (n->kind == SPFY_SK_HALFPHONE) ? "HP"
                            : (n->kind == SPFY_SK_SYLLABLE) ? "SYL"
                            : (n->kind == SPFY_SK_WORD)     ? "WORD"
                            : "PHR";
            fprintf(stderr, "  tree[%u] kind=%s parent=%u nchildren=%u",
                    s, ks, n->parent_idx, n->n_children);
            if (n->n_children > 0) {
                fprintf(stderr, " children=[");
                for (uint32_t i = 0; i < n->n_children; ++i)
                    fprintf(stderr, "%s%u", i?",":"", n->child_idx[i]);
                fprintf(stderr, "]");
            }
            fprintf(stderr, " preds=[");
            for (uint32_t i = 0; i < preds_tab.per_slot[s].n_preds; ++i)
                fprintf(stderr, "%s%u", i?",":"", preds_tab.per_slot[s].preds[i]);
            fprintf(stderr, "]\n");
        }
    }

    /* Iterate halfphone-leaf slots only. */
    uint32_t n_hp = tree.n_halfphone;
    hp_to_post = (uint32_t *)calloc(n_hp, sizeof *hp_to_post);
    if (!hp_to_post) {
        rc = SPFY_E_NOMEM; free(seg_names); goto fail;
    }
    post_to_hp = (uint32_t *)malloc(tree.n_slots * sizeof *post_to_hp);
    if (!post_to_hp) {
        rc = SPFY_E_NOMEM; free(seg_names); goto fail;
    }
    for (uint32_t s = 0; s < tree.n_slots; ++s) post_to_hp[s] = UINT32_MAX;
    {
        uint32_t k = 0;
        for (uint32_t s = 0; s < tree.n_slots; ++s) {
            if (tree.slots[s].kind == SPFY_SK_HALFPHONE) {
                hp_to_post[k] = s;
                post_to_hp[s] = k;
                ++k;
            }
        }
    }
    if (synth_is_verbose()) {
        fprintf(stdout, "FE produced %u halfphone slots for text: %s\n", n_hp, text);
        /* Also emit a stderr marker so multi-phrase audit parsers can
         * detect phrase boundaries reliably even when stdout/stderr buffers
         * are flushed out-of-order through a merged pipe. */
        fflush(stdout);
        fprintf(stderr, "\nspfy_phrase_boundary: phrase_idx=%u n_hp=%u\n",
                phrase_idx, n_hp);
        fflush(stderr);
    }
    if (n_hp == 0) { rc = SPFY_E_FORMAT; goto fail; }

    /* [live-trace] phrase start: emitted once n_hp is known and before any
     * candidate scoring, so the viz gets phrase -> slots -> cands -> pick
     * -> unit in temporal order. */
    spfy_trace_eventf("phrase",
        "{\"idx\":%u,\"n_hp\":%u,\"t\":%u}",
        phrase_idx, n_hp, sink->n_samples_written);

#ifdef SPFY_TRACE
    /* [live-trace] Per-word phone breakdown for the Synthesis Tracer's word
     * grouping ({text, [arpabet phones]}, keyed by phrase). */
    for (int wi_ = 0; wi_ < parsed->n_words; ++wi_) {
        const fe_parsed_word_t *w_ = &parsed->words[wi_];
        if ((uint32_t)w_->phrase_id != phrase_idx) continue;
        char wbuf[4096];
        int wo = snprintf(wbuf, sizeof wbuf, "{\"phrase\":%u,\"text\":\"", phrase_idx);
        for (const char *t = w_->text; *t && wo < (int)sizeof wbuf - 8; ++t) {
            if (*t == '"' || *t == '\\') wbuf[wo++] = '\\';
            wbuf[wo++] = *t;
        }
        wo += snprintf(wbuf + wo, sizeof wbuf - (size_t)wo, "\",\"phones\":[");
        for (int pi = 0; pi < w_->n_phonemes && wo < (int)sizeof wbuf - 24; ++pi) {
            wo += snprintf(wbuf + wo, sizeof wbuf - (size_t)wo, "%s\"%s\"",
                           pi ? "," : "", w_->phonemes[pi].arpabet);
        }
        if (wo > (int)sizeof wbuf - 4) wo = (int)sizeof wbuf - 4;
        snprintf(wbuf + wo, sizeof wbuf - (size_t)wo, "]}");
        spfy_trace_event("word", wbuf);
    }
#endif

    anchor_cands  = (uint32_t **)calloc(tree.n_slots, sizeof *anchor_cands);
    anchor_jks    = (uint32_t **)calloc(tree.n_slots, sizeof *anchor_jks);
    anchor_target = (float    **)calloc(tree.n_slots, sizeof *anchor_target);
    anchor_n      = (uint32_t  *)calloc(tree.n_slots, sizeof *anchor_n);
    anchor_c68    = (uint8_t **)calloc(tree.n_slots, sizeof *anchor_c68);
    anchor_c6c    = (uint8_t **)calloc(tree.n_slots, sizeof *anchor_c6c);
    anchor_c70    = (uint8_t **)calloc(tree.n_slots, sizeof *anchor_c70);
    anchor_c78    = (uint8_t **)calloc(tree.n_slots, sizeof *anchor_c78);
    if (!anchor_cands || !anchor_jks || !anchor_target || !anchor_n
        || !anchor_c68 || !anchor_c6c || !anchor_c70 || !anchor_c78) {
        rc = SPFY_E_NOMEM; free(seg_names); goto fail;
    }

    vslots = (spfy_viterbi_slot_t *)calloc(n_hp, sizeof *vslots);
    cbuf   = (uint32_t **)calloc(n_hp, sizeof *cbuf);
    tbuf   = (float    **)calloc(n_hp, sizeof *tbuf);
    cand_c68 = (uint8_t **)calloc(n_hp, sizeof *cand_c68);
    cand_c6c = (uint8_t **)calloc(n_hp, sizeof *cand_c6c);
    cand_c70 = (uint8_t **)calloc(n_hp, sizeof *cand_c70);
    cand_c78 = (uint8_t **)calloc(n_hp, sizeof *cand_c78);
    hp_word_idx = (uint32_t *)calloc(n_hp, sizeof *hp_word_idx);
    if (!vslots || !cbuf || !tbuf || !hp_word_idx
        || !cand_c68 || !cand_c6c || !cand_c70 || !cand_c78) {
        rc = SPFY_E_NOMEM; free(seg_names); goto fail;
    }
    hp_btone = (int8_t *)calloc(n_hp, sizeof *hp_btone);
    hp_acctype = (int8_t *)calloc(n_hp, sizeof *hp_acctype);
    /* Accent PRESENCE, distinct from accent TYPE: accent_type_st() returns
     * 0 for both H* and "no accent", so hp_acctype alone cannot tell the
     * prosody stage where the accents are. */
    hp_accent = (uint8_t *)calloc(n_hp, sizeof *hp_accent);
    hp_syl    = (uint32_t *)calloc(n_hp, sizeof *hp_syl);
    hp_nuc    = (uint8_t *)calloc(n_hp, sizeof *hp_nuc);
    if (!hp_btone || !hp_acctype || !hp_accent || !hp_syl || !hp_nuc) {
        rc = SPFY_E_NOMEM; free(seg_names); goto fail;
    }
    for (uint32_t hp = 0; hp < n_hp; ++hp) {
        uint32_t s = hp_to_post[hp];
        uint32_t syl_post = tree.slots[s].parent_idx;
        uint32_t word_post = (syl_post < tree.n_slots)
                             ? tree.slots[syl_post].parent_idx
                             : UINT32_MAX;
        hp_word_idx[hp] = word_post;
        /* Per-hp boundary tone + accent type via the syllable slot's
         * fe_shared id. */
        if (syl_post < tree.n_slots
            && tree.slots[syl_post].kind == SPFY_SK_SYLLABLE) {
            uint32_t shared = tree.slots[syl_post].fe_shared;
            if (shared >= 1 && (shared - 1) < fe_utt.n_syls) {
                /* Merged index: a liaison continuation half-phone would
                 * otherwise read its OWN word's syllable record rather than
                 * the record of the syllable it actually belongs to. */
                uint32_t si = spfy_syl_effective(&fe_utt, shared - 1);
                if (fe_utt.syl_btone)
                    hp_btone[hp] = fe_utt.syl_btone[si];
                if (fe_utt.syl_acctype)
                    hp_acctype[hp] = fe_utt.syl_acctype[si];
                if (fe_utt.syl_accent)
                    hp_accent[hp] = fe_utt.syl_accent[si] ? 1u : 0u;
            }
            /* Syllable identity, so the contour can keep two abutting
             * accented syllables apart. */
            if (hp_syl) hp_syl[hp] = tree.slots[syl_post].fe_shared;
        }
        /* Nucleus flag. */
        if (hp_nuc && s < tree.n_slots && v->phone_order.phone_names) {
            uint32_t ph = slice_ctx.ctx[s][2] >> 1;
            if (ph < v->phone_order.n_phones)
                hp_nuc[hp] = phone_is_nucleus(
                    v->phone_order.phone_names[ph]) ? 1u : 0u;
        }
    }

    /* Per-hp SSML / Balabolka prosody overrides. */
    hp_pitch_st = (int8_t *)calloc(n_hp, sizeof *hp_pitch_st);
    hp_rate_pct = (int8_t *)calloc(n_hp, sizeof *hp_rate_pct);
    hp_tobi     = (uint8_t *)calloc(n_hp, sizeof *hp_tobi);
    syl_vol = (uint16_t *)calloc(fe_utt.n_syls ? fe_utt.n_syls : 1,
                                 sizeof *syl_vol);
    if (!hp_pitch_st || !hp_rate_pct) {
        rc = SPFY_E_NOMEM; free(seg_names); goto fail;
    }
    {
        uint32_t *word_post_to_parsed =
            (uint32_t *)malloc(tree.n_slots * sizeof *word_post_to_parsed);
        if (word_post_to_parsed) {
            for (uint32_t s = 0; s < tree.n_slots; ++s)
                word_post_to_parsed[s] = UINT32_MAX;
            /* OFF-BY-ONE, fixed 2026-08-05. */
            uint32_t wc = 0;
            int seen_first_word = 0;
            uint32_t last_word_slot = UINT32_MAX;
            for (uint32_t s = 0; s < tree.n_slots; ++s)
                if (tree.slots[s].kind == SPFY_SK_WORD) last_word_slot = s;
            for (uint32_t s = 0; s < tree.n_slots; ++s) {
                if (tree.slots[s].kind != SPFY_SK_WORD) continue;
                if (!seen_first_word) {
                    seen_first_word = 1;
                    continue;
                }
                if (s == last_word_slot) continue;
                word_post_to_parsed[s] = wc++;
            }
            /* Fill the offsets the FE left unspecified, ONCE, before
             * anything keyed on them reads. */
            /* The accessor hands back a const view; this is the single
             * place that writes back into it, and only to fill a field the
             * FE itself left unspecified. */
            fe_fill_char_starts((void *)(uintptr_t)spfy_fe_get_parsed(v->fe),
                                text);
            const fe_parsed_t *parsed_ro =
                (const fe_parsed_t *)spfy_fe_get_parsed(v->fe);
            if (parsed_ro) {
                for (uint32_t hp = 0; hp < n_hp; ++hp) {
                    uint32_t wpost = hp_word_idx[hp];
                    if (wpost < tree.n_slots) {
                        uint32_t pi = word_post_to_parsed[wpost];
                        if (pi != UINT32_MAX && (int)pi < parsed_ro->n_words) {
                            hp_pitch_st[hp] = parsed_ro->words[pi].pitch_st;
                            hp_rate_pct[hp] = parsed_ro->words[pi].rate_pct;
                            /* \![ToBI:] rides the SAME word mapping as the
                             * per-word pitch offset, including its
                             * off-by-one fix, rather than re-deriving word
                             * identity. */
                            {
                                int cs = parsed_ro->words[pi].char_start;
                                /* STRESSED SYLLABLE ONLY. */
                                int stressed = 1;
                                {
                                    uint32_t sp2 = hp_to_post[hp];
                                    uint32_t syp = (sp2 < tree.n_slots)
                                                 ? tree.slots[sp2].parent_idx
                                                 : UINT32_MAX;
                                    if (syp < tree.n_slots
                                        && tree.slots[syp].kind
                                           == SPFY_SK_SYLLABLE
                                        && fe_utt.syl_stress) {
                                        uint32_t sh = tree.slots[syp].fe_shared;
                                        if (sh >= 1 && (sh - 1) < fe_utt.n_syls)
                                            stressed =
                                                (fe_utt.syl_stress[
                                                    spfy_syl_effective(
                                                        &fe_utt, sh - 1)] == 1);
                                    }
                                }
                                if (cs >= 0 && (size_t)cs < etag_maps_n
                                    && stressed) {
                                    if (hp_tobi && etag_acc)
                                        hp_tobi[hp] = etag_acc[cs];
                                    /* \!rp / \!rd were parsed into a map
                                     * that nothing ever read -- allocated,
                                     * filled, freed. */
                                    if (etag_rate && etag_rate[cs]
                                        && etag_rate[cs] != 100u) {
                                        int rp = (int)etag_rate[cs] - 100;
                                        if (rp < -100) rp = -100;
                                        if (rp >  100) rp =  100;
                                        hp_rate_pct[hp] = (int8_t)rp;
                                    }
                                }
                            }
                        }
                    }
                }
            }
            free(word_post_to_parsed);
        }
    }

    /* Per-syllable volume map for the \!vp/\!vd embedded tags, keyed by
     * fe_shared-1 (the reliable HP -> syllable id that hp_btone also uses;
     * the tree-word -> parsed-word count drifts when adjacent words merge). */
    if (syl_vol && etag_vol) {
        const fe_parsed_t *pv = (const fe_parsed_t *)spfy_fe_get_parsed(v->fe);
        if (pv) {
            uint32_t fw = 1;
            for (int wi = 0; wi < pv->n_words; ++wi) {
                if (pv->words[wi].phrase_id != (int)phrase_idx) continue;
                int cs = pv->words[wi].char_start;
                uint16_t vol = (cs >= 0 && (size_t)cs < etag_maps_n)
                               ? etag_vol[cs] : 0;
                if (spfy_env("SPFY_ETAG_DUMP"))
                    fprintf(stderr, "[etag] vol word wi=%d fw=%u "
                            "char_start=%d -> vol=%u\n", wi, fw, cs, vol);
                if (vol && fw < fe_utt.n_words && fe_utt.word_syls[fw]) {
                    for (uint32_t j = 0; j < fe_utt.word_n_syls[fw]; ++j) {
                        uint32_t sh = fe_utt.word_syls[fw][j];
                        if (sh >= 1 && (sh - 1) < fe_utt.n_syls)
                            syl_vol[sh - 1] = vol;
                    }
                }
                ++fw;
            }
        }
    }

    uint32_t total_cands = 0, n_empty = 0;
    int verbose = (spfy_env("SPFY_SYNTH_DEBUG") != NULL);
    for (uint32_t hp = 0; hp < n_hp; ++hp) {
        uint32_t s = hp_to_post[hp];
        const uint32_t *ctx5 = slice_ctx.ctx[s];
        const uint32_t *sp5  = sp_tab.sp[s];
        uint32_t q5 = q5_per_slot[s];

        /* PRSL pool query: triphone context = ctx[1]*10000+ctx[2]*100+ctx[3]. */
        uint32_t hp_bound = v->phone_order.n_phones
                              ? v->phone_order.n_phones * 2u : 92u;
        uint32_t ck = spfy_prsl_context_key(ctx5[1], ctx5[2], ctx5[3]);
        /* Two possible pool provenances, kept apart because they differ in
         * alignment: PRSL hands back a u32[] aliasing the VIN buffer at an
         * arbitrary offset (read via spfy_prsl_cand), while the hp-bucket
         * fallback below is an... */
        const uint8_t  *pool       = NULL;
        const uint32_t *pool_bucket = NULL;
        uint32_t pool_n = 0;
        /* ⚠ A PHRASE'S FIRST AND LAST HALF-PHONE ARE PINNED, NOT PRESELECTED.
         *
         * The engine hands those two slots a pool of exactly ONE unit: index
         * 0 at the front and index n_units-1 at the back. Measured on the
         * engine's own prsl_slot records over 983 phrases and five voices --
         * tom 323, jill 306, javier 120, paulina 120, felix 114 -- with ZERO
         * exceptions, and the pinned uid is n_units-1 for every one of them
         * (tom 169578, jill 185474, javier 219500, paulina 663409, felix
         * 259659).
         *
         * We were running the ordinary triphone lookup there instead, which
         * usually returns a group of one anyway -- that is why only felix
         * showed it, on the 11 slots where the group holds 2 or 3. The extra
         * units are real pauses from mid-recording; the pinned one is the
         * recording-boundary pau, and its phone_ctx carries the 255 sentinel
         * on the missing side (uid 0: `255 255 43 35`).
         *
         * SPFY_NO_BOUNDARY_PIN=1 restores the lookup. */
        static int no_pin = -1;
        if (no_pin < 0) no_pin = (spfy_env("SPFY_NO_BOUNDARY_PIN") != NULL);
        uint32_t pin_uid = 0;
        if (!no_pin && n_hp > 0 && (hp == 0 || hp == n_hp - 1u)
            && v->units.n_units > 0) {
            pin_uid     = (hp == 0) ? 0u : (v->units.n_units - 1u);
            pool_bucket = &pin_uid;
            pool_n      = 1u;
            goto pool_ready;
        }
        /* Which rung of the backoff ladder fed this slot. A candidate reached
         * through a fallback carries a context unrelated to the slot's, so
         * its prsl key does not satisfy the family relation S4 builds the
         * `hash` domain from -- and every join touching it is a guaranteed
         * miss at MISSING_JOIN_COST. Counting the rungs is what separates
         * "our preselection needs more KEYS" from "it needs more MEMBERS".
         * Printed at the end of the run under SPFY_PRSL_STATS. */
        ++g_prsl_rung_total;
        spfy_prsl_lookup(&v->prsl, ck, &pool, &pool_n);
        if (pool_n) ++g_prsl_rung_exact;
        if (pool_n == 0 && !spfy_env("SPFY_NO_PRSL_92_FALLBACK")) {
            uint32_t side = ctx5[2] & 1u;
            uint32_t l_fb = side ? hp_bound : ctx5[1];
            uint32_t r_fb = side ? ctx5[3] : hp_bound;
            uint32_t ck_fb = spfy_prsl_context_key(l_fb, ctx5[2], r_fb);
            spfy_prsl_lookup(&v->prsl, ck_fb, &pool, &pool_n);
            if (pool_n) ++g_prsl_rung_1side;
            /* ⚠ THE SUBSTITUTION ESCALATES. When the one-sided key misses
             * too, the engine drops BOTH contexts and takes the group keyed
             * on the centre class alone.
             *
             * aicraig nat_049 hp 78 (`airplane`, eh, left half) is the case:
             * ctx [72,42,24,66,62], so (42,24,66) -> miss and the one-sided
             * (42,24,92) -> miss as well, leaving our pool EMPTY while the
             * engine had 2174 candidates. Key (92,24,92) returns exactly
             * 2174, which is what named this. An empty pool is not a cost
             * problem: the engine's own uid was unreachable at any price.
             *
             * SPFY_NO_PRSL_BOTH_FALLBACK=1 stops at the one-sided level. */
            static int no_both = -1;
            if (no_both < 0)
                no_both = (spfy_env("SPFY_NO_PRSL_BOTH_FALLBACK") != NULL);
            if (pool_n == 0 && !no_both) {
                uint32_t ck_both = spfy_prsl_context_key(hp_bound, ctx5[2],
                                                         hp_bound);
                spfy_prsl_lookup(&v->prsl, ck_both, &pool, &pool_n);
                if (pool_n) ++g_prsl_rung_both;
                else        ++g_prsl_rung_empty;
            }
        }
    pool_ready:
        /* NO clamp by default. */
        {
            static long cap = -1;
            if (cap < 0) {
                const char *cap_env = spfy_env("SPFY_MAX_CANDS");
                cap = (cap_env && *cap_env) ? strtol(cap_env, NULL, 10) : 0;
                if (cap < 1) cap = 0;
            }
            if (cap > 0 && pool_n > (uint32_t)cap) pool_n = (uint32_t)cap;
        }

        /* Build a tiny adapter slot for the cart_feat callback (it expects
         * spfy_fe_slot_t.ctx + .sp). */
        spfy_fe_slot_t adapter = {0};
        for (int i = 0; i < 5; ++i) {
            adapter.ctx[i] = (int32_t)ctx5[i];
            adapter.sp[i]  = sp5[i];
        }

        if (verbose) {
            float dm_dbg = 0, dv_dbg = 0, fm_dbg = 0, fv_dbg = 0;
            /* Plan 03-04: silence-pad CART traversal for the debug-JSON emit path. */
            int silence_slot_dbg = ctx_is_silence(v, ctx5[2]);
            cart_feat_ctx_t cfc_dbg = { &adapter, q5, v, 0 };
            if (!silence_slot_dbg || !spfy_env("SPFY_NO_SILENCE_CART")) {
                uint32_t didx = phone_to_labl(v, ctx5[2] >> 1);
                if (didx < v->durt_cart.n_trees)
                    spfy_cart_traverse(&v->durt_cart, didx, cart_feat, &cfc_dbg,
                                       &dm_dbg, &dv_dbg);
                if (v->f0tr_cart.n_trees > 0) {
                    cfc_dbg.is_f0tr = 1;
                    spfy_cart_traverse(&v->f0tr_cart, 0, cart_feat, &cfc_dbg,
                                       &fm_dbg, &fv_dbg);
                    cfc_dbg.is_f0tr = 0;
                }
            }
            fprintf(stderr, "{\"hp\":%u,\"post\":%u,\"ctx\":[%u,%u,%u,%u,%u],"
                            "\"sp\":[%u,%u,%u,%u,%u],\"q5\":%u,"
                            "\"durt_mean\":%.4f,\"durt_var\":%.4f,"
                            "\"f0tr_mean\":%.4f,\"f0tr_var\":%.4f,"
                            "\"pool_n\":%u",
                    hp, s,
                    ctx5[0], ctx5[1], ctx5[2], ctx5[3], ctx5[4],
                    sp5[0], sp5[1], sp5[2], sp5[3], sp5[4],
                    q5,
                    (double)dm_dbg, (double)dv_dbg,
                    (double)fm_dbg, (double)fv_dbg,
                    pool_n);
            fprintf(stderr, ",\"cands\":[");
            /* Cap at 16 for the audit output (matches master_compare's
             * legacy expectation). */
            uint32_t cand_cap = spfy_env("SPFY_FULL_POOL_DUMP") ? pool_n : 16u;
            uint32_t dump_n = pool_n < cand_cap ? pool_n : cand_cap;
            for (uint32_t i = 0; i < dump_n; ++i) {
                /* ⚠ Must go through pool_cand(), which knows about BOTH
                 * provenances. Printing SILENCE_SENTINEL_UID whenever the
                 * PRSL pointer is NULL reported a uid the synth never used
                 * for every bucket-fallback and pinned slot -- and the audit
                 * reads this line, so it scored a divergence that was only in
                 * the dump. */
                fprintf(stderr, "%s%u", i ? "," : "",
                        (unsigned)pool_cand(pool, pool_bucket, i));
            }
            fprintf(stderr, "]}\n");
        }
        if (pool_n == 0) ++n_empty;

        uint32_t fb_hp = ctx5[2];
        if (pool_n == 0 && fb_hp < v->hpc_buckets && v->bucket_n[fb_hp] > 0) {
            pool        = NULL;
            pool_bucket = v->bucket[fb_hp];
            pool_n = v->bucket_n[fb_hp];
            if (pool_n > MAX_CANDS_PER_SLOT) pool_n = MAX_CANDS_PER_SLOT;
        }

        /* [live-trace] slot header: the half-phone target and its final
         * candidate-pool size (after PRSL, 92-fallback, and bucket
         * fallback). */
        spfy_trace_eventf("slot",
            "{\"slot\":%u,\"phone\":%u,\"pool_n\":%u}",
            hp, ctx5[2], pool_n);

        if (pool_n == 0) {
            cbuf[hp] = (uint32_t *)calloc(1, sizeof **cbuf);
            tbuf[hp] = (float    *)calloc(1, sizeof **tbuf);
            if (!cbuf[hp] || !tbuf[hp]) { rc = SPFY_E_NOMEM; goto fail; }
            cbuf[hp][0] = SILENCE_UID(v);
            tbuf[hp][0] = 0.0f;
            vslots[hp].cands = cbuf[hp];
            vslots[hp].target_cost = tbuf[hp];
            vslots[hp].n_cands = 1;
            continue;
        }

        cbuf[hp] = (uint32_t *)calloc(pool_n, sizeof **cbuf);
        tbuf[hp] = (float    *)calloc(pool_n, sizeof **tbuf);
        cand_c68[hp] = (uint8_t *)calloc(pool_n, sizeof **cand_c68);
        cand_c6c[hp] = (uint8_t *)calloc(pool_n, sizeof **cand_c6c);
        cand_c70[hp] = (uint8_t *)calloc(pool_n, sizeof **cand_c70);
        cand_c78[hp] = (uint8_t *)calloc(pool_n, sizeof **cand_c78);
        if (!cbuf[hp] || !tbuf[hp] || !cand_c68[hp] || !cand_c6c[hp]
            || !cand_c70[hp] || !cand_c78[hp]) {
            rc = SPFY_E_NOMEM; goto fail;
        }
        /* SPFY_UID_DUMP - see spfy_uid_dump_fp(). One NDJSON record per
         * half-phone slot listing EVERY candidate the DP was allowed to
         * consider. The matching "pick" record is emitted after the DP, and
         * the two join on (phrase, slot).
         *
         * Emitted here, where the pool is built, rather than after the DP:
         * these buffers are phrase-scoped and this avoids any assumption
         * about how long they stay alive. Diagnostics must not be able to
         * introduce a lifetime bug into the shipped path.
         *
         * ⚠ ENV-GATED AND SIDE-EFFECT FREE. Unset, this costs one getenv per
         * phrase and changes nothing - the byte-exact audit must stay
         * 226/226 with it absent AND present. */
        if (spfy_uid_dump_fp()) {
            FILE *uf = spfy_uid_dump_fp();
            fprintf(uf, "{\"t\":\"cands\",\"phrase\":%u,\"slot\":%u,"
                        "\"phone\":%u,\"n\":%u,\"uids\":[",
                    (unsigned)phrase_idx, (unsigned)hp,
                    (unsigned)ctx5[2], (unsigned)pool_n);
            for (uint32_t i = 0; i < pool_n; ++i)
                fprintf(uf, "%s%u", i ? "," : "",
                        (unsigned)pool_cand(pool, pool_bucket, i));
            fprintf(uf, "]}\n");
        }

        for (uint32_t i = 0; i < pool_n; ++i) {
            uint32_t cand_uid = pool_cand(pool, pool_bucket, i);
            cbuf[hp][i] = cand_uid;
            spfy_unit_record_t ur;
            if (spfy_unit_record_get(&v->units, cand_uid, &ur) == SPFY_OK) {
                /* mem+0x10 = disk+0x11 = f0_end, mem+0x11 = disk+0x12 =
                 * f0_mid, mem+0x0f = disk+0x10 = f0_start. */
                cand_c6c[hp][i] = ur.f0_end;
                cand_c68[hp][i] = ur.f0_mid;
                cand_c70[hp][i] = ur.f0_start;
                cand_c78[hp][i] = 0;   /* engine zeros at FUN_08e8abe0 */
            }
        }

        spfy_anchor_ctx_t ctx_in;
        for (int i = 0; i < 5; ++i) ctx_in.ctx[i] = (int32_t)ctx5[i];
        spfy_anchor_sp_target_t sp_in;
        for (int i = 0; i < 5; ++i) sp_in.sp[i] = sp5[i];

        cart_feat_ctx_t cfc = { &adapter, q5, v, 0 };
        spfy_anchor_cart_t cart = {0};
        /* Silence-pad CART traversal: engine emits durt-CART leaf
         * statistics for silence slots (ctx5[2] in {64, 65} = HP_PAU_L/R)
         * the same way it does for non-silence; we used to skip these slots
         * and emit (0, 0), which... */
        int silence_slot = ctx_is_silence(v, ctx5[2]);
        if (!silence_slot || !spfy_env("SPFY_NO_SILENCE_CART")) {
            uint32_t durt_idx = phone_to_labl(v, ctx5[2] >> 1);
            if (durt_idx < v->durt_cart.n_trees) {
                if (spfy_cart_traverse(&v->durt_cart, durt_idx, cart_feat, &cfc,
                                       &cart.durt_mean, &cart.durt_var) == SPFY_OK)
                    cart.durt_valid = 1;
            }
            if (v->f0tr_cart.n_trees > 0) {
                cfc.is_f0tr = 1;
                if (spfy_cart_traverse(&v->f0tr_cart, 0, cart_feat, &cfc,
                                       &cart.f0tr_mean, &cart.f0tr_var) == SPFY_OK) {
                    cart.f0tr_valid = 1;
                    /* Pitch shift via unit-selection bias - see
                     * spfy_synth_set_pitch_semitones(). */
                    cart.f0tr_mean *= v->pitch_scale;
                }
                cfc.is_f0tr = 0;
            }
            /* SSML / Balabolka per-word prosody overrides. */
            if (cart.durt_valid && hp_rate_pct[hp]) {
                cart.durt_mean *= 100.0f
                    / (100.0f + (float)hp_rate_pct[hp]);
            }
            /* SPFY_PROSODY_WORD_PITCH_NOSEL=1 keeps a per-word pitch tag
             * out of SELECTION so it acts only through the prosody stage's
             * contour. */
            static int no_sel_pitch = -1;
            if (no_sel_pitch < 0)
                no_sel_pitch = (spfy_env("SPFY_PROSODY_WORD_PITCH_NOSEL") != NULL);
            if (cart.f0tr_valid && hp_pitch_st[hp] && !no_sel_pitch) {
                cart.f0tr_mean *= powf(2.0f,
                    (float)hp_pitch_st[hp] / 12.0f);
            }
            /* Global scale on durt CART target. */
            if (cart.durt_valid) {
                static float dscale = -1.0f;
                if (dscale < 0.0f) {
                    const char *e = spfy_env("SPFY_DURT_SCALE");
                    dscale = (e && *e) ? (float)atof(e) : 1.0f;
                    if (dscale < 0.1f) dscale = 0.1f;
                    if (dscale > 5.0f) dscale = 5.0f;
                }
                if (dscale != 1.0f) cart.durt_mean *= dscale;
            }
            /* Option A: boundary-tone F0 target bias. */
            if (cart.f0tr_valid && hp_btone[hp]
                && spfy_env("SPFY_PROSODY_REALIZE")) {
                float gain = 1.0f;
                const char *g = spfy_env("SPFY_PROSODY_BT_GAIN");
                if (g) { float f = (float)atof(g); if (f >= 0.0f) gain = f; }
                cart.f0tr_mean *= powf(2.0f,
                    (float)hp_btone[hp] * gain / 12.0f);
            }
            /* Accent-TYPE F0 target bias. */
            if (cart.f0tr_valid && hp_acctype[hp]) {
                static float atgain = -1.0f;
                if (atgain < 0.0f) {
                    const char *g = spfy_env("SPFY_SPFY4_ACCTYPE_GAIN");
                    atgain = (g && *g) ? (float)atof(g) : 0.0f;
                    if (atgain < 0.0f) atgain = 0.0f;
                    if (atgain > 3.0f) atgain = 3.0f;
                }
                if (atgain > 0.0f) {
                    cart.f0tr_mean *= powf(2.0f,
                        (float)hp_acctype[hp] * atgain / 12.0f);
                }
            }
            /* Speechify-4 accent-height (F0-range) compression. */
            if (cart.f0tr_valid) {
                static float f0range = -1.0f, f0base = -1.0f;
                if (f0range < 0.0f) {
                    const char *e = spfy_env("SPFY_SPFY4_F0_RANGE");
                    f0range = (e && *e) ? (float)atof(e) : 1.0f;
                    if (f0range < 0.05f) f0range = 0.05f;
                    if (f0range > 3.0f)  f0range = 3.0f;
                    const char *b = spfy_env("SPFY_SPFY4_F0_BASE");
                    f0base = (b && *b) ? (float)atof(b) : 120.0f;
                    if (f0base < 50.0f)  f0base = 50.0f;
                    if (f0base > 400.0f) f0base = 400.0f;
                }
                if (f0range != 1.0f) {
                    cart.f0tr_mean = f0base
                        + (cart.f0tr_mean - f0base) * f0range;
                }
            }
        }

        /* SPFY_HP_COMP_DUMP marker: emit a per-slot header so the follow-on
         * hp_comp lines can be associated with this hp/slot. */
        if (spfy_env("SPFY_HP_COMP_DUMP")) {
            fprintf(stderr,
                "{\"hp_comp_slot\":1,\"hp\":%u,\"post\":%u,\"pool_n\":%u}\n",
                hp, s, pool_n);
        }
        for (uint32_t i = 0; i < pool_n; ++i) {
            float c = NAN;
            int rcs = spfy_hp_innerscorer(&v->av, &ctx_in, &sp_in, &cart,
                                          cbuf[hp][i], &c);
            tbuf[hp][i] = (rcs != SPFY_OK || isnan(c) || isinf(c)) ? 1e9f : c;
            /* [live-trace] one event per scored candidate - the raw search
             * space the viz shows filling in and then getting pruned by the
             * Viterbi. */
            spfy_trace_eventf("cand",
                "{\"slot\":%u,\"uid\":%u,\"tc\":%.4f}",
                hp, cbuf[hp][i], (double)tbuf[hp][i]);
        }

        /* Engine FUN_08e88de0 running-min early exit (decompiled
         * 2026-07-20). The engine pre-sets every candidate's cost to the
         * 10000.0f sentinel (`MOV [..+4],0x461c4000`), then accumulates
         * the components in stages -- SP+flag, ccos, D, F0 -- re-testing
         * `cost <= bound` after each and abandoning the candidate the
         * moment it exceeds. Only a candidate that clears all four stages
         * is stored, and only then:
         *
         *     if (cost < running_min) {
         *         running_min = cost;
         *         bound = slack + cost;      // local_50 = fVar4 + fVar14
         *     }
         *
         * with `running_min` and `bound` both starting at 10000.0f, and
         * `slack` read from cfg+0x4c -- which is the SAME field the prune
         * takes as its THRESH, i.e. HALFPHONE_CAND_PRUNE_THRESH. No new
         * constant. `running_min` is then what gets passed to
         * FUN_08e88830 as `best`.
         *
         * Staged bail-out is equivalent to a single post-hoc test here:
         * every component is non-negative, so the partial sums are
         * monotonic and `partial > bound` implies `full > bound`. What
         * matters is that the bound tightens IN POOL ORDER, so this must
         * run as a left-to-right sweep and not as a min-then-filter.
         *
         * SPFY_NO_HP_EARLY_EXIT=1 disables; SPFY_HP_EARLY_EXIT_VAL
         * overrides the slack for A/B work. */
        if (!spfy_env("SPFY_NO_HP_EARLY_EXIT")) {
            float slack = v->hp_prune_thresh;
            const char *eev = spfy_env("SPFY_HP_EARLY_EXIT_VAL");
            if (eev && *eev) slack = (float)atof(eev);

            float running_min = 10000.0f;
            float bound       = 10000.0f;
            for (uint32_t i = 0; i < pool_n; ++i) {
                float t = tbuf[hp][i];
                if (t <= bound) {
                    if (t < running_min) {
                        running_min = t;
                        bound = slack + t;
                    }
                } else {
                    tbuf[hp][i] = 10000.0f;
                }
            }
        }

        /* SPFY_TC_DUMP - per-cand target cost dump (post-inner-scorer,
         * pre-hist-prune). */
        if (spfy_env("SPFY_TC_DUMP")) {
            fprintf(stderr, "{\"tc_dump\":1,\"hp\":%u,\"post\":%u,\"pool_n\":%u,\"uids\":[", hp, s, pool_n);
            for (uint32_t i = 0; i < pool_n; ++i)
                fprintf(stderr, "%s%u", i ? "," : "", cbuf[hp][i]);
            fprintf(stderr, "],\"tc\":[");
            for (uint32_t i = 0; i < pool_n; ++i)
                fprintf(stderr, "%s%.6f", i ? "," : "", (double)tbuf[hp][i]);
            fprintf(stderr, "]}\n");
        }

        /* Engine-faithful HP-cand histogram prune + sort (FUN_08e88830).
         * Builds a 40-bin histogram of (cost - best) * 40 (so each bin
         * spans 0.025 cost v->units), walks bins until slack drops below
         * bin_dist or cum exceeds MAX_UNITS, then drops cands with cost
         * above (best + bin_dist). Sorts survivors by cost ascending
         * (engine uses shell-sort; we use qsort) and caps at MAX_UNITS.
         *
         * Tom's params (from VCF): THRESH=0.8, SLOPE=0.005, MAX=50.
         * Constants: bin_width=0.025, scale=40.0 (engine globals
         * _DAT_08e98520, _DAT_08e98524).
         *
         * Skip when pool is the silence-sentinel fallback (already 1
         * cand). */
        /* HP cand histogram_prune (FUN_08e88830). Engine-faithful core;
         * scoring-time running-min gating in FUN_08e88de0 not yet
         * implemented (engine prunes tighter than histogram alone).
         * THRESH/SLOPE come from the voice's VCF (Tom 0.8/0.005, Jill
         * 1.0/0.005); MAX=50, BIN_WIDTH=0.025, SCALE=40 are engine
         * globals, not per-voice. SPFY_NO_HP_PRUNE=1 disables. */
        /* NB: a "skip the prune for pools smaller than N" guard was tested
         * 2026-07-20 (engine traces show large kept-deltas at small pool
         * sizes on both voices) and REJECTED: N=3 already costs Tom
         * 8532->8525 while gaining Jill... */
        if (pool_n > 1 && !spfy_env("SPFY_NO_HP_PRUNE")) {
            const float HP_THRESH    = v->hp_prune_thresh;
            const float HP_SLOPE     = v->hp_prune_slope;
            const float HP_BIN_WIDTH = 0.025f;
            const float HP_SCALE     = 40.0f;
            /* ⚠ NOT a hardcoded 50. That is the config CONSTRUCTOR's default
             * (0x08e90e64 `mov [esi+0x48], 0x32`); the VCF's
             * HALFPHONE_CAND_MAX_UNITS overrides it, and FUN_08e88de0 pushes
             * it into the prune alongside THRESH and SLOPE at 0x08e8938b.
             * Every SpeechWorks voice here omits the key, so the literal was
             * right for all of them and wrong for aimara2, which sets 200 --
             * we pruned four times harder than the engine and could drop the
             * unit it went on to choose. SPFY_HP_MAX=<n> overrides. */
            uint32_t HP_MAX = v->hp_prune_max ? v->hp_prune_max : 50u;
            {
                const char *mx = spfy_env("SPFY_HP_MAX");
                if (mx && *mx) {
                    long o = strtol(mx, NULL, 10);
                    if (o >= 1) HP_MAX = (uint32_t)o;
                }
            }

            float best = tbuf[hp][0];
            for (uint32_t i = 1; i < pool_n; ++i)
                if (tbuf[hp][i] < best) best = tbuf[hp][i];

            int bins[40] = {0};
            for (uint32_t i = 0; i < pool_n; ++i) {
                float diff = (tbuf[hp][i] - best) * HP_SCALE;
                /* Engine FUN_08e9504c binning is TRUNCATION (round-toward
                 * -zero), NOT round-to-nearest. The decomp shows FRNDINT
                 * (banker's round) followed by an adjustment subtracting
                 * 1 whenever the input wasn't exactly integer - net effect
                 * is floor for positive values.
                 *
                 * Verified 2026-05-14 via Frida trace of FUN_08e88830 on
                 * text_002 HP=4: engine bin_dist=0.725 (break at k=28),
                 * matching truncation binning. Previously we used lroundf
                 * which rounded 29.86 up to 30, putting 33584 in bin 29
                 * instead of bin 28 - engine breaks at k=29 instead of
                 * k=28 and keeps 17 cands instead of 16.
                 *
                 * SPFY_HP_BIN_LROUND=1 reverts to lroundf for A/B audit. */
                int bidx;
                if (diff >= (float)HP_SCALE) {
                    bidx = 39;
                } else if (spfy_env("SPFY_HP_BIN_LROUND")) {
                    bidx = (int)lroundf(diff);
                    if (bidx < 0) bidx = 0;
                    if (bidx > 39) bidx = 39;
                } else {
                    /* Truncation (floor for positive). */
                    bidx = (int)diff;
                    if (bidx < 0) bidx = 0;
                    if (bidx > 39) bidx = 39;
                }
                bins[bidx]++;
            }
            int cum = 0;
            float bin_dist = 40.0f * HP_BIN_WIDTH;
            /* Engine uses local_c8 starting at 2 (not 1), so bin_dist for
             * iteration k is (k+1)*HP_BIN_WIDTH not k*HP_BIN_WIDTH. */
            /* Bin index the scan broke at; 40 == ran to completion. */
            int break_k = 40;
            /* Both sides of the break test are evaluated at x87 EXTENDED
             * precision in the engine and never rounded to 32-bit floats
             * (FUN_08e88830 @ 08e888c6..08e888ee):
             *
             *   FILD (k+1) ; FMUL BIN_WIDTH        -> bd,  extended
             *   FILD cum   ; FMUL SLOPE ; FSUBR THRESH -> lhs, extended
             *   FCOMPP                              -> break when bd > lhs
             *
             * The rounding only happens later, once, when best is added
             * and the result is stored (FADD ; FSTP dword). Getting this
             * wrong flips the k=38 boundary: at cum=5, THRESH=1.0 the two
             * sides are 0.9750000005588 and 0.9750000145286 -- distinct in
             * extended, but they round to the SAME float, so a rounded
             * comparison sees equality and fails to break.
             *
             * long double is 80-bit on this 32-bit x86 target, matching
             * the FPU registers exactly. */
            long double bd_x = 40.0L * (long double)HP_BIN_WIDTH;
            for (int k = 0; k < 40; ++k) {
                cum += bins[k];
                long double cur_bd = (long double)(k + 1)
                                   * (long double)HP_BIN_WIDTH;
                long double lhs_x  = (long double)HP_THRESH
                                   - (long double)cum * (long double)HP_SLOPE;
                if ((uint32_t)cum > HP_MAX || lhs_x < cur_bd) {
                    bd_x = cur_bd; break_k = k; break;
                }
            }
            bin_dist = (float)bd_x;
            /* Engine: FADD best onto the still-extended bd, then ONE FSTP
             * to a 32-bit float. */
            float thresh = (float)(bd_x + (long double)best);
            /* The engine gates the entire threshold filter on the break
             * bin: `if (iVar7 < 0x27)` in FUN_08e88830. If the scan broke
             * at bin 39 or ran off the end, NO candidate is dropped --
             * only the sort and the HP_MAX cap below still run.
             *
             * This is not a corner case, it is the whole sparse-pool
             * story. The break needs THRESH - cum*SLOPE < (k+1)*0.025,
             * and cum >= 1 always (best sits in bin 0), so:
             *   Tom  THRESH=0.8: lhs <= 0.795 -> breaks by k=31, always
             *                    < 39, so Tom ALWAYS prunes (this guard
             *                    is provably a no-op for him).
             *   Jill THRESH=1.0: lhs <= 0.995 -> needs (k+1)*0.025 >
             *                    0.995, i.e. k=39 -> NO prune whenever
             *                    the pool is sparse enough that cum stays
             *                    low through the scan.
             * Without the guard we cut Jill's sparse slots to best+1.0 and
             * drop units the engine kept (text_004 uid 145844 at
             * best+1.883, which is on the engine's chosen run).
             *
             * This guard is only HALF the engine's candidate reduction --
             * it MUST be paired with the running-min early exit above.
             * Enabled alone it overshoots badly (Jill 93.8% -> 92.6%,
             * survivor mean 13.92 -> 17.33 vs an engine mean of 14.66),
             * because our tighter histogram cut had been standing in for
             * the missing early exit. SPFY_NO_HP_PRUNE_BIN39_GUARD=1
             * reverts to the old always-filter behaviour for A/B. */
            uint32_t kept;
            if (break_k < 39 || spfy_env("SPFY_NO_HP_PRUNE_BIN39_GUARD")) {
                kept = 0;
                for (uint32_t i = 0; i < pool_n; ++i) {
                    if (tbuf[hp][i] <= thresh) {
                        cbuf[hp][kept] = cbuf[hp][i];
                        tbuf[hp][kept] = tbuf[hp][i];
                        cand_c68[hp][kept] = cand_c68[hp][i];
                        cand_c6c[hp][kept] = cand_c6c[hp][i];
                        cand_c70[hp][kept] = cand_c70[hp][i];
                        cand_c78[hp][kept] = cand_c78[hp][i];
                        ++kept;
                    }
                }
            } else {
                kept = pool_n;
            }
            /* Sort kept cands by target_cost ascending, ties broken by
             * DESCENDING uid. The engine uses a shell sort whose swap
             * predicate decodes (iVar12 = j, iVar9 = j+gap) to
             *
             *   cost_j >= cost_jg && (cost_j != cost_jg || uid_j <= uid_jg)
             *
             * -- i.e. on equal cost it moves the LARGER uid earlier. The
             * int at cand+0 is the uid (FUN_08e88de0 indexes the unit
             * table with it). Ordering matters downstream because the
             * DP's predecessor scan and the early-exit bound both walk
             * candidates in array order.
             *
             * We use a selection sort (counts are small post-prune); with
             * a TOTAL comparator the result is identical to the engine's
             * shell sort regardless of algorithm, since uids are unique
             * within a pool so no two entries compare equal.
             * SPFY_NO_HP_SORT_UID_TIE=1 reverts to cost-only for A/B. */
            int sort_uid_tie = (spfy_env("SPFY_NO_HP_SORT_UID_TIE") == NULL);
            for (uint32_t a = 0; a + 1 < kept; ++a) {
                uint32_t mn = a;
                for (uint32_t b = a + 1; b < kept; ++b) {
                    if (tbuf[hp][b] < tbuf[hp][mn]
                        || (sort_uid_tie
                            && tbuf[hp][b] == tbuf[hp][mn]
                            && cbuf[hp][b] > cbuf[hp][mn])) mn = b;
                }
                if (mn != a) {
                    float    tt = tbuf[hp][a];
                    uint32_t cc = cbuf[hp][a];
                    uint8_t  c68v = cand_c68[hp][a];
                    uint8_t  c6cv = cand_c6c[hp][a];
                    uint8_t  c70v = cand_c70[hp][a];
                    uint8_t  c78v = cand_c78[hp][a];
                    tbuf[hp][a]    = tbuf[hp][mn];
                    cbuf[hp][a]    = cbuf[hp][mn];
                    cand_c68[hp][a]= cand_c68[hp][mn];
                    cand_c6c[hp][a]= cand_c6c[hp][mn];
                    cand_c70[hp][a]= cand_c70[hp][mn];
                    cand_c78[hp][a]= cand_c78[hp][mn];
                    tbuf[hp][mn]   = tt;
                    cbuf[hp][mn]   = cc;
                    cand_c68[hp][mn]= c68v;
                    cand_c6c[hp][mn]= c6cv;
                    cand_c70[hp][mn]= c70v;
                    cand_c78[hp][mn]= c78v;
                }
            }
            if (kept > HP_MAX) kept = HP_MAX;
            if (spfy_env("SPFY_PRUNE_DEBUG")) {
                fprintf(stderr, "{\"prune\":1,\"hp\":%u,\"pool_n_in\":%u,"
                                "\"kept\":%u,\"best\":%.9f,\"bin_dist\":%.6f,"
                                "\"thresh\":%.9f,\"break_k\":%d,"
                                "\"filtered\":%d,\"kept_uids\":[",
                        hp, pool_n, kept, (double)best, (double)bin_dist,
                        (double)(best + bin_dist), break_k, break_k < 39);
                /* cbuf[0..kept-1] is the compacted survivor set; entries
                 * past `kept` are stale leftovers, so only the survivors
                 * are meaningful here. */
                for (uint32_t i = 0; i < kept; ++i)
                    fprintf(stderr, "%s%u", i ? "," : "", cbuf[hp][i]);
                fprintf(stderr, "]}\n");
            }
            pool_n = kept;
        }

        vslots[hp].cands = cbuf[hp];
        vslots[hp].target_cost = tbuf[hp];
        vslots[hp].n_cands = pool_n;
        total_cands += pool_n;
    }
    if (synth_is_verbose())
        fprintf(stdout, "PRSL pools built: %u total candidates across %u hp slots "
                        "(%u slots had empty pools)\n",
                total_cands, n_hp, n_empty);

    /* PostScoringAdj (Word + Syl level): for each Word/Syl in the slot
     * tree, call spfy_anchor_score on the cklx postings keyed by the word
     * text, then inject the surviving anchor cands' UIDs into the
     * corresponding... */
    /* For Syl-level PSA, re-syllabify phonemes left-to-right using
     * max-onset principle: each vowel is a syl nucleus; consonants split
     * between previous-coda and next-onset (last consonant goes to onset if
     * more than one... */
    uint32_t *psa_syl_start = NULL;
    uint32_t  psa_n_syls    = 0;
    if (n_segs_arr > 2) {
        psa_syl_start = (uint32_t *)calloc(n_segs_arr,
                                           sizeof *psa_syl_start);
    }
    if (psa_syl_start && !spfy_env("SPFY_PSA_SYL_FROM_RESYL")) {
        /* 2026-05-14: derive syllable boundaries from the slot tree's
         * SK_SYLLABLE nodes (engine FE output). */
        for (uint32_t s = 0; s < tree.n_slots; ++s) {
            if (tree.slots[s].kind != SPFY_SK_SYLLABLE) continue;
            /* A fr-CA liaison syllable is one node here, so its leftmost
             * leaf is the true syllable onset and no boundary opens
             * mid-span. */
            uint32_t cur = s;
            while (tree.slots[cur].n_children > 0)
                cur = tree.slots[cur].child_idx[0];
            uint32_t hp_idx = post_to_hp[cur];
            if (hp_idx == UINT32_MAX) continue;
            uint32_t phon_idx = hp_idx / 2u;
            if (phon_idx == 0) continue;
            if (phon_idx >= n_segs_arr - 1) continue;
            if (psa_n_syls == 0
                || phon_idx > psa_syl_start[psa_n_syls - 1])
                psa_syl_start[psa_n_syls++] = phon_idx;
        }
    } else if (psa_syl_start) {
        int seen_vowel = !spfy_env("SPFY_NO_SYL_INITIAL_VOWEL")
                         && is_arpa_vowel(seg_names[1]);
        uint32_t last_v = 1;
        psa_syl_start[psa_n_syls++] = 1;
        for (uint32_t i = 2; i < n_segs_arr - 1; ++i) {
            int is_v = is_arpa_vowel(seg_names[i]);
            if (is_v && seen_vowel) {
                uint32_t n_cons = i - last_v - 1;
                uint32_t boundary = (n_cons >= 1) ? (i - 1) : i;
                if (boundary > psa_syl_start[psa_n_syls - 1])
                    psa_syl_start[psa_n_syls++] = boundary;
            }
            if (is_v) { seen_vowel = 1; last_v = i; }
        }
    }

    if (!spfy_env("SPFY_NO_PSA")) {
        uint32_t psa_words = 0, psa_syls = 0;
        /* Utterance-wide SP targets by ABSOLUTE half-phone. */
        spfy_anchor_sp_target_t *sp_all = (spfy_anchor_sp_target_t *)
            calloc(n_hp, sizeof *sp_all);
        if (sp_all) {
            for (uint32_t hp = 0; hp < n_hp; ++hp) {
                for (int i = 0; i < 5; ++i)
                    sp_all[hp].sp[i] = sp_tab.sp[hp_to_post[hp]][i];
            }
        }
        /* Utterance-wide durt forest index per absolute half-phone -- the
         * same `didx` the per-HP loop below computes. */
        uint8_t *phone_all = (uint8_t *)calloc(n_hp, sizeof *phone_all);
        if (phone_all) {
            for (uint32_t hp = 0; hp < n_hp; ++hp) {
                uint32_t p = hp_to_post[hp];
                phone_all[hp] =
                    (uint8_t)phone_to_labl(v, slice_ctx.ctx[p][2] >> 1);
            }
        }
        /* First: tree-Word iteration (existing path). */
        for (uint32_t s = 0; s < tree.n_slots; ++s) {
            spfy_slot_kind_t kind = tree.slots[s].kind;
            if (kind != SPFY_SK_WORD) continue;

            char lc_key[128];
            int   anchor_type = 4;
            uint32_t group_idx = SPFY_CHUNK_GROUP_WORD;
            int found = 0;
            uint32_t wi = 0;
            for (uint32_t w = 0; w < fe_utt.n_words; ++w) {
                if (fe_utt.word_shareds[w] == tree.slots[s].fe_shared) {
                    wi = w; found = 1; break;
                }
            }
            if (!found) continue;
            const char *wname = fe_utt.word_names[wi];
            if (!wname || strcmp(wname, "_NULL_") == 0) continue;
            size_t kl = strlen(wname);
            if (kl >= sizeof lc_key) kl = sizeof lc_key - 1;
            for (size_t i = 0; i < kl; ++i)
                lc_key[i] = (char)tolower((unsigned char)wname[i]);
            lc_key[kl] = 0;

            const uint32_t *postings = NULL;
            uint32_t n_postings = 0;
            int hit = spfy_cklx_lookup(&v->chunks.cklx[group_idx],
                                       lc_key, &postings, &n_postings);
            if (spfy_env("SPFY_PSA_DEBUG"))
                fprintf(stderr, "  PSA[%s] key=%-30s hit=%d n_postings=%u\n",
                        kind == SPFY_SK_WORD ? "WORD" : "SYL ",
                        lc_key, hit, n_postings);
            if (hit <= 0) continue;
            if (n_postings == 0) continue;

            int32_t first_hp_post = -1;
            uint32_t first_hp_idx = 0, last_hp_idx = 0;
            for (uint32_t hp = 0; hp < n_hp; ++hp) {
                uint32_t post = hp_to_post[hp];
                uint32_t cur = post;
                int matches = 0;
                while (cur != UINT32_MAX && cur < tree.n_slots) {
                    if (cur == s) { matches = 1; break; }
                    cur = tree.slots[cur].parent_idx;
                }
                if (!matches) continue;
                if (first_hp_post < 0) {
                    first_hp_post = (int32_t)post; first_hp_idx = hp;
                }
                last_hp_idx = hp;
            }
            if (first_hp_post < 0) continue;
            uint32_t span_n = last_hp_idx - first_hp_idx + 1;

            spfy_anchor_cart_t      *cart_per = (spfy_anchor_cart_t *)
                calloc(span_n, sizeof *cart_per);
            spfy_anchor_sp_target_t *sp_per   = (spfy_anchor_sp_target_t *)
                calloc(span_n, sizeof *sp_per);
            int32_t                 *syl_idx_per = (int32_t *)
                calloc(span_n, sizeof *syl_idx_per);
            spfy_anchor_hp_feat_t   *hp_feat_per = (spfy_anchor_hp_feat_t *)
                calloc(span_n, sizeof *hp_feat_per);
            if (!cart_per || !sp_per || !syl_idx_per || !hp_feat_per) {
                free(cart_per); free(sp_per); free(syl_idx_per);
                free(hp_feat_per); continue;
            }
            for (uint32_t k = 0; k < span_n; ++k) {
                uint32_t hp = first_hp_idx + k;
                uint32_t post = hp_to_post[hp];
                cart_feat_ctx_t cfc = {NULL, q5_per_slot[post], v, 0};
                spfy_fe_slot_t adapter = {0};
                for (int i = 0; i < 5; ++i) {
                    adapter.ctx[i] = (int32_t)slice_ctx.ctx[post][i];
                    adapter.sp[i]  = sp_tab.sp[post][i];
                }
                cfc.slot = &adapter;
                /* Plan 03-04: silence-pad CART traversal - see primary
                 * spfy_cart_traverse call site for rationale. */
                int silence = ctx_is_silence(v, slice_ctx.ctx[post][2]);
                if (!silence || !spfy_env("SPFY_NO_SILENCE_CART")) {
                    uint32_t didx = phone_to_labl(v, slice_ctx.ctx[post][2] >> 1);
                    if (didx < v->durt_cart.n_trees) {
                        if (spfy_cart_traverse(&v->durt_cart, didx, cart_feat, &cfc,
                              &cart_per[k].durt_mean, &cart_per[k].durt_var) == SPFY_OK)
                            cart_per[k].durt_valid = 1;
                        hp_feat_per[k].phone_label = (uint8_t)didx;
                        hp_feat_per[k].durt_valid  = 1;
                    }
                    if (v->f0tr_cart.n_trees > 0) {
                        cfc.is_f0tr = 1;
                        if (spfy_cart_traverse(&v->f0tr_cart, 0, cart_feat, &cfc,
                              &cart_per[k].f0tr_mean, &cart_per[k].f0tr_var) == SPFY_OK) {
                            cart_per[k].f0tr_valid = 1;
                            cart_per[k].f0tr_mean *= v->pitch_scale;
                        }
                        cfc.is_f0tr = 0;
                    }
                }
                for (int i = 0; i < 5; ++i) {
                    sp_per[k].sp[i] = sp_tab.sp[post][i];
                    hp_feat_per[k].ctx[i] = (int32_t)slice_ctx.ctx[post][i];
                    hp_feat_per[k].sp[i]  = sp_tab.sp[post][i];
                }
                hp_feat_per[k].q5 = q5_per_slot[post];
                /* 2026-05-14: real syl_idx per HP - walk parent_idx up the
                 * tree until we hit an SK_SYLLABLE node. The anchor-score
                 * advance walk uses this to step target_idx at syllable
                 * boundaries (engine FUN_08e89530 walks `param_2+0x18`
                 * forward until value changes from prior). Previously this
                 * was always -1, making advance jump to last_hp on first
                 * non-initial iter - wrong for multi-syllable Word spans. */
                {
                    int32_t syl_idx = -1;
                    uint32_t cur = post;
                    while (cur != UINT32_MAX && cur < tree.n_slots) {
                        if (tree.slots[cur].kind == SPFY_SK_SYLLABLE) {
                            syl_idx = (int32_t)cur;
                            break;
                        }
                        cur = tree.slots[cur].parent_idx;
                    }
                    syl_idx_per[k] = syl_idx;
                }
            }

            spfy_anchor_slot_input_t aslot = {0};
            aslot.first_hp = (int32_t)first_hp_idx;
            aslot.last_hp  = (int32_t)last_hp_idx;
            for (int i = 0; i < 5; ++i) {
                aslot.first_ctx.ctx[i] =
                    (int32_t)slice_ctx.ctx[hp_to_post[first_hp_idx]][i];
                aslot.last_ctx.ctx[i] =
                    (int32_t)slice_ctx.ctx[hp_to_post[last_hp_idx]][i];
            }
            aslot.anchor_type = anchor_type;
            aslot.cart_per_hp = cart_per;
            aslot.sp_per_hp   = sp_per;
            aslot.syl_idx_per_hp = syl_idx_per;
            aslot.sp_all_hp   = sp_all;
            aslot.n_all_hp    = (int32_t)n_hp;
            aslot.phone_all_hp = phone_all;
            aslot.durt_cart   = &v->durt_cart;
            aslot.hp_feat     = hp_feat_per;

            /* Sized to the posting count, not a fixed 64. The old cap cut the
             * candidate list in POSTING order before cost order, so it dropped
             * candidates cheaper than ones it kept -- on paulina, 132 anchors
             * were truncated, one losing 179 candidates cheaper than its
             * dearest survivor. The engine's own buffers in FUN_08e8ce60 are
             * afStack_13880[10000] / local_9c40[9999], and it never caps below
             * that; the surviving count cannot exceed n_postings. */
            uint32_t cand_cap = n_postings ? n_postings : 1u;
            spfy_anchor_cand_t *out_cands = (spfy_anchor_cand_t *)
                calloc(cand_cap, sizeof *out_cands);
            if (!out_cands) {
                free(cart_per); free(sp_per); free(syl_idx_per);
                free(hp_feat_per);
                continue;
            }
            uint32_t out_n = 0;
            int rcs = spfy_anchor_score(&v->av, &aslot, postings, n_postings,
                                         &v->chunks.ckls[group_idx],
                                         out_cands, cand_cap, &out_n);
            free(cart_per); free(sp_per); free(syl_idx_per); free(hp_feat_per);
            if (rcs != SPFY_OK || out_n == 0) { free(out_cands); continue; }
            ++psa_words;

            /* Store anchor cands per tree slot for DAG Viterbi. */
            uint32_t *cands_buf = (uint32_t *)calloc(out_n, sizeof *cands_buf);
            uint32_t *jks_buf   = (uint32_t *)calloc(out_n, sizeof *jks_buf);
            float    *tgt_buf   = (float    *)calloc(out_n, sizeof *tgt_buf);
            if (!cands_buf || !jks_buf || !tgt_buf) {
                free(cands_buf); free(jks_buf); free(tgt_buf);
                free(out_cands); continue;
            }
            uint32_t kept = 0;
            for (uint32_t c = 0; c < out_n; ++c) {
                uint32_t ss = out_cands[c].ss;
                uint32_t se = out_cands[c].se;
                if (se < ss) continue;
                if (se >= v->units.n_units) continue;
                /* 2026-05-14: removed `se-ss+1 != span_n` filter. */
                if (!spfy_env("SPFY_ANCHOR_RELAX_SPAN_OFF")
                    && spfy_env("SPFY_ANCHOR_STRICT_SPAN")
                    && se - ss + 1 != span_n) continue;
                cands_buf[kept] = ss;
                jks_buf  [kept] = se;
                tgt_buf  [kept] = out_cands[c].pre_dp;
                ++kept;
            }
            anchor_cands [s] = cands_buf;
            anchor_jks   [s] = jks_buf;
            anchor_target[s] = tgt_buf;
            anchor_n     [s] = kept;
            /* Engine reads anchor cand+0x68/+0x6c/+0x70 from the RUN-TAIL
             * unit (= se = jks_buf[c]). */
            uint8_t *aC68 = (uint8_t *)calloc(kept, sizeof *aC68);
            uint8_t *aC6c = (uint8_t *)calloc(kept, sizeof *aC6c);
            uint8_t *aC70 = (uint8_t *)calloc(kept, sizeof *aC70);
            uint8_t *aC78 = (uint8_t *)calloc(kept, sizeof *aC78);
            if (aC68 && aC6c && aC70 && aC78) {
                for (uint32_t c = 0; c < kept; ++c) {
                    /* Engine-faithful anchor cand state init from
                     * FUN_08e8ce60 @ 0x08e8ce60 (decomp 2026-05-14):
                     * iterate v->units in [ss..se]; per qualifying unit
                     * (voicing[hp_class] != 0 - Tom has weight_8c =
                     * weight_90 = 0 so only voicing gates), update
                     *   c70 = MAX of unit.f0_start
                     *   c6c = FIRST unit.f0_end where f0_end >= 21
                     *   c68 = LAST unit.f0_mid where f0_mid >= 21
                     * Silence (voicing[hp_class]==0) v->units skip all
                     * three. Engine init is c68=c6c=c70=0; values
                     * only get set if at least one qualifying unit
                     * exists.
                     * SPFY_NO_ANCHOR_HEAD_C6C=1 reverts to legacy
                     * tail-based init. */
                    int legacy = spfy_env("SPFY_NO_ANCHOR_HEAD_C6C") != NULL;
                    if (legacy) {
                        spfy_unit_record_t ur;
                        if (spfy_unit_record_get(&v->units, jks_buf[c], &ur)
                            == SPFY_OK) {
                            aC6c[c] = ur.f0_end;
                            aC68[c] = ur.f0_mid;
                            aC70[c] = ur.f0_start;
                        }
                        aC78[c] = 0;
                        continue;
                    }
                    uint8_t v68 = 0, v6c = 0, v70 = 0;
                    uint16_t v78 = 0;
                    /* Engine FUN_08e8ce60 anchor cand init. The gate
                     * `voicing[hp_class]==0 AND weight_8c==0 AND
                     * weight_90==0 -> silence path` empirically never
                     * fires for Tom (engine c68=120 for uid=74341 which
                     * has hp_class voiceless), so weight_8c or
                     * weight_90 must be non-zero for the init code path.
                     * Treat all v->units as voiced. SPFY_ANCHOR_VOICING_GATE=1
                     * re-enables the gate. */
                    int do_gate = spfy_env("SPFY_ANCHOR_VOICING_GATE") != NULL;
                    for (uint32_t u = cands_buf[c]; u <= jks_buf[c]; ++u) {
                        spfy_unit_record_t ur;
                        if (spfy_unit_record_get(&v->units, u, &ur) != SPFY_OK)
                            continue;
                        if (do_gate) {
                            uint8_t uhpc = (v->av.hpclass_table
                                            && u < v->av.hpclass_n)
                                           ? v->av.hpclass_table[u] : 0xff;
                            int voiced = (v->av.voicing != NULL
                                          && uhpc < v->av.voicing_n
                                          && v->av.voicing[uhpc] != 0);
                            if (!voiced) { v78 += ur.dur_like; continue; }
                        }
                        if (v70 < ur.f0_start) v70 = ur.f0_start;
                        if (ur.f0_end >= 21 && v6c < 21)
                            v6c = ur.f0_end;
                        if (ur.f0_mid >= 21) { v68 = ur.f0_mid; v78 = 0; }
                        else                 { v78 += ur.dur_like; }
                    }
                    aC68[c] = v68;
                    aC6c[c] = v6c;
                    aC70[c] = v70;
                    /* c78 is a 16-bit accumulator engine-side; our slot
                     * struct holds it as a uint8_t. */
                    aC78[c] = (v78 > 255u) ? 255u : (uint8_t)v78;
                    if (spfy_env("SPFY_ANCHOR_STATE_DUMP")) {
                        fprintf(stderr, "{\"anchor_state\":1,\"slot\":%u,"
                                "\"key\":\"%s\",\"ss\":%u,\"se\":%u,"
                                "\"c68\":%u,\"c6c\":%u,\"c70\":%u,"
                                "\"c78\":%u}\n",
                                s, lc_key, cands_buf[c], jks_buf[c],
                                v68, v6c, v70, aC78[c]);
                    }
                }
                anchor_c68[s] = aC68;
                anchor_c6c[s] = aC6c;
                anchor_c70[s] = aC70;
                anchor_c78[s] = aC78;
            } else {
                free(aC68); free(aC6c); free(aC70); free(aC78);
            }
            if (spfy_env("SPFY_PSA_DEBUG")) {
                fprintf(stderr, "  WORD anchor slot=%u key=%s n=%u "
                                "first_hp=%u last_hp=%u\n",
                        s, lc_key, kept, first_hp_idx, last_hp_idx);
                for (uint32_t c = 0; c < kept; ++c)
                    fprintf(stderr, "    cand[%u] ss=%u se=%u pre_dp=%.6f\n",
                            c, cands_buf[c], jks_buf[c], (double)tgt_buf[c]);
            }
            free(out_cands);
        }

        /* Second pass: psa-derived syllables. */
        for (uint32_t g = 0; g < psa_n_syls; ++g) {
            uint32_t start_p = psa_syl_start[g];
            uint32_t end_p = (g + 1 < psa_n_syls)
                              ? (psa_syl_start[g + 1] - 1)
                              : (n_segs_arr >= 2 ? n_segs_arr - 2 : 0);
            if (end_p < start_p) continue;
            if (end_p >= n_segs_arr) continue;

            /* 2026-05-14 INVESTIGATED: SYL CKLX contains onset+first-vowel
             * keys for many syllables ("d_ao" hits, "d_ao_g" misses). */
            char lc_key[128];
            size_t pos = 0;
            lc_key[0] = 0;
            int abort_key = 0;
            for (uint32_t i = start_p; i <= end_p; ++i) {
                const char *aname = seg_names[i];
                if (!aname || strcmp(aname, "pau") == 0) {
                    abort_key = 1; break;
                }
                size_t al = strlen(aname);
                if (pos + al + 2 >= sizeof lc_key) { abort_key = 1; break; }
                if (pos > 0) lc_key[pos++] = '_';
                memcpy(lc_key + pos, aname, al); pos += al;
                lc_key[pos] = 0;
            }
            if (abort_key || pos == 0) continue;

            const uint32_t *postings = NULL;
            uint32_t n_postings = 0;
            int hit = spfy_cklx_lookup(&v->chunks.cklx[SPFY_CHUNK_GROUP_SYL],
                                       lc_key, &postings, &n_postings);
            /* first_hp/last_hp are printed so this line can be JOINED
             * against the engine's anchor_components frames, which report
             * the same span. */
            if (spfy_env("SPFY_PSA_DEBUG"))
                fprintf(stderr, "  PSA[SYL ] key=%-30s hit=%d n_postings=%u "
                                "first_hp=%u last_hp=%u\n",
                        lc_key, hit, n_postings,
                        2u * start_p, 2u * end_p + 1u);
            if (hit <= 0 || n_postings == 0) continue;

            /* HP span: phoneme i (in seg_names) -> HP slots 2*i .. */
            uint32_t first_hp_idx = 2u * start_p;
            uint32_t last_hp_idx  = 2u * end_p + 1u;
            if (last_hp_idx >= n_hp) continue;
            uint32_t span_n = last_hp_idx - first_hp_idx + 1u;

            spfy_anchor_cart_t      *cart_per = (spfy_anchor_cart_t *)
                calloc(span_n, sizeof *cart_per);
            spfy_anchor_sp_target_t *sp_per   = (spfy_anchor_sp_target_t *)
                calloc(span_n, sizeof *sp_per);
            int32_t                 *syl_idx_per = (int32_t *)
                calloc(span_n, sizeof *syl_idx_per);
            spfy_anchor_hp_feat_t   *hp_feat_per = (spfy_anchor_hp_feat_t *)
                calloc(span_n, sizeof *hp_feat_per);
            if (!cart_per || !sp_per || !syl_idx_per || !hp_feat_per) {
                free(cart_per); free(sp_per); free(syl_idx_per);
                free(hp_feat_per); continue;
            }
            for (uint32_t k = 0; k < span_n; ++k) {
                uint32_t hp = first_hp_idx + k;
                uint32_t post = hp_to_post[hp];
                cart_feat_ctx_t cfc = {NULL, q5_per_slot[post], v, 0};
                spfy_fe_slot_t adapter = {0};
                for (int i = 0; i < 5; ++i) {
                    adapter.ctx[i] = (int32_t)slice_ctx.ctx[post][i];
                    adapter.sp[i]  = sp_tab.sp[post][i];
                }
                cfc.slot = &adapter;
                /* Plan 03-04: silence-pad CART traversal - see primary
                 * spfy_cart_traverse call site for rationale. */
                int silence = ctx_is_silence(v, slice_ctx.ctx[post][2]);
                if (!silence || !spfy_env("SPFY_NO_SILENCE_CART")) {
                    uint32_t didx = phone_to_labl(v, slice_ctx.ctx[post][2] >> 1);
                    if (didx < v->durt_cart.n_trees) {
                        if (spfy_cart_traverse(&v->durt_cart, didx, cart_feat, &cfc,
                              &cart_per[k].durt_mean, &cart_per[k].durt_var) == SPFY_OK)
                            cart_per[k].durt_valid = 1;
                        hp_feat_per[k].phone_label = (uint8_t)didx;
                        hp_feat_per[k].durt_valid  = 1;
                    }
                    if (v->f0tr_cart.n_trees > 0) {
                        cfc.is_f0tr = 1;
                        if (spfy_cart_traverse(&v->f0tr_cart, 0, cart_feat, &cfc,
                              &cart_per[k].f0tr_mean, &cart_per[k].f0tr_var) == SPFY_OK) {
                            cart_per[k].f0tr_valid = 1;
                            cart_per[k].f0tr_mean *= v->pitch_scale;
                        }
                        cfc.is_f0tr = 0;
                    }
                }
                for (int i = 0; i < 5; ++i) {
                    sp_per[k].sp[i] = sp_tab.sp[post][i];
                    hp_feat_per[k].ctx[i] = (int32_t)slice_ctx.ctx[post][i];
                    hp_feat_per[k].sp[i]  = sp_tab.sp[post][i];
                }
                hp_feat_per[k].q5 = q5_per_slot[post];
                /* 2026-05-14: real syl_idx per HP - walk parent_idx up the
                 * tree until we hit an SK_SYLLABLE node. The anchor-score
                 * advance walk uses this to step target_idx at syllable
                 * boundaries (engine FUN_08e89530 walks `param_2+0x18`
                 * forward until value changes from prior). Previously this
                 * was always -1, making advance jump to last_hp on first
                 * non-initial iter - wrong for multi-syllable Word spans. */
                {
                    int32_t syl_idx = -1;
                    uint32_t cur = post;
                    while (cur != UINT32_MAX && cur < tree.n_slots) {
                        if (tree.slots[cur].kind == SPFY_SK_SYLLABLE) {
                            syl_idx = (int32_t)cur;
                            break;
                        }
                        cur = tree.slots[cur].parent_idx;
                    }
                    syl_idx_per[k] = syl_idx;
                }
            }

            spfy_anchor_slot_input_t aslot = {0};
            aslot.first_hp = (int32_t)first_hp_idx;
            aslot.last_hp  = (int32_t)last_hp_idx;
            for (int i = 0; i < 5; ++i) {
                aslot.first_ctx.ctx[i] =
                    (int32_t)slice_ctx.ctx[hp_to_post[first_hp_idx]][i];
                aslot.last_ctx.ctx[i] =
                    (int32_t)slice_ctx.ctx[hp_to_post[last_hp_idx]][i];
            }
            aslot.anchor_type = 2;
            aslot.cart_per_hp = cart_per;
            aslot.sp_per_hp   = sp_per;
            aslot.syl_idx_per_hp = syl_idx_per;
            aslot.sp_all_hp   = sp_all;
            aslot.n_all_hp    = (int32_t)n_hp;
            aslot.phone_all_hp = phone_all;
            aslot.durt_cart   = &v->durt_cart;
            aslot.hp_feat     = hp_feat_per;

            uint32_t cand_cap = n_postings ? n_postings : 1u;
            spfy_anchor_cand_t *out_cands = (spfy_anchor_cand_t *)
                calloc(cand_cap, sizeof *out_cands);
            if (!out_cands) {
                free(cart_per); free(sp_per); free(syl_idx_per);
                free(hp_feat_per);
                continue;
            }
            uint32_t out_n = 0;
            int rcs = spfy_anchor_score(&v->av, &aslot, postings, n_postings,
                                         &v->chunks.ckls[SPFY_CHUNK_GROUP_SYL],
                                         out_cands, cand_cap, &out_n);
            free(cart_per); free(sp_per); free(syl_idx_per); free(hp_feat_per);
            if (rcs != SPFY_OK) { free(out_cands); continue; }
            ++psa_syls;

            /* Store Syl anchor cands keyed by the SLOT POST_IDX of the
             * matching Syl in the tree (engine attaches anchor cands to the
             * Syl tree-slot, and LinkGraph routes the DAG through them). */
            int32_t syl_slot_post = -1;
            for (uint32_t s2 = 0; s2 < tree.n_slots; ++s2) {
                if (tree.slots[s2].kind != SPFY_SK_SYLLABLE) continue;
                uint32_t cur = s2;
                while (tree.slots[cur].n_children > 0)
                    cur = tree.slots[cur].child_idx[0];
                uint32_t hp_lookup = post_to_hp[cur];
                if (hp_lookup == first_hp_idx) {
                    syl_slot_post = (int32_t)s2; break;
                }
            }

            uint32_t kept_s = 0;
            uint32_t *cands_s = NULL, *jks_s = NULL;
            float    *tgt_s   = NULL;
            if (syl_slot_post >= 0) {
                cands_s = (uint32_t *)calloc(out_n, sizeof *cands_s);
                jks_s   = (uint32_t *)calloc(out_n, sizeof *jks_s);
                tgt_s   = (float    *)calloc(out_n, sizeof *tgt_s);
                if (!cands_s || !jks_s || !tgt_s) {
                    free(cands_s); free(jks_s); free(tgt_s);
                    cands_s = jks_s = NULL; tgt_s = NULL;
                }
            }

            for (uint32_t c = 0; c < out_n; ++c) {
                uint32_t ss = out_cands[c].ss;
                uint32_t se = out_cands[c].se;
                if (se < ss) continue;
                uint32_t span_uid_n = se - ss + 1;
                /* 2026-05-14: removed strict span filter - see WORD-pass
                 * comment for rationale. */
                if (spfy_env("SPFY_ANCHOR_STRICT_SPAN")
                    && span_uid_n != span_n) continue;
                if (cands_s && jks_s && tgt_s) {
                    cands_s[kept_s] = ss;
                    jks_s  [kept_s] = se;
                    tgt_s  [kept_s] = out_cands[c].pre_dp;
                    ++kept_s;
                }
            }
            /* The SYL pass had no visibility at all: the WORD pass prints
             * its kept count, this one printed nothing, so "felix builds
             * zero anchors" could not be attributed to the lookup, the
             * scorer, or the tree-slot resolve. */
            if (spfy_env("SPFY_PSA_DEBUG")) {
                fprintf(stderr, "  SYL  anchor key=%-22s out_n=%u kept=%u "
                                "syl_slot=%d first_hp=%u last_hp=%u\n",
                        lc_key, out_n, kept_s, syl_slot_post,
                        first_hp_idx, last_hp_idx);
                /* Print the candidates the same way the WORD branch does. */
                for (uint32_t c = 0; c < kept_s; ++c)
                    fprintf(stderr, "    cand[%u] ss=%u se=%u pre_dp=%.6f\n",
                            c, cands_s[c], jks_s[c], (double)tgt_s[c]);
            }
            if (syl_slot_post >= 0 && kept_s > 0
                && cands_s && jks_s && tgt_s) {
                free(anchor_cands [syl_slot_post]);
                free(anchor_jks   [syl_slot_post]);
                free(anchor_target[syl_slot_post]);
                free(anchor_c68   [syl_slot_post]);
                free(anchor_c6c   [syl_slot_post]);
                free(anchor_c70   [syl_slot_post]);
                free(anchor_c78   [syl_slot_post]);
                anchor_cands [syl_slot_post] = cands_s;
                anchor_jks   [syl_slot_post] = jks_s;
                anchor_target[syl_slot_post] = tgt_s;
                anchor_n     [syl_slot_post] = kept_s;
                /* Engine-faithful c68/c6c/c70 aggregation. See WORD
                 * anchor block above for the FUN_08e8ce60 derivation.
                 * SPFY_NO_ANCHOR_HEAD_C6C=1 reverts to legacy tail. */
                uint8_t *aC68 = (uint8_t *)calloc(kept_s, sizeof *aC68);
                uint8_t *aC6c = (uint8_t *)calloc(kept_s, sizeof *aC6c);
                uint8_t *aC70 = (uint8_t *)calloc(kept_s, sizeof *aC70);
                uint8_t *aC78 = (uint8_t *)calloc(kept_s, sizeof *aC78);
                if (aC68 && aC6c && aC70 && aC78) {
                    int legacy = spfy_env("SPFY_NO_ANCHOR_HEAD_C6C") != NULL;
                    for (uint32_t c = 0; c < kept_s; ++c) {
                        if (legacy) {
                            spfy_unit_record_t ur;
                            if (spfy_unit_record_get(&v->units, jks_s[c], &ur)
                                == SPFY_OK) {
                                aC6c[c] = ur.f0_end;
                                aC68[c] = ur.f0_mid;
                                aC70[c] = ur.f0_start;
                            }
                            aC78[c] = 0;
                            continue;
                        }
                        uint8_t v68 = 0, v6c = 0, v70 = 0;
                        uint16_t v78 = 0;
                        int do_gate = spfy_env("SPFY_ANCHOR_VOICING_GATE")
                                      != NULL;
                        for (uint32_t u = cands_s[c]; u <= jks_s[c]; ++u) {
                            spfy_unit_record_t ur;
                            if (spfy_unit_record_get(&v->units, u, &ur)
                                != SPFY_OK) continue;
                            if (do_gate) {
                                uint8_t uhpc = (v->av.hpclass_table
                                                && u < v->av.hpclass_n)
                                               ? v->av.hpclass_table[u] : 0xff;
                                int voiced = (v->av.voicing != NULL
                                              && uhpc < v->av.voicing_n
                                              && v->av.voicing[uhpc] != 0);
                                if (!voiced) {
                                    v78 += ur.dur_like; continue;
                                }
                            }
                            if (v70 < ur.f0_start) v70 = ur.f0_start;
                            if (ur.f0_end >= 21 && v6c < 21)
                                v6c = ur.f0_end;
                            if (ur.f0_mid >= 21) {
                                v68 = ur.f0_mid; v78 = 0;
                            } else {
                                v78 += ur.dur_like;
                            }
                        }
                        aC68[c] = v68;
                        aC6c[c] = v6c;
                        aC70[c] = v70;
                        aC78[c] = (v78 > 255u) ? 255u : (uint8_t)v78;
                    }
                    anchor_c68[syl_slot_post] = aC68;
                    anchor_c6c[syl_slot_post] = aC6c;
                    anchor_c70[syl_slot_post] = aC70;
                    anchor_c78[syl_slot_post] = aC78;
                } else {
                    free(aC68); free(aC6c); free(aC70); free(aC78);
                }
            } else {
                free(cands_s); free(jks_s); free(tgt_s);
            }
            free(out_cands);
        }

        if (synth_is_verbose())
            fprintf(stdout, "PostScoringAdj: %u words + %u syls processed\n",
                    psa_words, psa_syls);
        free(sp_all);
        free(phone_all);
    }
    free(seg_names);
    free(psa_syl_start);

    /* Engine-faithful DAG Viterbi (FUN_08e8b620 semantics): every tree
     * slot is a node, predecessors come from spfy_link_graph, Word/Syl
     * slots carry anchor cands and HP-leaf slots carry PRSL pool cands.
     * The DP picks the cheapest route, which lets a low-cost Word
     * anchor BYPASS its HP children. */
    join_ctx_t jc;
    jc.hash = &v->hash; jc.units = &v->units;
    /* miss_default is only used when the hist curve fails to load
     * (degenerate fallback). */
    jc.miss_default = 0.0f;
    /* F0-prob curve (VIN hist chunk + voice+0xc8). */
    load_f0_hist_curve(&v->vin, &jc);
    jc.f0_edge_change_weight =
        spfy_vcf_f32(&v->vcf, "F0_EDGE_CHANGE_WEIGHT", 0.6f);
    /* MISSING_JOIN_COST = 1000.0 from FE-init default (FUN_08e90dc0
     * sets param_3[0x21] = 0x447a0000 = 1000.0 before VCF override;
     * no shipped VCF overrides it). Huge by design - makes the DP almost
     * exclusively use same-rec runs and v->hash hits. */
    jc.missing_join_cost     =
        spfy_vcf_f32(&v->vcf, "MISSING_JOIN_COST", 1000.0f);
    /* Hash-hit join weighting, off by default (see the note at dag_join_cb). */
    /* SPFY_POW_CONT_W energy-continuity term. */
    jc.pow           = unit_pow;
    jc.pow_n         = unit_pow_n;
    jc.pow_cont_w    = pow_join_w;
    jc.pow_cont_dead = pow_join_dead;
    jc.apply_join_w = (spfy_env("SPFY_JOIN_W_APPLY") != NULL);
    /* Fallbacks are the ENGINE's built-in defaults, read out of
     * SWIttsUSel.dll FUN_08e90dc0: the constructor stores 0x3e800000 (0.25)
     * at +0x2c and 0x3e4ccccd (0.2) at +0x30, and a failed config lookup
     * (`test eax,eax` / `jne` past the store) leaves them untouched. The
     * weight fallback used to be 0.7f, which is TOM's VCF value, not a
     * default -- latent, since all eight shipped voices define the param,
     * but it would silently mis-weight any voice that omitted it. */
    jc.join_w   = spfy_vcf_f32(&v->vcf, "JOIN_COST_WEIGHT", 0.25f);
    jc.join_off = spfy_vcf_f32(&v->vcf, "JOIN_COST_OFFSET", 0.2f);
    jc.f0           = dp_f0_on ? dp_f0.f0 : NULL;
    jc.f0_n         = dp_f0_on ? dp_f0.n_units : 0u;
    jc.f0_cont_w    = dp_f0_w;
    jc.f0_cont_dead  = dp_f0_dead;
    jc.f0_cont_up    = dp_f0_up;
    jc.f0_cont_break = dp_f0_break;
    jc.f0_cont_slope = dp_f0_slope;
    jc.f0_cont_scope = dp_f0_scope;
    jc.f0_cont_seam  = dp_f0_seam;
    {
        const char *e = spfy_env("SPFY_JOIN_W");
        if (e && *e) jc.join_w = (float)atof(e);
        e = spfy_env("SPFY_JOIN_OFF");
        if (e && *e) jc.join_off = (float)atof(e);
        /* SPFY_MISSING_JOIN sweeps MISSING_JOIN_COST, which no shipped VCF
         * carries (so vcf_variant.py cannot reach it -- adding unknown
         * names makes the server exit rc=5). */
        e = spfy_env("SPFY_MISSING_JOIN");
        if (e && *e) jc.missing_join_cost = (float)atof(e);
    }
    if (jc.apply_join_w && synth_is_verbose())
        fprintf(stdout, "join weighting ON: cost = %.3f*cell + %.3f\n",
                (double)jc.join_w, (double)jc.join_off);
    if (spfy_env("SPFY_NO_F0_CURVE")) jc.curve = NULL;
    if (jc.curve) {
        if (synth_is_verbose())
            fprintf(stdout, "F0-curve loaded: %d bins, sub_off=%d, "
                            "F0_EDGE=%.2f, MISSING_JOIN=%.2f\n",
                    jc.curve_max_idx, jc.curve_sub_off,
                    (double)jc.f0_edge_change_weight,
                    (double)jc.missing_join_cost);
        if (spfy_env("SPFY_DUMP_F0_CURVE")) {
            fprintf(stderr, "{\"f0_curve\":1,\"n\":%d,\"sub_off\":%d,\"vals\":[",
                    jc.curve_max_idx, jc.curve_sub_off);
            for (int i = 0; i < jc.curve_max_idx; ++i)
                fprintf(stderr, "%s%.4f", i ? "," : "",
                        (double)spfy_le_f32(jc.curve + (size_t)i * 4u));
            fprintf(stderr, "]}\n");
        }
    }
    n_slots = n_hp;
    /* ONE allocation, TWO arrays. */
    uint32_t *path_uids = (uint32_t *)calloc((size_t)n_slots * 2u,
                                             sizeof *path_uids);
    if (!path_uids) { rc = SPFY_E_NOMEM; goto fail; }
    uint32_t *path_extra = path_uids + n_slots;
    /* Default-fill with silence sentinel so any HP slot not covered by the
     * path (shouldn't happen in a correct DAG) plays as silence. */
    for (uint32_t i = 0; i < n_slots; ++i) path_uids[i] = SILENCE_UID(v);
    float total = 0.0f;

    {
        /* Build dag_slots[tree.n_slots]: HP slots get the PRSL pools (cands
         * == join_keys for leaves), Word/Syl slots get anchor cands (cands
         * = ss array, join_keys = se array), all slots get preds from
         * spfy_link_graph. */
        dag_slots = (spfy_viterbi_dag_slot_t *)
                    calloc(tree.n_slots, sizeof *dag_slots);
        if (!dag_slots) { rc = SPFY_E_NOMEM; free(path_uids); goto fail; }
        for (uint32_t s = 0; s < tree.n_slots; ++s) {
            dag_slots[s].preds   = preds_tab.per_slot[s].preds;
            dag_slots[s].n_preds = preds_tab.per_slot[s].n_preds;
        }
        /* HP-leaf slots: copy from vslots[]. */
        for (uint32_t hp = 0; hp < n_hp; ++hp) {
            uint32_t s = hp_to_post[hp];
            dag_slots[s].cands       = vslots[hp].cands;
            dag_slots[s].join_keys   = vslots[hp].cands;
            dag_slots[s].target_cost = vslots[hp].target_cost;
            dag_slots[s].n_cands     = vslots[hp].n_cands;
            dag_slots[s].c68         = cand_c68[hp];
            dag_slots[s].c6c         = cand_c6c[hp];
            dag_slots[s].c70         = cand_c70[hp];
            dag_slots[s].c78         = cand_c78[hp];
        }
        for (uint32_t s = 0; s < tree.n_slots; ++s) {
            spfy_slot_kind_t k = tree.slots[s].kind;
            if (k != SPFY_SK_WORD && k != SPFY_SK_SYLLABLE) continue;
            if (anchor_n[s] == 0 || !anchor_cands[s]) continue;
            dag_slots[s].cands       = anchor_cands [s];
            dag_slots[s].join_keys   = anchor_jks   [s];
            dag_slots[s].target_cost = anchor_target[s];
            dag_slots[s].n_cands     = anchor_n     [s];
            dag_slots[s].c68         = anchor_c68   [s];
            dag_slots[s].c6c         = anchor_c6c   [s];
            dag_slots[s].c70         = anchor_c70   [s];
            dag_slots[s].c78         = anchor_c78   [s];
        }

        uint32_t *path_slots_buf = (uint32_t *)
                                    calloc(tree.n_slots, sizeof *path_slots_buf);
        uint32_t *path_uids_buf  = (uint32_t *)
                                    calloc(tree.n_slots, sizeof *path_uids_buf);
        if (!path_slots_buf || !path_uids_buf) {
            free(path_slots_buf); free(path_uids_buf);
            rc = SPFY_E_NOMEM; free(path_uids); goto fail;
        }
        uint32_t path_len = 0;
        /* cfg+0x94, per voice. */
        int path_f0_flag =
            (spfy_vcf_f32(&v->vcf, "GET_RID_OF_PATH_F0", 0.0f) != 0.0f);
        int rc_v = spfy_viterbi_run_dag(dag_slots, tree.n_slots,
                                         dag_join_cb, &jc,
                                         path_slots_buf, path_uids_buf,
                                         &path_len, &total, path_f0_flag);
        if (rc_v != SPFY_OK) {
            fprintf(stderr, "viterbi-dag failed: %s\n", spfy_strerror(rc_v));
            free(path_slots_buf); free(path_uids_buf);
            rc = rc_v; free(path_uids); goto fail;
        }
        if (synth_is_verbose())
            fprintf(stdout, "viterbi[dag] total cost=%.3f path_len=%u "
                            "(of %u tree slots; %u HP)\n",
                    (double)total, path_len, tree.n_slots, n_hp);

        /* [live-trace] DP done: total path cost + lengths. */
        spfy_trace_eventf("viterbi",
            "{\"phrase\":%u,\"total\":%.4f,\"path_len\":%u,\"n_slots\":%u}",
            phrase_idx, (double)total, path_len, tree.n_slots);

        /* Expand multi-UID anchor cands into per-HP UIDs. */
        uint32_t expand_anchors = 0, expand_hps = 0;
        for (uint32_t i = 0; i < path_len; ++i) {
            uint32_t s   = path_slots_buf[i];
            uint32_t uid = path_uids_buf[i];
            if (s >= tree.n_slots) continue;
            spfy_slot_kind_t k = tree.slots[s].kind;
            if (k == SPFY_SK_HALFPHONE) {
                uint32_t hp = post_to_hp[s];
                if (hp < n_slots) path_uids[hp] = uid;
                /* [live-trace] the Viterbi's winning candidate for this
                 * half-phone - the viz uses this to collapse the candidate
                 * cloud onto the chosen unit. */
                spfy_trace_eventf("pick", "{\"slot\":%u,\"uid\":%u}", hp, uid);
                ++expand_hps;
                continue;
            }
            if (k != SPFY_SK_WORD && k != SPFY_SK_SYLLABLE) continue;

            /* Find this slot's HP span: leftmost-leaf .. */
            uint32_t leftmost = s;
            while (tree.slots[leftmost].n_children > 0)
                leftmost = tree.slots[leftmost].child_idx[0];
            uint32_t rightmost = s;
            while (tree.slots[rightmost].n_children > 0) {
                uint32_t nch = tree.slots[rightmost].n_children;
                rightmost = tree.slots[rightmost].child_idx[nch - 1];
            }
            uint32_t first_hp = post_to_hp[leftmost];
            uint32_t last_hp  = post_to_hp[rightmost];
            if (first_hp >= n_slots || last_hp >= n_slots) continue;

            uint32_t se = uid;
            for (uint32_t c = 0; c < anchor_n[s]; ++c) {
                if (anchor_cands[s][c] == uid) {
                    se = anchor_jks[s][c]; break;
                }
            }
            uint32_t ss = uid;
            uint32_t span_n = (se >= ss) ? (se - ss + 1u) : 1u;
            uint32_t hp_span_n = last_hp - first_hp + 1u;
            uint32_t fill_n = (span_n < hp_span_n) ? span_n : hp_span_n;
            for (uint32_t k2 = 0; k2 < fill_n; ++k2) {
                if (first_hp + k2 < n_slots)
                    path_uids[first_hp + k2] = ss + k2;
            }
            /* For partial anchors (uid range < hp span), mark the
             * "overshoot" HPs (consumed by the anchor but with no
             * corresponding UID) with UINT32_MAX so the path dump and audio
             * decode can skip them. */
            for (uint32_t k2 = fill_n; k2 < hp_span_n; ++k2) {
                if (first_hp + k2 < n_slots)
                    path_uids[first_hp + k2] = 0xFFFFFFFFu;
            }
            /* ⚠ THE OTHER DIRECTION IS ALSO REAL: span_n > hp_span_n.
             *
             * `fill_n` is a min, and path_uids is indexed by half-phone, so an
             * anchor holding MORE units than half-phones silently lost its
             * tail -- the units simply had nowhere to live. The engine emits
             * the whole ss..se range regardless of how many half-phones it
             * spans (the same asymmetry the anchor durt walk hits, where it
             * clamps its own index at last_hp rather than dropping the tail).
             *
             * paulina es_014: the engine's wsola_in run head 599500 carries
             * lp=14 over a 12-half-phone anchor, and the two dropped units are
             * exactly the 62-vs-60 emitted-unit gap the gate reports.
             *
             * Record the remainder on the anchor's LAST half-phone; the concat
             * loop appends it to that run. SPFY_ANCHOR_NO_TAIL=1 reverts. */
            static int no_tail = -1;
            if (no_tail < 0)
                no_tail = (spfy_env("SPFY_ANCHOR_NO_TAIL") != NULL);
            if (span_n > hp_span_n && !no_tail
                && first_hp + hp_span_n - 1u < n_slots)
                path_extra[first_hp + hp_span_n - 1u] = span_n - hp_span_n;
            ++expand_anchors;
        }
        /* --- SPFY_DP_F0_CONT diagnostic
         * ------------------------------------ What the term is SUPPOSED to
         * minimise, measured on the path it actually chose: total |dF0| in
         * semitones over consecutive voiced slots, plus how many... */
        if (dp_f0_on) {
            double sum_st = 0.0, max_st = 0.0;
            uint32_t n_pair = 0, n_break = 0;
            uint32_t prev = 0xFFFFFFFFu;
            for (uint32_t k = 0; k < n_slots; ++k) {
                uint32_t u = path_uids[k];
                float f = (u < dp_f0.n_units) ? dp_f0.f0[u] : 0.0f;
                if (f <= 0.0f) { prev = 0xFFFFFFFFu; ++n_break; continue; }
                if (prev != 0xFFFFFFFFu) {
                    double st = fabs(12.0 * log2((double)f
                                                 / (double)dp_f0.f0[prev]));
                    sum_st += st;
                    if (st > max_st) max_st = st;
                    ++n_pair;
                }
                prev = u;
            }
            spfy_log_warn("dp-f0: path[%u] total |dF0| = %.1f st over %u "
                          "voiced pair(s) (mean %.2f, max %.2f), %u unvoiced "
                          "slot(s)",
                          phrase_idx, sum_st, n_pair,
                          n_pair ? sum_st / n_pair : 0.0, max_st, n_break);
        }
        if (spfy_env("SPFY_DUMP_PATH")) {
            fprintf(stdout, "  dag path: %u HP-slot hops, %u anchor hops "
                            "(span expanded into %u HP slots)\n",
                    expand_hps, expand_anchors, n_hp);
            fprintf(stdout, "  raw dag path (slot -> uid):\n");
            for (uint32_t i = 0; i < path_len; ++i) {
                uint32_t s = path_slots_buf[i];
                spfy_slot_kind_t k = (s < tree.n_slots)
                                     ? tree.slots[s].kind : SPFY_SK_PHRASE;
                const char *ks = (k == SPFY_SK_HALFPHONE) ? "HP"
                                : (k == SPFY_SK_SYLLABLE) ? "SYL"
                                : (k == SPFY_SK_WORD)     ? "WORD"
                                : "?";
                fprintf(stdout, "    [%u] slot=%u kind=%s uid=%u "
                                "n_preds=%u n_cands=%u\n",
                        i, s, ks, path_uids_buf[i],
                        dag_slots[s].n_preds, dag_slots[s].n_cands);
            }
            for (uint32_t hp = 0; hp < n_hp; ++hp) {
                /* Skip partial-anchor overshoot positions - engine's path
                 * doesn't emit these either. */
                if (path_uids[hp] == 0xFFFFFFFFu) continue;
                fprintf(stdout, "  hp %2u: uid=%u\n", hp, path_uids[hp]);
                /* An anchor holding more units than half-phones continues
                 * past its last slot; those units ARE emitted, so the dump
                 * -- and the gate's EMITTED-UNIT audit, which reads it --
                 * must list them or it disagrees with the audio... */
                for (uint32_t e = 1; e <= path_extra[hp]; ++e)
                    fprintf(stdout, "  hp %2u: uid=%u\n", hp,
                            path_uids[hp] + e);
            }
        }
        free(path_slots_buf); free(path_uids_buf);
    }

    /* SPFY_PATH_DUMP - the chosen UID per half-phone slot, one JSON line
     * per phrase. */
    if (spfy_env("SPFY_PATH_DUMP")) {
        fprintf(stderr, "{\"path_dump\":1,\"phrase\":%u,\"uids\":[",
                phrase_idx);
        for (uint32_t hp = 0; hp < n_slots; ++hp)
            fprintf(stderr, "%s%d", hp ? "," : "",
                    path_uids[hp] == 0xFFFFFFFFu ? -1 : (int)path_uids[hp]);
        fprintf(stderr, "]}\n");
    }

    /* SPFY_UID_DUMP, pick half - the DP's winner per half-phone. */
    if (spfy_uid_dump_fp()) {
        FILE *uf = spfy_uid_dump_fp();
        for (uint32_t hp = 0; hp < n_slots; ++hp)
            fprintf(uf, "{\"t\":\"pick\",\"phrase\":%u,\"slot\":%u,"
                        "\"uid\":%d}\n",
                    (unsigned)phrase_idx, (unsigned)hp,
                    path_uids[hp] == 0xFFFFFFFFu ? -1 : (int)path_uids[hp]);
    }

    /* SPFY_UID_OVERRIDE - substitute chosen units and hear the result. */
    uid_override_apply(phrase_idx, path_uids, n_slots);

    /* (Was: SPFY_NO_FORCE_END_SILENCE / force-set last UID to silence
     * sentinel. */

    /* Decode chosen v->units to audio via WSOLA streamer. */
    int      prev_have = 0;
    uint16_t prev_file_idx = 0, prev_local_pos = 0, prev_dur_like = 0;
    /* F0 at the trailing edge of the last emitted span - fed into WSOLA for
     * PSOLA voiced-join detection. */
    uint8_t  prev_f0_end = 0;
    /* Inter-word silence: engine inserts visible breathing gaps between
     * words (clearly audible in waveform A/B). */
    uint32_t prev_word_idx = 0xFFFFFFFFu;
#ifdef SPFY_TRACE
    /* [live-trace] previous word_post for the global-word tagger; resets
     * per phrase so the first content word of each phrase ticks g_wseq. */
    uint32_t g_wprev = 0xFFFFFFFFu;
#endif
    /* Set up below, once silence_n is known - the contour's timeline has to
     * account for inter-word silence. */
    prosody_stage_t pros;
    memset(&pros, 0, sizeof pros);

    /* Inter-word gap fill. */
    static int16_t SILENCE_BUF[1024];
    {
        int floor_amp = 24;
        const char *fe_ = spfy_env("SPFY_INTERWORD_FLOOR");
        if (fe_ && *fe_) floor_amp = atoi(fe_);
        if (floor_amp < 0)   floor_amp = 0;
        if (floor_amp > 512) floor_amp = 512;
        uint32_t rng = 0x13579BDFu;
        for (size_t i = 0; i < sizeof SILENCE_BUF / sizeof *SILENCE_BUF; ++i) {
            rng = rng * 1664525u + 1013904223u;
            SILENCE_BUF[i] = floor_amp
                ? (int16_t)((int32_t)((rng >> 16) % (uint32_t)(2 * floor_amp + 1))
                            - floor_amp)
                : (int16_t)0;
        }
    }
    /* Inter-word silence injection: default OFF (0ms). */
    int env_silence_ms = 0;
    {
        const char *e = spfy_env("SPFY_INTERWORD_MS");
        if (e) env_silence_ms = atoi(e);
        if (env_silence_ms < 0)   env_silence_ms = 0;
        if (env_silence_ms > 200) env_silence_ms = 200;
    }
    size_t silence_n = (size_t)env_silence_ms * (size_t)v->vdb.sample_rate / 1000u;
    if (silence_n > sizeof SILENCE_BUF / sizeof *SILENCE_BUF)
        silence_n = sizeof SILENCE_BUF / sizeof *SILENCE_BUF;
    /* Gap-fade width. */
    uint32_t gap_fade_n = 0;
    if (silence_n > 0) {
        const char *gf = spfy_env("SPFY_GAP_FADE_MS");
        double gms = (gf && *gf) ? atof(gf) : 28.0;
        if (gms < 0.0) gms = 0.0;
        if (gms > 40.0) gms = 40.0;
        gap_fade_n = (uint32_t)(gms * (double)v->vdb.sample_rate / 1000.0);
        spfy_wsola_set_gap_ola(&ws, gap_fade_n);
    }
    size_t played = 0, skipped = 0, paired_same = 0, paired_cross = 0,
           interword_pauses = 0;

    /* --- optional prosody stage (PLAN_PROSODY_STAGE.md) ------------------
     * OFF unless SPFY_PROSODY_STAGE is set, so the byte-exact audit is
     * unaffected. */
    {
        spfy_contour_params_t cp;
        /* Lazy S4-mode application for callers that never went through
         * main() -- the SAPI DLL builds this file with SPFY_SYNTH_NO_MAIN,
         * so exporting SPFY_4_MODE would otherwise do nothing there. */
        spfy4_mode_apply(NULL, 0);
        const char *pm_stem = spfy_env("SPFY_PROSODY_PM");
        if (spfy_contour_env(&cp) && pm_stem && *pm_stem) {
            if (spfy_pmarks_load(pm_stem, &pros.marks) != SPFY_OK) {
                spfy_log_warn("prosody: no marks at '%s'; stage disabled",
                              pm_stem);
            } else {
                /* pau lookup for the getPitchMarks-faithful skip above. */
                pros.pau_label   = 0xFFFFu;
                pros.phone_center = NULL;
                pros.phone_stride = 0;
                pros.n_units      = v->units.n_units;
                /* SPFY_PROSODY_F0_TABLE=<path> - every unit's F0 by the
                 * engine's own definition, written once per process. */
                {
                    static int f0_table_done = 0;
                    const char *ft = spfy_env("SPFY_PROSODY_F0_TABLE");
                    if (ft && *ft && !f0_table_done) {
                        f0_table_done = 1;
                        FILE *tf = fopen(ft, "wb");
                        if (!tf) {
                            spfy_log_warn("prosody: cannot write F0 table "
                                          "'%s'", ft);
                        } else {
                            uint32_t nu = v->units.n_units;
                            if (pros.marks.n_units < nu)
                                nu = pros.marks.n_units;
                            /* file_idx and the halfphone class travel with
                             * the F0 so an offline ceiling can price the
                             * JOIN the same three ways spfy_reselect_find
                             * does -- consecutive (free), same recording,
                             * elsewhere. */
                            for (uint32_t u = 0; u < nu; ++u) {
                                spfy_unit_record_t ur;
                                float f = spfy_reselect_unit_f0(
                                    &pros.marks, u, pros.marks.rate);
                                if (f <= 0.0f) continue;
                                if (spfy_unit_record_get(&v->units, u, &ur)
                                    != SPFY_OK) continue;
                                fprintf(tf, "%u %.2f %u %u %u\n", u, (double)f,
                                        (unsigned)ur.file_idx,
                                        (unsigned)ur.phone_center,
                                        (unsigned)(ur.is_first_half ? 1u : 0u));
                            }
                            fclose(tf);
                            spfy_log_warn("prosody: wrote F0 table for %u "
                                          "unit(s) to %s", nu, ft);
                        }
                    }
                }
                if (!env_flag_off("SPFY_PROSODY_PM_PAU")) {
                    uint8_t ph = spfy_phone_order_index(&v->phone_order, "pau");
                    if (ph != SPFY_PHONE_NONE
                        && ph < v->phone_order.n_phones
                        && v->phone_order.feat_to_labl) {
                        uint8_t lab = v->phone_order.feat_to_labl[ph];
                        if (lab != SPFY_PHONE_NONE && v->units.data) {
                            pros.pau_label = lab;
                            pros.phone_center = v->units.data
                                              + v->units.off_phone_center;
                            pros.phone_stride = v->units.rec_size;
                        }
                    }
                    if (pros.pau_label == 0xFFFFu)
                        spfy_log_warn("prosody: no 'pau' phone; pau marks "
                                      "will be read (engine skips them)");
                }
                uint32_t  n_c  = n_slots;
                uint32_t *cdur = (uint32_t *)calloc(n_c ? n_c : 1,
                                                    sizeof *cdur);
                /* The ToBI arrays are sized n_hp; the timeline is sized n_slots. */
                uint8_t *acc = (uint8_t *)calloc(n_c ? n_c : 1, sizeof *acc);
                int8_t  *typ = (int8_t  *)calloc(n_c ? n_c : 1, sizeof *typ);
                int8_t  *bto = (int8_t  *)calloc(n_c ? n_c : 1, sizeof *bto);
                /* Per-word <prosody pitch="Nst"> offsets. */
                int8_t  *pst = (int8_t  *)calloc(n_c ? n_c : 1, sizeof *pst);
                /* Syllable id on the same timeline, so accent runs can be
                 * cut where the syllable changes. */
                uint32_t *syl = (uint32_t *)calloc(n_c ? n_c : 1, sizeof *syl);
                uint8_t  *nuc = (uint8_t *)calloc(n_c ? n_c : 1, sizeof *nuc);
                if (cdur && acc && typ && bto && pst && syl && nuc) {
                    uint32_t n_copy = (n_hp < n_c) ? n_hp : n_c;
                    memcpy(acc, hp_accent,  n_copy * sizeof *acc);
                    memcpy(typ, hp_acctype, n_copy * sizeof *typ);
                    memcpy(bto, hp_btone,   n_copy * sizeof *bto);
                    if (hp_pitch_st)
                        memcpy(pst, hp_pitch_st, n_copy * sizeof *pst);
                    if (hp_syl)
                        memcpy(syl, hp_syl, n_copy * sizeof *syl);
                    if (hp_nuc)
                        memcpy(nuc, hp_nuc, n_copy * sizeof *nuc);
                    /* \![ToBI:] overrides the FE's accent decision per word. */
                    if (hp_tobi) {
                        uint32_t n_ov = 0;
                        for (uint32_t k = 0; k < n_copy; ++k) {
                            uint8_t on = 0;
                            int8_t  bias = 0;
                            if (!hp_tobi[k]) continue;
                            if (!spfy_tobi_by_code(hp_tobi[k], &on, &bias))
                                continue;
                            acc[k] = on;
                            if (on) typ[k] = bias;
                            ++n_ov;
                        }
                        if (n_ov)
                            spfy_log_warn("prosody: ToBI override on %u "
                                          "halfphone(s)", n_ov);
                    }
                    prosody_slot_out_dur(v, path_uids, n_slots, hp_to_post,
                                         (const uint32_t (*)[5])slice_ctx.ctx,
                                         hp_word_idx, silence_n,
                                         spfy_env("SPFY_NO_RUN_BATCH") != NULL,
                                         cdur);
                    if (spfy_contour_build(&pros.contour, &cp, cdur,
                                           (int)n_c, acc, typ, syl, nuc, bto,
                                           pst,
                                           (int)v->vdb.sample_rate) == 0) {
                        pros.on       = 1;
                        pros.out0     = sink->n_samples_written;
                        pros.slot_dur = cdur;
                        /* SPFY_TIMELINE_DUMP=1 - the authoritative map from
                         * half-phone to OUTPUT SAMPLES, on the same axis
                         * the mark dump's `wpos` uses. */
                        /* The contour's OWN accent positions, in nominal
                         * samples -- the same axis [tl] uses. */
                        if (spfy_env("SPFY_TIMELINE_DUMP")) {
                            for (int ai = 0; ai < pros.contour.n_acc; ++ai)
                                fprintf(stderr, "[acc] %u %d %.1f %.4f\n",
                                        phrase_idx, ai,
                                        pros.contour.pos[ai],
                                        pros.contour.height[ai]);
                        }
                        if (spfy_env("SPFY_TIMELINE_DUMP")) {
                            /* TWO AXES, both emitted, because they are NOT
                             * the same and joining the wrong one is the
                             * mistake this dump exists to prevent. */
                            unsigned long acc_s = 0UL;
                            for (uint32_t k = 0; k < n_c; ++k) {
                                uint32_t s2 = (k < n_hp) ? hp_to_post[k] : 0u;
                                const char *pn = "?";
                                unsigned half = 0;
                                if (k < n_hp && s2 < tree.n_slots
                                    && v->phone_order.phone_names) {
                                    uint32_t hc = slice_ctx.ctx[s2][2];
                                    if ((hc >> 1) < v->phone_order.n_phones)
                                        pn = v->phone_order.phone_names[hc >> 1];
                                    half = hc & 1u;
                                }
                                int stress = -1;
                                if (k < n_hp && hp_syl && hp_syl[k] >= 1
                                    && fe_utt.syl_stress
                                    && (hp_syl[k] - 1) < fe_utt.n_syls)
                                    stress =
                                        (int)fe_utt.syl_stress[hp_syl[k] - 1];
                                fprintf(stderr,
                                        "[tl] %u %u %lu %lu %lu %s %u %u %d %u "
                                        "%u %u\n",
                                        phrase_idx, k, acc_s,
                                        acc_s + (unsigned long)cdur[k],
                                        (unsigned long)pros.out0,
                                        pn, half,
                                        (k < n_hp && hp_syl) ? hp_syl[k] : 0u,
                                        stress,
                                        (k < n_c) ? acc[k] : 0u,
                                        (k < n_c) ? nuc[k] : 0u,
                                        (k < n_hp) ? hp_word_idx[k] : 0u);
                                acc_s += (unsigned long)cdur[k];
                            }
                        }
                        cdur          = NULL;
                        spfy_log_warn("prosody: ON - total %.0f smp, %d accent(s), "
                                      "base %.1f Hz, accent %.2f st, "
                                      "fall %.2f st%s",
                                      pros.contour.total, pros.contour.n_acc,
                                      (double)cp.base_hz,
                                      (double)cp.accent_st,
                                      (double)cp.fall_st,
                                      pros.contour.have_fall ? " (L-L%)" : "");
                        /* SPFY_PROSODY_SLOT_DUMP=1 - SELECTION against the
                         * contour, which nothing emitted before.
                         *
                         * The mark dump answers "what did the warp do"; it
                         * cannot answer "was the unit a good starting point",
                         * because by the time marks exist the unit is fixed.
                         * This is the only point where the chosen uid and the
                         * contour coexist, so it is where the question is
                         * answerable at all.
                         *
                         * `nat` is the engine's own unit F0 (the expression
                         * spfy_reselect_find would score with), `st` the
                         * contour at the slot's CENTRE, and `land` where this
                         * unit ends up once the relative warp is applied:
                         * nat * 2^(st/12). In relative mode `land` is the only
                         * one of the three that selection can move -- st is a
                         * function of time alone -- so it is the quantity any
                         * selection change has to be judged on.
                         *
                         * Joins to SPFY_UID_DUMP's "cands" records on
                         * (phrase, slot), which is what supplies the CEILING:
                         * the best `land` reachable from the pool the DP was
                         * actually given.
                         *
                         * ⚠ EMITTED AFTER THE RESELECT PASS, not here. Placed
                         * here it recorded the path BEFORE any substitution,
                         * so a working reselect would have measured as inert. */
                        /* Where the FE actually put the accents. */
                        /* Per-word pitch offsets actually installed in the
                         * contour, collapsed to runs. */
                        if (synth_is_verbose() && pros.contour.n_seg) {
                            int k = 0;
                            while (k < pros.contour.n_seg) {
                                int j = k;
                                while (j + 1 < pros.contour.n_seg
                                       && pros.contour.seg_off[j + 1]
                                          == pros.contour.seg_off[k]) ++j;
                                if (pros.contour.seg_off[k] != 0.0f)
                                    spfy_log_warn("prosody:   word offset "
                                                  "%+.1f st over slots %d..%d "
                                                  "(%.0f..%.0f smp)",
                                                  (double)pros.contour.seg_off[k],
                                                  k, j,
                                                  k ? pros.contour.seg_end[k-1] : 0.0,
                                                  pros.contour.seg_end[j]);
                                k = j + 1;
                            }
                        }
                        if (synth_is_verbose())
                            for (int k = 0; k < pros.contour.n_acc; ++k)
                                spfy_log_warn("prosody:   accent %d at %.0f "
                                              "smp (%.1f%% of phrase), "
                                              "%.2f st",
                                              k, pros.contour.pos[k],
                                              100.0 * pros.contour.pos[k]
                                                    / pros.contour.total,
                                              pros.contour.height[k]);
                    }
                }
                free(cdur); free(acc); free(typ); free(bto); free(pst);
                free(syl); free(nuc);

                /* --- F0-aware re-selection (SPFY_PROSODY_RESELECT) ---
                 * Take the accent from SELECTION where a unit at the wanted
                 * pitch exists (free, no artifacts) and leave only the
                 * residual to PSOLA. */
                if (pros.on && spfy_env("SPFY_PROSODY_RESELECT")) {
                    spfy_reselect_t rs;
                    spfy_reselect_params_t rp;
                    spfy_reselect_defaults(&rp);
                    {
                        const char *g = spfy_env("SPFY_PROSODY_RESELECT_GAIN");
                        if (g && *g) rp.min_gain_st = (float)atof(g);
                        const char *x = spfy_env("SPFY_PROSODY_RESELECT_CROSS");
                        if (x && *x) rp.cross_rec_st = (float)atof(x);
                        const char *cx = spfy_env("SPFY_PROSODY_RESELECT_CTX");
                        if (cx && *cx) rp.ctx_strict = atoi(cx);
                    }
                    if (spfy_reselect_build(&rs, &v->units, &pros.marks)
                        == SPFY_OK) {
                        uint32_t n_c2 = (n_hp < n_slots) ? n_hp : n_slots;
                        /* Target is RELATIVE to each unit: "find a unit
                         * like this one, but N semitones higher", N from
                         * the contour. */
                        double tpos = 0.0;
                        int n_sub = 0, n_free = 0, n_same = 0, n_cross = 0;
                        /* BAND mode. */
                        const int band_mode =
                            spfy_env("SPFY_PROSODY_RESELECT_BAND") != NULL;
                        /* Default to the knee's own bounds so the two are
                         * aimed at the same thing. */
                        const float band_lo =
                            env_f("SPFY_PROSODY_RESELECT_BAND_LO",
                                  env_f("SPFY_PROSODY_F0_FLOOR_HZ", 0.0f));
                        const float band_hi =
                            env_f("SPFY_PROSODY_RESELECT_BAND_HI",
                                  env_f("SPFY_PROSODY_F0_CEIL_HZ", 0.0f));
                        if (band_mode && band_lo <= 0.0f && band_hi <= 0.0f)
                            spfy_log_warn("prosody: RESELECT_BAND set but no "
                                          "F0_FLOOR_HZ/F0_CEIL_HZ - inert");
                        for (uint32_t k = 0; k < n_c2; ++k) {
                            uint32_t uid0 = path_uids[k];
                            uint32_t dur_s = pros.slot_dur[k];
                            /* SPFY_PROSODY_RESELECT_ALL lifts the accented-only gate. */
                            static int resel_all = -1;
                            if (resel_all < 0)
                                resel_all = (spfy_env("SPFY_PROSODY_RESELECT_ALL")
                                             != NULL);
                            /* Band mode scores every voiced slot: the slots
                             * that land out of band are phrase-final, and
                             * the accented-only gate excludes exactly
                             * those. */
                            if ((hp_accent[k] || resel_all || band_mode)
                                && uid0 != 0xFFFFFFFFu
                                && uid0 < rs.n_units && rs.f0[uid0] > 0.0f) {
                                uint16_t nb = UINT16_MAX;
                                if (k > 0 && path_uids[k - 1] != 0xFFFFFFFFu
                                    && path_uids[k - 1] < rs.n_units)
                                    nb = rs.file_idx[path_uids[k - 1]];
                                uint32_t pv = (k > 0) ? path_uids[k - 1]
                                                      : 0xFFFFFFFFu;
                                uint32_t nu;
                                if (band_mode) {
                                    double stv = (double)spfy_contour_st_at(
                                        &pros.contour, tpos + dur_s * 0.5);
                                    nu = spfy_reselect_find_band(
                                        &rs, &rp, uid0, pow(2.0, stv / 12.0),
                                        band_lo, band_hi, nb, pv);
                                } else {
                                    float tgt = spfy_contour_at(
                                        &pros.contour, tpos + dur_s * 0.5,
                                        rs.f0[uid0]);
                                    nu = spfy_reselect_find(&rs, &rp, uid0,
                                                            tgt, nb, pv);
                                }
                                if (nu != uid0) {
                                    /* Join tier of each substitution. */
                                    if (pv != 0xFFFFFFFFu && nu == pv + 1u)
                                        ++n_free;
                                    else if (nb != UINT16_MAX
                                             && nu < rs.n_units
                                             && rs.file_idx[nu] == nb)
                                        ++n_same;
                                    else
                                        ++n_cross;
                                    path_uids[k] = nu; ++n_sub;
                                }
                            }
                            tpos += (double)dur_s;
                        }
                        spfy_log_warn("prosody: reselect substituted %d unit(s)"
                                      " - %d free join, %d same recording,"
                                      " %d CROSS recording",
                                      n_sub, n_free, n_same, n_cross);
                        spfy_reselect_free(&rs);
                    }
                }
                /* SPFY_PROSODY_SLOT_DUMP - see the note at the contour build. */
                if (pros.on && spfy_env("SPFY_PROSODY_SLOT_DUMP")) {
                    double sp = 0.0;
                    for (uint32_t k = 0; k < n_slots; ++k) {
                        uint32_t su = path_uids[k];
                        double dur = (double)pros.slot_dur[k];
                        double ctr = sp + 0.5 * dur;
                        double stv = (double)spfy_contour_st_at(&pros.contour,
                                                                ctr);
                        float nat = (su != 0xFFFFFFFFu)
                            ? spfy_reselect_unit_f0(&pros.marks, su,
                                                    pros.marks.rate)
                            : 0.0f;
                        fprintf(stderr,
                                "[slot] %u %u %d %.2f %.4f %.2f %.0f %.0f %d\n",
                                phrase_idx, k,
                                su == 0xFFFFFFFFu ? -1 : (int)su,
                                (double)nat, stv,
                                nat > 0.0f
                                    ? (double)nat * pow(2.0, stv / 12.0) : 0.0,
                                sp, dur, (int)(k < n_hp ? hp_accent[k] : 0));
                        sp += dur;
                    }
                }
                if (!pros.on) spfy_pmarks_free(&pros.marks);
            }
        }
    }

    /* Phrase-start boundary event: fires before this phrase's first unit is
     * pushed, at the current output sample count. */
    if (cb && cb->phrase_cb)
        cb->phrase_cb(cb->ctx, phrase_idx,
                      out_pos(sink, sink->n_samples_written));

    /* Position on the prosody contour's timeline. */
    uint64_t nom_pos = 0;
    /* Target duration (ms) for this phrase's pau pad slots.
     *
     * FUN_08ee2960 sizes every sub-unit before handing it to the OLA:
     *
     *     pau  -> scale * (float)sub[0x18]     <- the FE's target
     *     else -> scale * (int)  sub[0x08]     <- the unit table's dur
     *
     * So a pause plays for as long as the FE ASKED, not for the length of
     * whatever recorded silence the unit search happened to pick. Tom's pau
     * units carry dur_like 118-203 ms while the FE typically asks for
     * 12.5-50 ms, which is why sizing them from dur_like made every render
     * open and close far too long -- ~62% of our total excess samples were
     * in the leading pad alone.
     *
     * The slot builder emits two leading and two trailing pau pads per
     * phrase ((n_phons + 2) * 2 slots), and a live capture of the engine's
     * unit loads shows it loading exactly those four, with the same target
     * on each side. Target ms = p/2; see FE_PAU_DEFAULT_P for `?d`.
     *
     * SPFY_PAU_FULL_DUR=1 restores dur_like sizing. */
    static int pau_full = -1;
    if (pau_full < 0) pau_full = (spfy_env("SPFY_PAU_FULL_DUR") != NULL);
    uint32_t pau_smp_lead = 0, pau_smp_trail = 0;
    if (!pau_full && parsed && phrase_idx < FE_PARSE_MAX_PHRASES) {
        int pb = parsed->phrase_pau_p_before[phrase_idx];
        int pa = parsed->phrase_pau_p_after [phrase_idx];
        if (pb <= 0) pb = FE_PAU_DEFAULT_P;
        if (pa <= 0) pa = FE_PAU_DEFAULT_P;
        /* target_ms = ROUND(p/2), then whole milliseconds -> samples.
         *
         * ⚠ NOT p*sps/2. FUN_08ee1ee0 resolves the target through
         * FUN_08ee8828 (an ST0 -> int converter) and writes the result back
         * into sub[0x08] as WHOLE MILLISECONDS, with sub[0x0c] = ms << shift.
         * Read straight out of the engine at a join: f=12.5 -> dur=13,
         * smp=104 (not 100); f=25.0 -> 25/200; f=50.0 -> 50/400. Half rounds
         * UP, so (p + 1) / 2 in integer arithmetic. Getting this wrong costs
         * 4 samples on every p25 pause. */
        uint32_t sps = v->vdb.sample_rate / 1000u;
        if (sps == 0) sps = 1u;
        pau_smp_lead  = (((uint32_t)pb + 1u) / 2u) * sps;
        pau_smp_trail = (((uint32_t)pa + 1u) / 2u) * sps;
    }
    /* --- pause-length knobs, every one identity by default ---------------
     * MEASURED AXIS: S4's median phrase pause runs ~50 ms longer than ours,
     * and no VCF parameter lengthens a pause in either direction -- the
     * reachable... */
    if (pau_smp_lead || pau_smp_trail) {
        static double pau_scale = -1.0, pau_extra = 0.0,
                      pau_lead_extra = 0.0, pau_trail_extra = 0.0;
        if (pau_scale < 0.0) {
            const char *e;
            pau_scale = ((e = spfy_env("SPFY_PAUSE_SCALE")) && *e)
                        ? atof(e) : 1.0;
            if (pau_scale < 0.0) pau_scale = 0.0;
            if (pau_scale > 8.0) pau_scale = 8.0;
            pau_extra = ((e = spfy_env("SPFY_PAUSE_EXTRA_MS")) && *e)
                        ? atof(e) : 0.0;
            pau_lead_extra = ((e = spfy_env("SPFY_PAUSE_LEAD_EXTRA_MS")) && *e)
                             ? atof(e) : 0.0;
            pau_trail_extra = ((e = spfy_env("SPFY_PAUSE_TRAIL_EXTRA_MS")) && *e)
                              ? atof(e) : 0.0;
        }
        if (pau_scale != 1.0 || pau_extra != 0.0
            || pau_lead_extra != 0.0 || pau_trail_extra != 0.0) {
            /* Samples per ms, taken from the voice rather than the `sps`
             * macro, which is not in scope this early in the function. */
            double per_ms = (double)v->vdb.sample_rate / 1000.0;
            if (per_ms <= 0.0) per_ms = 1.0;
            if (pau_smp_lead) {
                double ms = (double)pau_smp_lead / per_ms * pau_scale
                            + pau_extra + pau_lead_extra;
                if (ms < 0.0)    ms = 0.0;
                if (ms > 2000.0) ms = 2000.0;
                pau_smp_lead = (uint32_t)(ms * per_ms + 0.5);
            }
            if (pau_smp_trail) {
                double ms = (double)pau_smp_trail / per_ms * pau_scale
                            + pau_extra + pau_trail_extra;
                if (ms < 0.0)    ms = 0.0;
                if (ms > 2000.0) ms = 2000.0;
                pau_smp_trail = (uint32_t)(ms * per_ms + 0.5);
            }
        }
    }
    #define PAU_TARGET_SMP(si) \
        ((pau_full || n_slots < 4u) ? 0u \
         : ((si) < 2u ? pau_smp_lead \
            : ((si) >= n_slots - 2u ? pau_smp_trail : 0u)))
    uint32_t s = 0;
    while (s < n_slots) {
        uint32_t u = path_uids[s];
        if (u == 0xFFFFFFFFu) {
            /* Partial-anchor overshoot - audio for these HPs is supplied by
             * the anchor's UID range above; don't emit silence here. */
            if (pros.on) nom_pos += pros.slot_dur[s];
            ++s; continue;
        }
        /* ⚠ uid 0 AND 169578 ARE REAL UNITS, NOT MARKERS.
         *
         * The VIN holds 169579 units indexed 0..169578, so "SILENCE_SENTINEL"
         * is the LAST valid index, and uid 0 resolves to file_idx 0,
         * local_pos 0, dur_like 203, phone_center 32 = Tom's `pau`. Both are
         * addressable audio, and the engine plays them: FUN_08ee2960 reads
         * every sub-unit's samples from the speech DB through the provider
         * vtable, and there is NO silence generator anywhere on that path.
         * Its only mention of "pau" is a strncmp inside the
         * apply_target_prosody branch, which is off.
         *
         * ⚠ BUT EMITTING THEM IN FULL IS ALSO WRONG, so this is OFF by
         * default until the amount is understood. Tested: letting uid 0
         * through moved the FIRST DIFFERING SAMPLE from index 0 to index 2 on
         * every corpus text - samples 0 and 1 then match the engine exactly,
         * which confirms the engine really does open on this unit - but the
         * length ratio went the wrong way, 1.27 -> 1.85 median, because uid 0
         * carries dur_like 203 (1624 samples) and the engine emits only ~65-80
         * of them. It uses the pau to PRIME THE CROSSFADE TAIL, not as body.
         *
         * ✅ RESOLVED -- "how much" is the FE's target, not dur_like. The
         * engine's own answer is readable at sub+0x18 (see PAU_TARGET_MS
         * above), so these units are now emitted at that length and no
         * longer skipped. Skipping was costing the leading pad entirely and
         * sizing the survivors from dur_like was costing the rest.
         *
         * SPFY_PAU_SKIP=1 restores the old skip, for A/B only. */
        static int pau_skip = -1;
        if (pau_skip < 0) pau_skip = (spfy_env("SPFY_PAU_SKIP") != NULL);
        static int skip_trace = -1;
        if (skip_trace < 0)
            skip_trace = (spfy_env("SPFY_WSOLA_TRACE") != NULL);
        if (u >= v->units.n_units
            || (pau_skip && (u == 0 || u == SILENCE_UID(v)))) {
            if (skip_trace)
                fprintf(stderr, "[wsolask] slot=%u uid=%u reason=%s "
                        "n_units=%u\n", s, u,
                        (u >= v->units.n_units) ? "oob" : "pau_skip",
                        v->units.n_units);
            if (pros.on) nom_pos += pros.slot_dur[s];
            ++skipped; ++s; prev_have = 0; prev_f0_end = 0; continue;
        }
        spfy_unit_record_t r1;
        if (spfy_unit_record_get(&v->units, u, &r1) != SPFY_OK) {
            if (skip_trace)
                fprintf(stderr, "[wsolask] slot=%u uid=%u reason=record_get\n",
                        s, u);
            if (pros.on) nom_pos += pros.slot_dur[s];
            ++skipped; ++s; prev_have = 0; prev_f0_end = 0; continue;
        }

        /* Inter-word silence injection. */
        uint32_t this_post = hp_to_post[s];
        const uint32_t (*ctx_arr)[5] = (const uint32_t (*)[5])slice_ctx.ctx;
        int this_is_silence = ctx_is_silence(v, ctx_arr[this_post][2]);
        uint32_t this_word_idx = hp_word_idx[s];
        if (silence_n > 0 && prev_have
            && prev_word_idx != 0xFFFFFFFFu
            && this_word_idx != prev_word_idx
            && !this_is_silence) {
            /* Push with align=1 so the OLA blend Hann-fades the previous
             * voiced tail down to zero across the OLA region (10 ms),
             * rather than one-sample-cutting it to zero. */
            int sil_align = (spfy_env("SPFY_WSOLA_NO_SILENCE_FADE") != NULL)
                            ? 0 : 1;
            /* ENTRY side of the gap: the tail is real speech but the chunk
             * going in is fill, so the low-NCC guard would otherwise
             * collapse the fade-OUT to 2 ms exactly as it did the fade-in. */
            spfy_wsola_mark_next_push_synthetic(&ws);
            /* Chunk length must host the whole blend, or do_ola_blend takes
             * its degraded short-chunk path and swallows the held tail:
             * [eff_ola blend] [body = the actual gap] [save_n held tail]
             * The blend REPLACES the tail the... */
            size_t push_n = silence_n;
            if (gap_fade_n > 0) {
                spfy_wsola_request_tail_save(&ws, gap_fade_n);
                push_n = (size_t)gap_fade_n * 2u + silence_n;
                if (push_n > sizeof SILENCE_BUF / sizeof *SILENCE_BUF)
                    push_n = sizeof SILENCE_BUF / sizeof *SILENCE_BUF;
            }
            (void)spfy_wsola_push_unit(&ws, SILENCE_BUF, push_n,
                                       sil_align);
            /* The held tail is now gap fill, not speech. */
            spfy_wsola_mark_tail_synthetic(&ws);
            ++interword_pauses;
            prev_have = 0;
            prev_f0_end = 0;
        }
        /* Fire the SAPI/CLI word-event callback at every non-silence slot
         * whose parent word differs from the previous one. */
        if (cb && cb->word_cb && !this_is_silence
            && this_word_idx != prev_word_idx) {
            cb->word_cb(cb->ctx, out_pos(sink, sink->n_samples_written));
        }
        if (!this_is_silence) prev_word_idx = this_word_idx;
        /* Engine-faithful batching: SWIttsWsolaConcat receives a pre-
         * batched WsolaUnit array where each entry groups a run of
         * CONSECUTIVE UIDs (uid, uid+1, uid+2, ...). Verified empirically
         * via wsola_unit_probe on Tom pangram (text_002): all 31 groups
         * are pure uid+1 runs, group sizes ranging 1..5. The engine then
         * does ONE OLA per group regardless of length, with sub-v->units
         * inside the group emitted verbatim.
         *
         * Old logic collapsed only PAIRS that matched phone_center +
         * file_idx + local_pos contiguity (44 pushes for the pangram's
         * 64 path UIDs). Switching to engine's pure uid+1 rule batches
         * arbitrary-length runs across phone boundaries (31 pushes,
         * matching engine), eliminating the ±100ms local timing drift
         * from mismatched OLA placement. SPFY_NO_RUN_BATCH=1 reverts to
         * pair-only legacy. */
        int no_run = (spfy_env("SPFY_NO_RUN_BATCH") != NULL);
        uint32_t run_n = 1;
        if (!no_run) {
            while (s + run_n < n_slots) {
                uint32_t v_prev = path_uids[s + run_n - 1];
                uint32_t v_next = path_uids[s + run_n];
                if (v_next == 0 || v_next == SILENCE_UID(v)
                    || v_next >= v->units.n_units) break;
                if (v_next != v_prev + 1u) break;
                /* A uid+1 run can STRADDLE A WORD BOUNDARY (the live-trace
                 * block below documents the same thing: "a run straddling a
                 * word boundary - e.g. */
                if (silence_n > 0
                    && hp_word_idx[s + run_n] != hp_word_idx[s + run_n - 1])
                    break;
                spfy_unit_record_t rn, rp;
                if (spfy_unit_record_get(&v->units, v_next, &rn) != SPFY_OK) break;
                if (spfy_unit_record_get(&v->units, v_prev, &rp) != SPFY_OK) break;
                /* Sanity: uid+1 should always imply same file_idx and
                 * contiguous local_pos in a well-formed VDB. */
                if (rn.file_idx != rp.file_idx) break;
                if (rn.local_pos < rp.local_pos
                    || rn.local_pos > rp.local_pos + rp.dur_like + 64u) break;
                ++run_n;
            }
        }
        /* Per-word volume gain for this slot (\!vp/\!vd), via the slot's
         * parent-syllable fe_shared into the syl_vol map. */
        float vol_gain = 1.0f;
        if (syl_vol) {
            uint32_t post = hp_to_post[s];
            if (post < tree.n_slots) {
                uint32_t syl_post = tree.slots[post].parent_idx;
                if (syl_post < tree.n_slots
                    && tree.slots[syl_post].kind == SPFY_SK_SYLLABLE) {
                    uint32_t sh = tree.slots[syl_post].fe_shared;
                    if (sh >= 1 && (sh - 1) < fe_utt.n_syls) {
                        uint32_t si = spfy_syl_effective(&fe_utt, sh - 1);
                        if (syl_vol[si])
                            vol_gain = (float)syl_vol[si] / 100.0f;
                    }
                }
            }
        }
#ifdef SPFY_TRACE
        /* [live-trace] per-slot global word index for this WSOLA push. */
        char ws_buf[1024];
        {
            const uint32_t (*cxg)[5] = (const uint32_t (*)[5])slice_ctx.ctx;
            int wo = 1; ws_buf[0] = '[';
            for (uint32_t k = s; k < s + run_n && wo < (int)sizeof ws_buf - 16; ++k) {
                uint32_t kp = hp_to_post[k];
                int ksil = ctx_is_silence(v, cxg[kp][2]);
                if (!ksil && hp_word_idx[k] != g_wprev) { ++g_wseq; g_wprev = hp_word_idx[k]; }
                wo += snprintf(ws_buf + wo, sizeof ws_buf - (size_t)wo,
                               "%s%d", k > s ? "," : "", ksil ? -1 : g_wseq);
            }
            ws_buf[wo++] = ']'; ws_buf[wo] = '\0';
        }
#endif
        /* Look ahead: if the slot right after this run starts a new word, a
         * gap is about to be inserted, so make this push hold back enough
         * real speech for the widened fade to work on. */
        if (gap_fade_n > 0 && s + run_n < n_slots
            && hp_word_idx[s + run_n] != hp_word_idx[s + run_n - 1])
            spfy_wsola_request_tail_save(&ws, gap_fade_n);

        /* Units an anchor carries past its half-phone span (see path_extra
         * at the expansion). */
        uint32_t extra_n = path_extra[s + run_n - 1u];
        {
            uint32_t last_uid = path_uids[s + run_n - 1u];
            if (last_uid >= v->units.n_units
                || last_uid + extra_n >= v->units.n_units)
                extra_n = 0;
        }
        uint32_t emit_n = run_n + extra_n;
        if (emit_n >= 2) {
            /* Output offset where this run's audio starts, captured before
             * the push so intra-run word events can be placed within it. */
            const uint32_t run_out_start = sink->n_samples_written;
            spfy_unit_record_t r_last;
            (void)spfy_unit_record_get(&v->units,
                                       path_uids[s + run_n - 1] + extra_n,
                                       &r_last);
            uint32_t span = (uint32_t)r_last.local_pos
                          + (uint32_t)r_last.dur_like
                          - (uint32_t)r1.local_pos;
            /* A uid+1 run can END on a pau pad -- e.g. */
            /* Scan the WHOLE run: the pau can sit at either end. */
            pau_resize_t run_pau[4];
            uint32_t     n_run_pau = 0;
            {
                uint32_t sps = v->vdb.sample_rate / 1000u;
                if (sps == 0) sps = 1u;
                for (uint32_t k = 0; k < run_n
                                  && n_run_pau < (uint32_t)(sizeof run_pau
                                                            / sizeof *run_pau);
                     ++k) {
                    uint32_t tsmp = PAU_TARGET_SMP(s + k);
                    if (!tsmp) continue;
                    spfy_unit_record_t rk;
                    if (spfy_unit_record_get(&v->units, path_uids[s + k], &rk)
                            != SPFY_OK) continue;
                    uint32_t nsmp = (uint32_t)rk.dur_like * sps;
                    if (tsmp == nsmp) continue;
                    run_pau[n_run_pau].off = ((uint32_t)rk.local_pos
                                              - (uint32_t)r1.local_pos) * sps;
                    run_pau[n_run_pau].nom = nsmp;
                    run_pau[n_run_pau].tgt = tsmp;
                    ++n_run_pau;
                }
            }
            int align = !prev_have || prev_file_idx != r1.file_idx;
            /* [live-trace] emitted span (a run of consecutive UIDs
             * collapsed into one WSOLA push). */
            spfy_trace_eventf("unit",
                "{\"slot\":%u,\"uid\":%u,\"lp\":%u,\"dur\":%u,\"t\":%u,\"align\":%d,\"run\":%u,\"ws\":%s}",
                s, u, (unsigned)r1.local_pos, span,
                sink->n_samples_written, align, run_n, ws_buf);
            uid_dump_emit(v, phrase_idx, s, path_uids, run_n,
                          r1.local_pos, run_out_start, sink);
            rc = append_recording_span(&ws, r1.file_idx, r1.local_pos,
                                       span, &v->feat, &v->vdb, &v->lookup, align,
                                       prev_f0_end, r1.f0_start,
                                       v->vdb.sample_rate, vol_gain,
                                       &pros, &(unit_ref_t){ path_uids[s],
                                             emit_n, nom_pos },
                                       run_pau, n_run_pau);
            if (rc != SPFY_OK) { free(path_uids); goto fail; }
            ++paired_same; played += emit_n;
            prev_have = 1;
            prev_file_idx = r_last.file_idx;
            prev_local_pos = r_last.local_pos;
            prev_dur_like  = r_last.dur_like;
            prev_f0_end    = r_last.f0_end;
            if (pros.on)
                for (uint32_t k = 0; k < run_n; ++k)
                    nom_pos += pros.slot_dur[s + k];
            /* Catch up word events for slots INSIDE the run. The per-slot
             * callback above only sees the run's first slot, so a run that
             * straddles a word boundary silently swallowed one event --
             * measured as exactly one missing word per phrase ("cloudy",
             * "Weather") against the engine's dumpwav --phonemes marks.
             * The live-trace block does this already for its own counter;
             * the SAPI/CLI callback did not.
             *
             * Offset: the whole run went out as ONE push starting at
             * `run_out_start`, so slot k's audio begins about
             * (local_pos_k - local_pos_first) into it. Approximate because
             * WSOLA's crossfade shortens the real output slightly, but far
             * better than reporting the run's start for every word in it.
             *
             * ⚠ local_pos is MILLISECONDS, not samples - this added the raw
             * delta and so placed intra-run word events at 1/8 of their true
             * offset (at 8 kHz), i.e. essentially back at the run start,
             * which is the very failure the block exists to fix. Scale by
             * the VDB's own rate rather than a literal 8, so a 16 kHz voice
             * is right too. Found while adding uid_dump_emit(), which needs
             * the identical arithmetic. */
            if (cb && cb->word_cb) {
                const uint32_t (*cxw)[5] =
                    (const uint32_t (*)[5])slice_ctx.ctx;
                uint32_t sps = v->vdb.sample_rate / 1000u;
                if (sps == 0) sps = 1;
                for (uint32_t k = s + 1; k < s + run_n; ++k) {
                    uint32_t kp = hp_to_post[k];
                    if (ctx_is_silence(v, cxw[kp][2])) continue;
                    if (hp_word_idx[k] == prev_word_idx) continue;
                    spfy_unit_record_t rk;
                    uint32_t off = run_out_start;
                    if (spfy_unit_record_get(&v->units, path_uids[k], &rk)
                            == SPFY_OK && rk.local_pos >= r1.local_pos)
                        off += ((uint32_t)rk.local_pos
                                - (uint32_t)r1.local_pos) * sps;
                    cb->word_cb(cb->ctx, out_pos(sink, off));
                    prev_word_idx = hp_word_idx[k];
                }
            }
            s += run_n;
        } else {
            int align = !prev_have || prev_file_idx != r1.file_idx
                     || (uint32_t)r1.local_pos
                        != (uint32_t)prev_local_pos + prev_dur_like;
            if (s + 1 < n_slots) {
                spfy_unit_record_t r2;
                if (path_uids[s+1] < v->units.n_units
                    && spfy_unit_record_get(&v->units, path_uids[s+1], &r2) == SPFY_OK
                    && r1.phone_center == r2.phone_center
                    && r1.file_idx != r2.file_idx) {
                    ++paired_cross;
                }
            }
            spfy_trace_eventf("unit",
                "{\"slot\":%u,\"uid\":%u,\"lp\":%u,\"dur\":%u,\"t\":%u,\"align\":%d,\"run\":1,\"ws\":%s}",
                s, u, (unsigned)r1.local_pos, (unsigned)r1.dur_like,
                sink->n_samples_written, align, ws_buf);
            uid_dump_emit(v, phrase_idx, s, path_uids, 1u,
                          r1.local_pos, sink->n_samples_written, sink);
            pau_resize_t one_pau; uint32_t n_one_pau = 0;
            {
                uint32_t sps = v->vdb.sample_rate / 1000u;
                if (sps == 0) sps = 1u;
                uint32_t tsmp = PAU_TARGET_SMP(s);
                uint32_t nsmp = (uint32_t)r1.dur_like * sps;
                if (tsmp && tsmp != nsmp) {
                    one_pau.off = 0u;
                    one_pau.nom = nsmp;
                    one_pau.tgt = tsmp;
                    n_one_pau   = 1u;
                }
            }
            rc = append_recording_span(&ws, r1.file_idx, r1.local_pos,
                                       r1.dur_like, &v->feat, &v->vdb, &v->lookup, align,
                                       prev_f0_end, r1.f0_start,
                                       v->vdb.sample_rate, vol_gain,
                                       &pros, &(unit_ref_t){ u, 1u, nom_pos },
                                       &one_pau, n_one_pau);
            if (rc != SPFY_OK) { free(path_uids); goto fail; }
            ++played; prev_have = 1;
            prev_file_idx = r1.file_idx;
            prev_local_pos = r1.local_pos;
            prev_dur_like  = r1.dur_like;
            prev_f0_end    = r1.f0_end;
            if (pros.on) nom_pos += pros.slot_dur[s];
            ++s;
        }
    }
    #undef PAU_TARGET_SMP
    /* The contour is built on the cumulative sum of pros.slot_dur[] and
     * sampled at nom_pos, which walks that same array. */
    if (pros.on) {
        double nominal = pros.contour.total;
        if ((double)nom_pos != nominal)
            spfy_log_warn("prosody: timeline walk ended at %llu, contour "
                          "total is %.0f - accents are displaced; "
                          "prosody_slot_out_dur() is out of sync with the "
                          "concat loop",
                          (unsigned long long)nom_pos, nominal);
        else if (synth_is_verbose())
            spfy_log_warn("prosody: timeline %.0f smp closed exactly; "
                          "%.0f smp real output (%.1f%% OLA overlap)",
                          nominal,
                          (double)(sink->n_samples_written - pros.out0),
                          100.0 * (nominal
                                   - (double)(sink->n_samples_written
                                              - pros.out0)) / nominal);
    }

    /* END OF PHRASE = end of one Wsola::process.
     *
     * The engine runs FUN_08ee3aa0 once PER PHRASE, not once per utterance:
     * its unit loop finishes by emitting `hop` samples from the history
     * buffer, and the next phrase starts over with unit 0 on the no-join
     * path. A live capture of "Hello, world." shows exactly that -- phrase
     * 1's first unit emits 360 = content - hop, the first-unit formula, not
     * a blended join.
     *
     * Carrying one streamer across phrases instead cost 102 samples at every
     * phrase boundary (the missing flush, less the lag the spurious join
     * consumed). Flushing here emits the held tail and clears the history,
     * so the next phrase's first push takes the no-join path by itself. */
    rc = spfy_wsola_flush(&ws);
    if (rc != SPFY_OK) goto fail;

    total_played += played;
    total_skipped += skipped;
    total_paired_same += paired_same;
    total_paired_cross += paired_cross;
    total_interword_pauses += interword_pauses;
    free(path_uids); path_uids = NULL;

    if (vslots) {
        for (uint32_t i = 0; i < n_slots; ++i) {
            if (cbuf) free(cbuf[i]);
            if (tbuf) free(tbuf[i]);
            if (cand_c68) free(cand_c68[i]);
            if (cand_c6c) free(cand_c6c[i]);
            if (cand_c70) free(cand_c70[i]);
            if (cand_c78) free(cand_c78[i]);
        }
    }
    free(vslots); vslots = NULL;
    free(cbuf); cbuf = NULL;
    free(tbuf); tbuf = NULL;
    free(cand_c68); cand_c68 = NULL;
    free(cand_c6c); cand_c6c = NULL;
    free(cand_c70); cand_c70 = NULL;
    free(cand_c78); cand_c78 = NULL;
    free(q5_per_slot); q5_per_slot = NULL;
    free(q5_has); q5_has = NULL;
    free(hp_to_post); hp_to_post = NULL;
    free(post_to_hp); post_to_hp = NULL;
    free(hp_word_idx); hp_word_idx = NULL;
    free(hp_pitch_st);
    free(hp_rate_pct);
    free(syl_vol);
    if (pros.on) {
        spfy_pmarks_free(&pros.marks);
        spfy_contour_free(&pros.contour);
        free(pros.slot_dur); pros.slot_dur = NULL;
        pros.on = 0;
    }
    free(hp_btone); hp_btone = NULL;
    free(hp_acctype); hp_acctype = NULL;
    free(hp_accent); hp_accent = NULL;
    free(hp_syl); hp_syl = NULL;
    free(hp_nuc); hp_nuc = NULL;
    free(dag_slots); dag_slots = NULL;
    if (anchor_cands) {
        for (uint32_t i = 0; i < tree.n_slots; ++i) {
            free(anchor_cands[i]); free(anchor_jks[i]); free(anchor_target[i]);
            if (anchor_c68) free(anchor_c68[i]);
            if (anchor_c6c) free(anchor_c6c[i]);
            if (anchor_c70) free(anchor_c70[i]);
            if (anchor_c78) free(anchor_c78[i]);
        }
        free(anchor_cands); anchor_cands = NULL;
        free(anchor_jks); anchor_jks = NULL;
        free(anchor_target); anchor_target = NULL;
        free(anchor_n); anchor_n = NULL;
        free(anchor_c68); anchor_c68 = NULL;
        free(anchor_c6c); anchor_c6c = NULL;
        free(anchor_c70); anchor_c70 = NULL;
        free(anchor_c78); anchor_c78 = NULL;
    }
    spfy_slot_preds_table_free(&preds_tab);
    preds_tab = (spfy_slot_preds_table_t){0};
    spfy_sp_target_table_free(&sp_tab);
    sp_tab = (spfy_sp_target_table_t){0};
    spfy_slice_ctx_table_free(&slice_ctx);
    slice_ctx = (spfy_slice_ctx_table_t){0};
    spfy_slot_tree_free(&tree);
    tree = (spfy_slot_tree_t){0};
    spfy_fe_utt_free(&fe_utt);
    fe_utt = (spfy_fe_utt_t){0};
    n_slots = 0;

    /* Inter-phrase silence - DEFAULT OFF (rely on FE pad slots). */
    if (phrase_idx + 1 < n_phrases) {
        int sil_ms = (inter_phrase_ms_override > 0) ? inter_phrase_ms_override : 0;
        /* User `\!pN` pause before the NEXT phrase, threaded from the FE
         * parser's phrase_lead_pause_ms (set from `pau(uN)` markers that
         * build_inline_mixed_tagged emits for embedded pause tags). */
        int npid = (int)phrase_idx + 1;
        if (npid < FE_PARSE_MAX_PHRASES
            && parsed->phrase_lead_pause_ms[npid] > sil_ms)
            sil_ms = parsed->phrase_lead_pause_ms[npid];
        if (sil_ms > 0) {
            static const int16_t INTER_PHRASE_SILENCE[16000] = {0};
            size_t cap = sizeof INTER_PHRASE_SILENCE / sizeof *INTER_PHRASE_SILENCE;
            size_t remain = (size_t)sil_ms * (size_t)v->vdb.sample_rate / 1000u;
            int sil_align = (spfy_env("SPFY_WSOLA_NO_SILENCE_FADE") != NULL) ? 0 : 1;
            while (remain > 0) {
                size_t n = remain > cap ? cap : remain;
                if (sil_align) spfy_wsola_mark_next_push_synthetic(&ws);
                (void)spfy_wsola_push_unit(&ws, INTER_PHRASE_SILENCE, n, sil_align);
                remain -= n;
                sil_align = 0;
            }
            /* Same as the inter-word case: the tail is now synthetic, so
             * the next real unit must not be lag-searched against it. */
            spfy_wsola_mark_tail_synthetic(&ws);
        }
    }

    /* Update sentence_idx_in_para for next phrase per engine logic
     * (FUN_08e8c7d0): reset to 0 at end-punct ('.', '?', '!'), else
     * increment. */
    {
        char term = '.';
        if ((int)phrase_idx < parsed->n_phrase_terms
            && parsed->phrase_terms[phrase_idx] != 0) {
            term = parsed->phrase_terms[phrase_idx];
        }
        if (term == '.' || term == '?' || term == '!') {
            sentence_idx_in_para = 0;
        } else {
            sentence_idx_in_para += 1;
        }
    }

    }

    rc = spfy_wsola_flush(&ws);

    if (out_stats) {
        out_stats->total_played           = total_played;
        out_stats->total_skipped          = total_skipped;
        out_stats->total_paired_same      = total_paired_same;
        out_stats->total_paired_cross     = total_paired_cross;
        out_stats->total_interword_pauses = total_interword_pauses;
        out_stats->wsola_aligned          = ws.n_aligned;
        out_stats->wsola_pushes           = ws.n_pushes;
        out_stats->n_phrases              = n_phrases;
        out_stats->samples_emitted        = sink->n_samples_written;
    }
    rc = SPFY_OK;
    goto cleanup;

fail:
cleanup:
    /* Undo a `\s4m` before returning, on EVERY exit path -- there are no
     * early returns in this function, so this label is the only way out. */
    if (s4_scoped) spfy4_env_restore(&s4_scope);
    free(split_buf);
    if (vslots) {
        for (uint32_t i = 0; i < n_slots; ++i) {
            if (cbuf) free(cbuf[i]);
            if (tbuf) free(tbuf[i]);
            if (cand_c68) free(cand_c68[i]);
            if (cand_c6c) free(cand_c6c[i]);
            if (cand_c70) free(cand_c70[i]);
            if (cand_c78) free(cand_c78[i]);
        }
    }
    free(vslots); free(cbuf); free(tbuf);
    free(cand_c68); free(cand_c6c); free(cand_c70); free(cand_c78);
    free(q5_per_slot); free(q5_has);
    free(hp_to_post); free(post_to_hp); free(hp_word_idx);
    free(hp_btone);
    free(hp_acctype);
    free(hp_accent);
    free(hp_syl);
    free(hp_nuc);
    free(dag_slots);
    if (anchor_cands) {
        for (uint32_t i = 0; i < tree.n_slots; ++i) {
            free(anchor_cands[i]); free(anchor_jks[i]); free(anchor_target[i]);
            if (anchor_c68) free(anchor_c68[i]);
            if (anchor_c6c) free(anchor_c6c[i]);
            if (anchor_c70) free(anchor_c70[i]);
            if (anchor_c78) free(anchor_c78[i]);
        }
        free(anchor_cands); free(anchor_jks); free(anchor_target); free(anchor_n);
        free(anchor_c68); free(anchor_c6c); free(anchor_c70); free(anchor_c78);
    }
    spfy_slot_preds_table_free(&preds_tab);
    spfy_sp_target_table_free(&sp_tab);
    spfy_slice_ctx_table_free(&slice_ctx);
    spfy_slot_tree_free(&tree);
    spfy_fe_utt_free(&fe_utt);
    spfy_prosody_hints_free(&hints);
    spfy_reselect_free(&dp_f0);
    spfy_pmarks_free(&dp_marks);
    /* Energy tables; all NULL when both levers were off. */
    v->av.unit_pow = NULL;
    v->av.unit_pow_n = 0u;
    v->av.pow_mean = NULL;
    v->av.pow_sd = NULL;
    v->av.pow_rows = 0u;
    v->av.w_pow_t = 0.0f;
    free(unit_pow);
    free(pow_mean);
    free(pow_sd);
    free(etags_text);
    free(etag_acc);
    free(hp_tobi);
    free(etag_vol);
    free(etag_rate);
    /* Everything previously freed here individually (v->bucket, carts,
     * v->chunks, FE, v->hpc, v->prsl, v->hash, v->pros, v->maps, v->ccos,
     * v->lookup, v->feat, v->vcf, v->vdb, v->vin, v->voicing_buf) is owned
     * by the... */
    return rc;
}


/* Word-event sidecar writer used when SPFY_WORD_EVENTS_FILE env is set. */
struct spfy_cli_wev_ctx {
    FILE     *fp;
    FILE     *pfp;
    unsigned *idx;
};
void spfy_cli_word_cb(void *ctx, uint32_t sample_offset);
void spfy_cli_word_cb(void *ctx, uint32_t sample_offset)
{
    struct spfy_cli_wev_ctx *c = (struct spfy_cli_wev_ctx *)ctx;
    if (!c || !c->fp) return;
    fprintf(c->fp, "%u\t%u\n", sample_offset, *c->idx);
    (*c->idx)++;
}

void spfy_cli_phrase_cb(void *ctx, uint32_t phrase_idx,
                        uint32_t sample_offset);
void spfy_cli_phrase_cb(void *ctx, uint32_t phrase_idx,
                        uint32_t sample_offset)
{
    struct spfy_cli_wev_ctx *c = (struct spfy_cli_wev_ctx *)ctx;
    if (!c || !c->pfp) return;
    fprintf(c->pfp, "%u\t%u\n", sample_offset, phrase_idx);
}

/* The CLI main() is gated so this same .c file can be compiled into both
 * spfy_synth.exe and spfy_sapi.dll. */
#ifndef SPFY_SYNTH_NO_MAIN
#include "embedded_assets.h"
#include "pitch_shift.h"
/* Reopen a rendered int16-mono WAV, compress its F0 contour toward `base`
 * by `ratio` via TD-PSOLA, and rewrite it in place. */
static int spfy_f0_flatten_wav_file(const char *path, float base, float ratio)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t hdr[44];
    if (fread(hdr, 1, 44, f) != 44
        || memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f); return -2;
    }
    uint32_t sr = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8)
                | ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
    long data_off = -1;
    uint32_t data_sz = 0;
    fseek(f, 12, SEEK_SET);
    for (;;) {
        uint8_t ch[8];
        if (fread(ch, 1, 8, f) != 8) break;
        uint32_t sz = (uint32_t)ch[4] | ((uint32_t)ch[5] << 8)
                    | ((uint32_t)ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (memcmp(ch, "data", 4) == 0) { data_off = ftell(f); data_sz = sz; break; }
        fseek(f, (long)sz, SEEK_CUR);
    }
    if (data_off < 0) { fclose(f); return -3; }
    size_t n = data_sz / 2u;
    int16_t *pcm = (int16_t *)malloc(n * sizeof(int16_t));
    int16_t *out = (int16_t *)malloc(n * sizeof(int16_t));
    if (!pcm || !out) { free(pcm); free(out); fclose(f); return -4; }
    fseek(f, data_off, SEEK_SET);
    if (fread(pcm, sizeof(int16_t), n, f) != n) {
        free(pcm); free(out); fclose(f); return -5;
    }
    fclose(f);
    if (spfy_f0_retarget_block(pcm, n, out, base, ratio, (int)sr) != 0) {
        free(pcm); free(out); return -6;
    }
    /* Rewrite data payload in place; header/size unchanged (duration is
     * preserved by the retarget). */
    FILE *w = fopen(path, "r+b");
    if (!w) { free(pcm); free(out); return -7; }
    fseek(w, data_off, SEEK_SET);
    size_t wr = fwrite(out, sizeof(int16_t), n, w);
    fclose(w);
    free(pcm); free(out);
    return wr == n ? 0 : -8;
}

/* Extract the embedded FE assets and report where they landed.
 *
 * ⚠ NOT KEYED ON THE BINARY'S MTIME ANY MORE. It used to be, which made every
 * rebuild a cold start: the directory name changed even though the FE tables
 * had not, so all 22 parity workers re-extracted 728 files each and a corpus
 * audit ran ~22 s against a ~10 s floor. The name now carries
 * SPFY_ASSETS_DIGEST, a hash of the embedded bytes, so it survives rebuilds
 * that leave the assets alone and necessarily changes when they do.
 *
 * Two candidates, in order:
 *   1. beside the executable - durable, and SHARED with spfy_sapi.dll when
 *      both are installed in the same directory, so the installer's elevated
 *      regsvr32 populates it once for every user on the machine;
 *   2. %TEMP% / $TMPDIR - for a read-only install dir, or a CLI run from a
 *      location this user cannot write to.
 *
 * Returns the directory in use, or NULL if neither candidate worked. */
static const char *resolve_assets(const char *argv0, spfy_asset_paths_t *out)
{
    static char dir[1024];
    char base[1024];

    if (argv0 && *argv0) {
        size_t n = strlen(argv0);
        while (n > 0 && argv0[n - 1] != '/' && argv0[n - 1] != '\\') --n;
        if (n > 1 && n < sizeof base) {
            memcpy(base, argv0, n - 1);
            base[n - 1] = '\0';
            if (spfy_assets_dir(base, dir, sizeof dir) == 0
                && spfy_assets_extract(dir, out) == 0)
                return dir;
        }
    }

    const char *tmp;
#ifdef _WIN32
    tmp = spfy_env("TEMP");
    if (!tmp) tmp = spfy_env("TMP");
    if (!tmp) tmp = "C:\\Windows\\Temp";
#else
    tmp = spfy_env("TMPDIR");
    if (!tmp) tmp = "/tmp";
#endif
    if (spfy_assets_dir(tmp, dir, sizeof dir) == 0
        && spfy_assets_extract(dir, out) == 0)
        return dir;
    return NULL;
}

/* Print every voice the search path can see. Used by --list-voices and by
 * the "no such voice" error, because a name that did not match is only
 * actionable next to the names that would have. */
static void print_voice_list(const char *argv0, FILE *fp)
{
    static spfy_voice_paths v[256];
    size_t n = spfy_voice_list(argv0, v, sizeof v / sizeof v[0]);
    size_t i;

    if (n == 0) {
        fprintf(fp, "no voices found. searched:\n");
        if (*spfy_voice_search_path())
            fprintf(fp, "  %s\n", spfy_voice_search_path());
        else
            fprintf(fp, "  (no directory holding a language folder was found "
                        "-- set SPFY_VOICE_DIR)\n");
        return;
    }
    fprintf(fp, "%zu voice%s:\n", n, n == 1 ? "" : "s");
    for (i = 0; i < n; ++i)
        fprintf(fp, "  %-12s %-7s %s\n", v[i].name, v[i].lang, v[i].dir);
}

/* Read an entire text file into a malloc'd, NUL-terminated buffer, with
 * trailing whitespace stripped (an editor / `echo` newline would otherwise
 * ride into the final phrase). */
static char *read_text_file(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = '\0';

    /* Byte-order marks, the same two spfy_dumpwav's read_file_utf8 accepts. */
    if (got >= 3 && (unsigned char)buf[0] == 0xEF
                 && (unsigned char)buf[1] == 0xBB
                 && (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, got - 3 + 1);
        got -= 3;
    } else if (got >= 2 && (unsigned char)buf[0] == 0xFF
                        && (unsigned char)buf[1] == 0xFE) {
        /* UTF-16 LE -> UTF-8, hand-rolled so this stays portable to the
         * wasm and arm64 targets (no Win32 WideCharToMultiByte here). */
        const unsigned char *p = (const unsigned char *)buf + 2;
        size_t n_units = (got - 2) / 2;
        char *u8 = (char *)malloc(n_units * 3 + 1);
        if (!u8) { free(buf); return NULL; }
        size_t o = 0;
        for (size_t i = 0; i < n_units; ++i) {
            unsigned c = (unsigned)p[i * 2] | ((unsigned)p[i * 2 + 1] << 8);
            if (c < 0x80) {
                u8[o++] = (char)c;
            } else if (c < 0x800) {
                u8[o++] = (char)(0xC0 | (c >> 6));
                u8[o++] = (char)(0x80 | (c & 0x3F));
            } else {
                /* Surrogate pairs are not reassembled: the FE has no
                 * astral-plane phonemes, so a lone replacement is more
                 * honest than a silently mangled pair. */
                if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
                u8[o++] = (char)(0xE0 | (c >> 12));
                u8[o++] = (char)(0x80 | ((c >> 6) & 0x3F));
                u8[o++] = (char)(0x80 | (c & 0x3F));
            }
        }
        u8[o] = '\0';
        free(buf);
        buf = u8;
        got = o;
    }

    while (got > 0 && isspace((unsigned char)buf[got - 1])) buf[--got] = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    /* Two CLI forms: 5-arg (preferred): <voice.vin> <voice.vdb> <voice.vcf>
     * "<text>" <out.wav> vocab + fe_tables_{a,b} are embedded in the binary
     * (see spfy/tools/embed_assets.py) and extracted to a tempdir on
     * first... */
    const char *vin_path, *vdb_path, *vcf_path;
    /* hpc_path MUST default to NULL: the short form never assigns it, and
     * spfy_voice_load reads paths->hpclass[0] to decide load-vs-derive. */
    const char *hpc_path = NULL;
    /* NULL-initialised: both the short and legacy forms assign all three
     * before use, but the assignments sit in different branches and GCC
     * cannot prove it once there is a call between them. */
    const char *vocab = NULL, *tab_a = NULL, *tab_b = NULL;
    const char *text, *out_wav;
    spfy_asset_paths_t embedded_paths = {0};
    char *file_text = NULL;

    /* Pull the option flags (-f/--file and its =VALUE variants, -q/--quiet,
     * -v/--verbose) out of argv, compacting the remaining positionals down
     * so the argc-based layout below is unchanged apart from the missing
     * "<text>"... */
    const char *file_path_arg = NULL;
    /* --trace-stream: emit the live NDJSON event stream to stdout. */
    int trace_stream = 0;
    /* --s4 / -4 turn on Speechify 4 mode; --no-s4 forces it off even when
     * SPFY_4_MODE is exported. */
    int s4_flag = 0;
    /* The once-a-week update check, which runs AFTER the WAV is written and
     * never before it -- a synth the user is waiting on does not stop to
     * talk to GitHub. */
    int update_check = 1;
    {
        int w = 1;
        for (int r = 1; r < argc; r++) {
            const char *a = argv[r];
            if (strcmp(a, "--s4") == 0 || strcmp(a, "-4") == 0) {
                s4_flag = 1;
            } else if (strcmp(a, "--no-s4") == 0) {
                s4_flag = -1;
            } else if (strcmp(a, "-f") == 0 || strcmp(a, "--file") == 0) {
                if (r + 1 >= argc) {
                    fprintf(stderr, "%s: %s requires a file path argument\n",
                            argv[0], a);
                    return 2;
                }
                file_path_arg = argv[++r];
            } else if (strncmp(a, "--file=", 7) == 0) {
                file_path_arg = a + 7;
            } else if (strncmp(a, "-f=", 3) == 0) {
                file_path_arg = a + 3;
            } else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) {
                spfy_synth_verbose = 0;
            } else if (strcmp(a, "-v") == 0 || strcmp(a, "--verbose") == 0) {
                spfy_synth_verbose = 1;
            } else if (strcmp(a, "--trace-stream") == 0) {
                trace_stream = 1;
            } else if (strcmp(a, "--list-voices") == 0) {
                print_voice_list(argv[0], stdout);
                return 0;
#ifdef SPFY_HAVE_UPDATE_CHECK
            } else if (strcmp(a, "--version") == 0) {
                printf("%s\n", SPFY_VERSION);
                return 0;
            } else if (strcmp(a, "--no-update-check") == 0) {
                update_check = 0;
            } else if (strcmp(a, "--check-update") == 0) {
                /* Ignores the interval and reports "up to date" out loud --
                 * the automatic check is silent unless there is news. */
                int urc = spfy_upd_run(argv[0], 1, 0, stdout);
                if (urc == 0) printf("up to date (spfy %s)\n", SPFY_VERSION);
                else if (urc < 0)
                    fprintf(stderr, "update check failed (offline, or %s is "
                                    "unreachable)\n", spfy_upd_url());
                return urc < 0 ? 1 : 0;
#endif
            } else {
                argv[w++] = argv[r];
            }
        }
        argc = w;
    }

    /* Three positional layouts, told apart by count alone -- 1 path, 3 paths
     * or 7 paths -- so nothing has to guess at what a token means. */
    int short_form;
    int name_form = 0;
    if (argc == (file_path_arg ? 3 : 4)) {
        short_form = 1;
        name_form = 1;
    } else if (argc == (file_path_arg ? 5 : 6)) {
        short_form = 1;
    } else if (argc == (file_path_arg ? 9 : 10)) {
        short_form = 0;
    } else {
        fprintf(stderr,
            "usage: %s <voice> \"<text>\" <out.wav>\n"
            "   or: %s <voice.vin> <voice.vdb> <voice.vcf> \"<text>\" <out.wav>\n"
            "   or: %s <voice.vin> <voice.vdb> <voice.vcf> <hpclass.bin>\n"
            "          <vocab.json> <fe_tables_a> <fe_tables_b>\n"
            "          \"<text>\" <out.wav>          (legacy)\n"
            "\n"
            "  <voice> is a voice FOLDER NAME, matched case-insensitively --\n"
            "  `tom`, `Tom` and `TOM` all find en-US/tom/tom.{vin,8.vdb,vcf}.\n"
            "  A path to the voice directory works too. Searched, in order:\n"
            "  $SPFY_VOICE_DIR, the working directory and its parents, the\n"
            "  directory holding this binary and its parents, then\n"
            "  ~/Documents/Speechify. `--list-voices` prints what that finds.\n"
            "\n"
            "  -f, --file <path>   read input text from <path> instead of the\n"
            "                      \"<text>\" argument (which is then omitted)\n"
            "      --list-voices   print every discoverable voice and exit\n"
            "      --version       print the build version and exit\n"
            "      --check-update  check now for a newer engine or voice\n"
            "      --no-update-check   skip the automatic check this run\n"
            "                      (SPFY_NO_UPDATE_CHECK=1 disables it for good)\n"
            "  -q, --quiet         suppress per-synth diagnostics, keeping only\n"
            "                      the FE-backend banner (default)\n"
            "  -v, --verbose       print the full FE/synth pipeline diagnostics\n"
            "  -4, --s4            Speechify 4 mode: the f95_k1 prosody\n"
            "                      configuration, equivalent to SPFY_4_MODE=1.\n"
            "                      Individual SPFY_PROSODY_* variables still\n"
            "                      override it; see SPFY4_* near the top of\n"
            "                      spfy_synth.c for the values.\n"
            "      --no-s4         force the mode off, ignoring SPFY_4_MODE\n",
            argv[0], argv[0], argv[0]);
        return 2;
    }

    if (file_path_arg) {
        file_text = read_text_file(file_path_arg);
        if (!file_text) {
            fprintf(stderr, "%s: cannot read input file '%s'\n",
                    argv[0], file_path_arg);
            return 1;
        }
    }

    /* Resolved triple for the name form; the three const pointers below
     * reference it, so it has to outlive them. */
    static spfy_voice_paths found;

    {
        int i = 1;
        if (name_form) {
            const char *want = argv[i++];
            int rc = spfy_voice_resolve(want, argv[0], &found);
            if (rc == -1) {
                fprintf(stderr, "%s: no voice named '%s'.\n", argv[0], want);
                print_voice_list(argv[0], stderr);
                free(file_text);
                return 1;
            }
            if (rc == -2) {
                fprintf(stderr,
                        "%s: voice '%s' is incomplete in %s -- missing%s%s%s\n",
                        argv[0], want, found.dir,
                        found.vin[0] ? "" : " .vin",
                        found.vdb[0] ? "" : " .vdb",
                        found.vcf[0] ? "" : " .vcf");
                free(file_text);
                return 1;
            }
            vin_path = found.vin;
            vdb_path = found.vdb;
            vcf_path = found.vcf;
            if (spfy_synth_verbose)
                fprintf(stderr, "[voice] %s (%s) from %s\n",
                        found.name, found.lang[0] ? found.lang : "-",
                        found.dir);
        } else {
            vin_path = argv[i++];
            vdb_path = argv[i++];
            vcf_path = argv[i++];
        }
        if (!short_form) {
            hpc_path = argv[i++];
            vocab    = argv[i++];
            tab_a    = argv[i++];
            tab_b    = argv[i++];
        }
        text     = file_path_arg ? file_text : argv[i++];
        out_wav  = argv[i++];
    }

    /* Speechify 4 mode. */
    spfy4_note_vdb_path(vdb_path);
    spfy4_mode_apply(vdb_path, s4_flag);

    if (short_form) {
        if (resolve_assets(argv[0], &embedded_paths) == NULL) {
            fprintf(stderr, "failed to extract embedded assets beside %s "
                            "or into the temp directory\n", argv[0]);
            free(file_text);
            return 1;
        }
        /* hpc_path deliberately stays NULL: spfy_voice_load then derives
         * the hp_class table from THIS voice's VIN. */
        vocab    = embedded_paths.vocab;
        tab_a    = embedded_paths.tables_a;
        tab_b    = embedded_paths.tables_b;
    }

    /* Voice loaded once via the shared synth library; the rest of main()
     * references voice members through the `voice.` prefix below. */
    spfy_voice_t voice = {0};
    int rc;

    /* All voice tables (voice.vin/voice.vdb/voice.vcf,
     * voice.units/voice.feat/voice.lookup,
     * voice.ccos/voice.maps/voice.pros, voice.hash/voice.prsl,
     * voice.durt_cart/voice.f0tr_cart, voice.chunks, voice.hpc +
     * voice.bucket... */
    {
        spfy_voice_paths_t paths = {
            .vin         = vin_path,
            .vdb         = vdb_path,
            .vcf         = vcf_path,
            .hpclass     = hpc_path,
            .vocab       = vocab,
            .fe_tables_a = tab_a,
            .fe_tables_b = tab_b,
        };
        if ((rc = spfy_voice_load(&paths, &voice)) != SPFY_OK) {
            fprintf(stderr, "error loading voice: %s\n", spfy_strerror(rc));
            free(file_text);
            return 1;
        }
    }


    /* [live-trace] Route the NDJSON event stream to stdout when
     * --trace-stream was given. */
    if (trace_stream) spfy_trace_set_sink(stdout);

    spfy_wav_writer_t wav = {0};
    if ((rc = spfy_wav_open(&wav, out_wav, voice.vdb.sample_rate)) != SPFY_OK) {
        fprintf(stderr, "error opening %s: %s\n", out_wav, spfy_strerror(rc));
        spfy_voice_free(&voice);
        free(file_text);
        return 1;
    }
    /* SPFY_RATE=N applies a whole-utterance WSOLA time-scale in
     * spfy_wav_close(): N<1 slows down, N>1 speeds up. */
    {
        const char *rv = spfy_env("SPFY_RATE");
        if (rv && *rv) {
            double f = atof(rv);
            if (f > 0.25 && f < 4.0) spfy_wav_set_stretch(&wav, (float)f);
        }
    }

    /* Optional word-events sidecar for the 64-bit SAPI shim, plus an
     * optional phrase-events sidecar (analysis: exact per-utterance
     * segmentation of multi-phrase renders). */
    FILE *wev_fp = NULL;
    FILE *pev_fp = NULL;
    unsigned wev_word_idx = 0;
    {
        const char *wev_path = spfy_env("SPFY_WORD_EVENTS_FILE");
        if (wev_path && *wev_path) wev_fp = fopen(wev_path, "wb");
        const char *pev_path = spfy_env("SPFY_PHRASE_EVENTS_FILE");
        if (pev_path && *pev_path) pev_fp = fopen(pev_path, "wb");
    }
    struct spfy_cli_wev_ctx wev_ctx = { wev_fp, pev_fp, &wev_word_idx };
    spfy_synth_callbacks_t cb = {0};
    spfy_synth_callbacks_t *cbp = NULL;
    if (wev_fp || pev_fp) {
        if (wev_fp) cb.word_cb   = spfy_cli_word_cb;
        if (pev_fp) cb.phrase_cb = spfy_cli_phrase_cb;
        cb.ctx = &wev_ctx;
        cbp = &cb;
    }

    /* SPFY_PITCH_SEMITONES - shift target F0 via unit-selection bias. */
    {
        const char *pe = spfy_env("SPFY_PITCH_SEMITONES");
        if (pe && *pe) {
            float st = (float)atof(pe);
            spfy_synth_set_pitch_semitones(&voice, st);
        }
    }

    spfy_synth_stats_t stats = {0};
    rc = spfy_synth_to_sink(&voice, text, &wav, cbp, &stats);

    if (wev_fp) { fclose(wev_fp); wev_fp = NULL; }
    if (pev_fp) { fclose(pev_fp); pev_fp = NULL; }
    spfy_wav_close(&wav);
    /* Speechify-4 F0-flatten post-process. */
    if (rc == SPFY_OK && out_wav && spfy_env("SPFY_SPFY4_F0_FLATTEN")) {
        float ratio = (float)atof(spfy_env("SPFY_SPFY4_F0_FLATTEN"));
        const char *be = spfy_env("SPFY_SPFY4_F0_FLATTEN_BASE");
        float base = (be && *be) ? (float)atof(be) : 120.0f;
        int frc = spfy_f0_flatten_wav_file(out_wav, base, ratio);
        if (frc != 0 && !trace_stream)
            fprintf(stderr, "warning: F0-flatten post-process rc=%d\n", frc);
    }
    if (rc == SPFY_OK) {
        /* [live-trace] terminal event - lets the viz finalize and fetch the WAV. */
        spfy_trace_eventf("done", "{\"samples\":%u,\"n_phrases\":%u}",
                          stats.samples_emitted, stats.n_phrases);
        /* Keep stdout pure NDJSON in stream mode; the human summary would
         * otherwise land mid-stream and break the SSE relay's JSON parse. */
        if (!trace_stream)
        fprintf(stdout, "wrote %s: %u samples (%.2f s)  "
                        "[%zu units, %zu same-rec pairs, %zu cross-rec, "
                        "%zu skipped, %zu interword pauses, "
                        "wsola_aligned=%llu/%llu, %u phrases]\n",
                out_wav, stats.samples_emitted,
                (double)stats.samples_emitted / (double)voice.vdb.sample_rate,
                stats.total_played, stats.total_paired_same, stats.total_paired_cross,
                stats.total_skipped, stats.total_interword_pauses,
                (unsigned long long)stats.wsola_aligned,
                (unsigned long long)stats.wsola_pushes,
                stats.n_phrases);
    } else {
        fprintf(stderr, "error: %s\n", spfy_strerror(rc));
    }
    if (spfy_env("SPFY_PRSL_STATS") && g_prsl_rung_total) {
        double t = (double)g_prsl_rung_total;
        fprintf(stderr,
                "prsl ladder: %llu preselected slots  exact %llu (%.2f%%)  "
                "one-sided %llu (%.2f%%)  both-sided %llu (%.2f%%)  "
                "empty %llu (%.2f%%)\n",
                (unsigned long long)g_prsl_rung_total,
                (unsigned long long)g_prsl_rung_exact,
                100.0 * (double)g_prsl_rung_exact / t,
                (unsigned long long)g_prsl_rung_1side,
                100.0 * (double)g_prsl_rung_1side / t,
                (unsigned long long)g_prsl_rung_both,
                100.0 * (double)g_prsl_rung_both / t,
                (unsigned long long)g_prsl_rung_empty,
                100.0 * (double)g_prsl_rung_empty / t);
    }
    free(file_text);
    spfy_voice_free(&voice);

    /* Last thing before the exit code, and only on the success path: the
     * user has their WAV, stdout has already been written, and the check
     * gets a 5-second leash that it only ever spends once a week. Output
     * goes to stderr so a caller parsing the "wrote ..." line never sees it,
     * and trace mode skips it entirely to keep stdout pure NDJSON. */
#ifdef SPFY_HAVE_UPDATE_CHECK
    if (update_check && !trace_stream && rc == SPFY_OK) {
        if (spfy_upd_console_visible()) {
            (void)spfy_upd_run(argv[0], 0, 0, stderr);
        } else if (spfy_upd_due_now()) {
            /* No console: this is spfy_sapi64.dll rendering for a 64-bit SAPI
             * client, and the shim is WAITING on us to exit before it can
             * return audio. So hand off and leave -- an inline check would
             * print into a pipe nobody reads, and showing the balloon here
             * would hold the speaking application for the length of the
             * notification. */
            spfy_upd_spawn_helper(NULL);
        }
    }
#else
    (void)update_check;
#endif

    return rc == SPFY_OK ? 0 : 1;
}
#endif
