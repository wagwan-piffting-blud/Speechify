/* Online update check for the engine and the installed voices.
 *
 * Split into two libraries on purpose:
 *
 *   spfy_update_trigger  paths + state + "spawn the helper". No network, no
 *                        crypto, no parser. This is what spfy_sapi.dll links,
 *                        because that DLL runs IN-PROCESS inside Narrator and
 *                        Balabolka -- it must never fetch, never block and
 *                        never open a window. All it does is stat a small
 *                        file and, at most once every interval, start a
 *                        detached helper process.
 *
 *   spfy_update_core     fetch + SHA-256 + manifest parse + compare + notify.
 *                        Linked by spfy_synth (a console app that can afford
 *                        an inline check once a week) and by the spfy_update
 *                        executable.
 *
 * The check is throttled by <state dir>/update_state.json and is silent on
 * every failure: no network, no DNS, a 404, a truncated body and a manifest
 * from the future all end the same way -- nothing printed, timestamp NOT
 * advanced on transport failure so a flaky link retries next run.
 *
 * Off switches, in order of precedence:
 *   SPFY_NO_UPDATE_CHECK=1   env, kills it everywhere including the DLL
 *   "enabled": false         in update_state.json (what the installer writes
 *                            when the user declines the check at install)
 *   --no-update-check        spfy_synth only
 */

#ifndef SPFY_UPDATE_H
#define SPFY_UPDATE_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SPFY_UPD_MAX_VOICES   64
#define SPFY_UPD_MAX_FILES     8
#define SPFY_UPD_DEFAULT_DAYS  7

/* ---------------------------------------------------------------- state */

typedef struct {
    int       enabled;          /* 0 disables every check              */
    int       interval_days;    /* <=0 means SPFY_UPD_DEFAULT_DAYS     */
    long long last_check;       /* unix seconds, 0 = never             */
    long long last_success;     /* unix seconds of the last 200        */
    char      skip_engine[32];  /* engine version the user dismissed   */
    char      skip_voices[512]; /* "id@version,id@version" dismissals  */
} spfy_upd_state;

/* <state dir> is %LOCALAPPDATA%\Speechify on Windows and
 * ${XDG_STATE_HOME:-~/.local/state}/spfy elsewhere. Created on demand.
 * Returns 0 on success. */
int  spfy_upd_state_dir(char *buf, size_t buf_n);

/* Missing / unreadable / malformed state all yield the defaults (enabled,
 * never checked), never a failure -- a corrupt state file must not disable
 * the feature silently. */
void spfy_upd_state_load(spfy_upd_state *st);
int  spfy_upd_state_save(const spfy_upd_state *st);

/* Has `interval_days` elapsed since last_check? Also 0 when disabled by env
 * or by state. `now` is unix seconds. */
int  spfy_upd_due(const spfy_upd_state *st, long long now);

/* Cheap enough for the SAPI path: reads the state file once per process and
 * caches the answer. */
int  spfy_upd_due_now(void);

/* Is there a `no_update_check` file beside the installed binaries? That is
 * the MACHINE-WIDE off switch, and it exists because the installer runs
 * elevated: an opt-out written to %LOCALAPPDATA% would land in the
 * administrator's profile, not the profile of whoever uses the voice. */
int  spfy_upd_machine_opt_out(void);

/* ------------------------------------------------------------- manifest */

typedef struct {
    char      name[64];
    char      sha256[65];
    long long bytes;
} spfy_upd_file;

typedef struct {
    char           id[64];
    char           display[96];
    char           lang[16];
    char           version[32];
    char           url[512];
    long long      zip_bytes;
    int            notify;
    int            n_files;
    spfy_upd_file  files[SPFY_UPD_MAX_FILES];
} spfy_upd_voice;

typedef struct {
    int             schema;
    char            engine_version[32];
    char            engine_url[512];
    char            message[256];
    int             engine_notify;
    int             n_voices;
    spfy_upd_voice  voices[SPFY_UPD_MAX_VOICES];
} spfy_upd_manifest;

/* Returns 0 on success, -1 on malformed JSON, -2 on an unsupported schema.
 * Unknown keys are ignored, so the manifest can grow without breaking an
 * installed checker. */
int spfy_upd_manifest_parse(const char *json, size_t n, spfy_upd_manifest *m);

/* --------------------------------------------------------------- report */

enum {
    SPFY_UPD_R_NONE = 0,    /* up to date                              */
    SPFY_UPD_R_SIZE,        /* a file's size differs from the manifest */
    SPFY_UPD_R_HASH,        /* sizes match, SHA-256 differs            */
    SPFY_UPD_R_VERSION,     /* no hashes to compare, version differs   */
    SPFY_UPD_R_MISSING      /* the manifest lists a file we don't have */
};

typedef struct {
    char      id[64];
    char      display[96];
    char      lang[16];
    char      local_version[32];
    char      remote_version[32];
    char      url[512];
    long long zip_bytes;
    int       reason;
} spfy_upd_voice_result;

typedef struct {
    int                   engine_update;
    char                  local_version[32];
    char                  remote_version[32];
    char                  engine_url[512];
    char                  message[256];
    int                   n_voices;          /* voices needing an update */
    spfy_upd_voice_result voices[SPFY_UPD_MAX_VOICES];
} spfy_upd_report;

/* Compare the manifest against what is installed. `argv0` seeds the voice
 * search (see spfy_voice_list); NULL is fine. Dismissals in `st` are honoured
 * -- pass NULL to ignore them (that is what --check-update does).
 *
 * Size first, hash only on a size match: a rebuilt voice essentially always
 * changes size, so the 96 MB SHA-256 is the rare tiebreaker rather than the
 * common path. Returns the number of things worth telling the user about. */
int spfy_upd_compare(const spfy_upd_manifest *m, const char *argv0,
                     const spfy_upd_state *st, spfy_upd_report *rep);

/* ---------------------------------------------------------------- fetch */

/* HTTPS GET into a heap buffer (NUL-terminated, *out_n excludes the NUL).
 * WinHTTP on Windows, a forked curl/wget elsewhere -- no shell, so a URL
 * from the environment cannot inject a command. `timeout_s` covers the whole
 * transfer. Returns 0 on success, negative otherwise. Caller frees. */
int spfy_upd_fetch(const char *url, int timeout_s, char **out, size_t *out_n);

/* The URL actually used: $SPFY_UPDATE_URL when set, else the baked-in one. */
const char *spfy_upd_url(void);

/* --------------------------------------------------------------- notify */

void spfy_upd_notify_console(const spfy_upd_report *rep, FILE *fp);

/* Would a printed line actually be read? 0 on Windows when the process has no
 * console window -- which is how spfy_synth.exe runs when spfy_sapi64.dll
 * spawns it for a 64-bit SAPI client. The caller then hands the job to the
 * detached helper instead of printing into a pipe nobody reads. */
int  spfy_upd_console_visible(void);

/* Windows: a tray balloon (a toast on 10/11) that NEVER takes focus, then a
 * short message loop so clicking it opens the releases page. Chosen over
 * MessageBox because this can fire while a screen reader is speaking.
 * Returns 0 if something was shown. Elsewhere: a no-op returning -1. */
int  spfy_upd_notify_gui(const spfy_upd_report *rep);

/* Open a URL in the user's browser. */
void spfy_upd_open_url(const char *url);

/* ------------------------------------------------------------ top level */

/* Returned by spfy_upd_run when the interval has not elapsed, or checks are
 * switched off. Distinct from 0 ("checked, nothing to say") because only the
 * caller knows whether "I did not look" is worth printing. */
#define SPFY_UPD_NOT_DUE (-2)

/* Fetched something, but it is not a manifest this build can read -- a schema
 * bump, or a captive-portal login page with a 200 on it. Distinct from -1 so
 * "the network is down" and "that URL is not what I expected" are not the
 * same diagnosis. */
#define SPFY_UPD_BAD_MANIFEST (-3)

/* Fetch + compare + print, honouring the throttle unless `force`.
 * `out` may be NULL to suppress console output. Returns the number of things
 * reported (>0), 0 when up to date, SPFY_UPD_NOT_DUE when it did not look,
 * and -1 on a transport or parse failure. */
int spfy_upd_run(const char *argv0, int force, int use_gui, FILE *out);

/* -------------------------------------------------------------- trigger */

/* Start `spfy_update` detached and return immediately. Never waits, never
 * inherits stdio, and does nothing at all when the helper is not installed
 * beside us. This is the ONLY update entry point the SAPI DLL calls. */
void spfy_upd_spawn_helper(const char *self_dir);

/* Directory of the running module: the .exe's own directory, or the DLL's.
 * Returns 0 on success. */
int  spfy_upd_self_dir(char *buf, size_t buf_n);

#ifdef __cplusplus
}
#endif

#endif
