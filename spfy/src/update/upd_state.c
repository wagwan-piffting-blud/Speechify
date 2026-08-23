/* update_state.json -- the throttle, the off switch and the dismissals.
 *
 * Deliberately tolerant: a missing, truncated or hand-mangled file yields
 * the defaults (enabled, never checked) rather than an error. A corrupt
 * state file must not be able to silently disable update notifications, and
 * it must not be able to make the engine fail either.
 */

#include "spfy_update.h"
#include "upd_internal.h"
#include "mini_json.h"
#include "env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void spfy_upd_strlcpy(char *dst, const char *src, size_t dst_n)
{
    size_t n;
    if (!dst || dst_n == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    n = strlen(src);
    if (n >= dst_n) n = dst_n - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

long long spfy_upd_now(void)
{
    return (long long)time(NULL);
}

static void state_defaults(spfy_upd_state *st)
{
    memset(st, 0, sizeof *st);
    st->enabled       = 1;
    st->interval_days = SPFY_UPD_DEFAULT_DAYS;
}

void spfy_upd_state_load(spfy_upd_state *st)
{
    char path[1024];
    char *buf = NULL;
    long sz;
    FILE *fp;
    mj_node nodes[64];
    mj_doc doc;
    int root;

    state_defaults(st);
    if (spfy_upd_state_path(path, sizeof path, "update_state.json") != 0)
        return;
    fp = fopen(path, "rb");
    if (!fp) return;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return; }
    sz = ftell(fp);
    /* 64 KiB is already absurd for six scalars; anything larger is not our
     * file and is not worth parsing. */
    if (sz <= 0 || sz > 65536) { fclose(fp); return; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return;
    }
    buf[sz] = '\0';
    fclose(fp);

    if (mj_parse(buf, nodes, (int)(sizeof nodes / sizeof nodes[0]), &doc) != 0) {
        free(buf);
        return;
    }
    root = 0;
    st->enabled       = mj_bool(&doc, mj_obj_get(&doc, root, "enabled"), 1);
    st->interval_days = (int)mj_num(&doc, mj_obj_get(&doc, root, "interval_days"),
                                    SPFY_UPD_DEFAULT_DAYS);
    st->last_check    = (long long)mj_num(&doc,
                            mj_obj_get(&doc, root, "last_check"), 0.0);
    st->last_success  = (long long)mj_num(&doc,
                            mj_obj_get(&doc, root, "last_success"), 0.0);
    mj_str(&doc, mj_obj_get(&doc, root, "skip_engine"),
           st->skip_engine, sizeof st->skip_engine);
    mj_str(&doc, mj_obj_get(&doc, root, "skip_voices"),
           st->skip_voices, sizeof st->skip_voices);
    free(buf);
}

/* JSON string escape for the few fields we write: versions and voice ids,
 * i.e. printable ASCII. Anything outside that is dropped rather than
 * escaped -- it cannot have come from us. */
static void write_json_string(FILE *fp, const char *s)
{
    fputc('"', fp);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', fp); fputc((int)c, fp); }
        else if (c >= 0x20 && c < 0x7f) fputc((int)c, fp);
    }
    fputc('"', fp);
}

int spfy_upd_state_save(const spfy_upd_state *st)
{
    char path[1024], tmp[1024];
    FILE *fp;

    if (spfy_upd_state_path(path, sizeof path, "update_state.json") != 0)
        return -1;
    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp)
        return -1;

    fp = fopen(tmp, "wb");
    if (!fp) return -1;
    fprintf(fp, "{\n");
    fprintf(fp, "  \"schema\": 1,\n");
    fprintf(fp, "  \"enabled\": %s,\n", st->enabled ? "true" : "false");
    fprintf(fp, "  \"interval_days\": %d,\n",
            st->interval_days >= 0 ? st->interval_days : SPFY_UPD_DEFAULT_DAYS);
    fprintf(fp, "  \"last_check\": %lld,\n", st->last_check);
    fprintf(fp, "  \"last_success\": %lld,\n", st->last_success);
    fprintf(fp, "  \"skip_engine\": ");
    write_json_string(fp, st->skip_engine);
    fprintf(fp, ",\n");
    fprintf(fp, "  \"skip_voices\": ");
    write_json_string(fp, st->skip_voices);
    fprintf(fp, "\n}\n");
    if (fclose(fp) != 0) return -1;

    /* rename(2) refuses to clobber on Windows, so the old file goes first.
     * The window between the two is the whole risk, and losing this file
     * costs one extra check. */
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    return 0;
}

/* A marker file beside the installed binaries switches the check off for
 * EVERY account on the machine.
 *
 * The per-user state file cannot do that job here: the installer runs
 * elevated, so its %LOCALAPPDATA% is the administrator's, and an opt-out
 * written there would miss the person who actually uses the voice. The
 * installer creates and deletes this file from the "check for updates"
 * checkbox, and uninstalling removes it with the rest of {app}. */
int spfy_upd_machine_opt_out(void)
{
    static int cached = -1;
    char dir[1024], marker[1088];

    if (cached >= 0) return cached;
    cached = 0;
    if (spfy_upd_self_dir(dir, sizeof dir) == 0 &&
        snprintf(marker, sizeof marker, "%s%cno_update_check", dir,
#ifdef _WIN32
                 '\\'
#else
                 '/'
#endif
                 ) < (int)sizeof marker &&
        spfy_upd_file_stat(marker, NULL, NULL) == 0)
        cached = 1;
    return cached;
}

int spfy_upd_due(const spfy_upd_state *st, long long now)
{
    long long interval;
    const char *ev;

    ev = spfy_env("SPFY_NO_UPDATE_CHECK");
    if (ev && *ev && strcmp(ev, "0") != 0) return 0;
    if (spfy_upd_machine_opt_out()) return 0;
    if (!st->enabled) return 0;

    /* >= 0, not > 0: zero is a REQUEST ("check every run"), which is what
     * --interval 0 documents and what testing needs. Only a negative -- i.e.
     * a value nobody set -- falls back to the default. */
    interval = (long long)(st->interval_days >= 0
                           ? st->interval_days : SPFY_UPD_DEFAULT_DAYS);
    ev = spfy_env("SPFY_UPDATE_INTERVAL_DAYS");
    if (ev && *ev) {
        long long v = atoll(ev);
        if (v >= 0) interval = v;
    }
    if (st->last_check <= 0) return 1;
    /* A clock that moved backwards (a VM restore, a timezone repair) would
     * otherwise park last_check in the future and suppress the check for
     * years. Treat any negative delta as due. */
    if (now < st->last_check) return 1;
    return (now - st->last_check) >= interval * 86400ll;
}

int spfy_upd_due_now(void)
{
    static int cached = -1;
    spfy_upd_state st;

    if (cached >= 0) return cached;
    spfy_upd_state_load(&st);
    cached = spfy_upd_due(&st, spfy_upd_now());
    return cached;
}
