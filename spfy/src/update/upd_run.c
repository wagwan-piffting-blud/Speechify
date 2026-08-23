/* Fetch, compare, report, remember. The whole check in one call.
 *
 * Failure is always silent and always cheap. The timestamp advances on a
 * failed fetch as well as a successful one: an offline machine that retried
 * every run would pay the connect timeout on every synth, which is a far
 * worse bug than learning about a release a week late. `last_success` keeps
 * the two apart for anyone debugging it.
 */

#include "spfy_update.h"
#include "upd_internal.h"
#include "env.h"

#include <stdlib.h>
#include <string.h>

int spfy_upd_run(const char *argv0, int force, int use_gui, FILE *out)
{
    spfy_upd_state st;
    spfy_upd_manifest *m = NULL;
    spfy_upd_report *rep = NULL;
    char *body = NULL;
    size_t body_n = 0;
    long long now = spfy_upd_now();
    int timeout_s;
    int rc = -1;
    int n;

    spfy_upd_state_load(&st);
    /* -2, not 0: "I did not look" and "I looked and there is nothing" are
     * different answers, and only the caller knows which one is worth
     * printing. */
    if (!force && !spfy_upd_due(&st, now)) return SPFY_UPD_NOT_DUE;

    /* The detached helper can wait; the inline path runs at the end of a
     * synth the user is watching, so it gets a short leash. */
    timeout_s = use_gui ? 20 : 5;
    {
        const char *ev = spfy_env("SPFY_UPDATE_TIMEOUT");
        if (ev && *ev) {
            int v = atoi(ev);
            if (v > 0 && v <= 300) timeout_s = v;
        }
    }

    st.last_check = now;
    if (spfy_upd_fetch(spfy_upd_url(), timeout_s, &body, &body_n) != 0) {
        spfy_upd_state_save(&st);
        return -1;
    }

    m   = (spfy_upd_manifest *)calloc(1, sizeof *m);
    rep = (spfy_upd_report *)calloc(1, sizeof *rep);
    if (!m || !rep) goto done;

    if (spfy_upd_manifest_parse(body, body_n, m) != 0) {
        rc = SPFY_UPD_BAD_MANIFEST;
        goto done;
    }
    st.last_success = now;

    n = spfy_upd_compare(m, argv0, &st, rep);
    rc = n;
    if (n > 0) {
        if (out) spfy_upd_notify_console(rep, out);
        if (use_gui) spfy_upd_notify_gui(rep);
    }

done:
    spfy_upd_state_save(&st);
    free(body);
    free(m);
    free(rep);
    return rc;
}
