/* spfy_update -- the update checker as a standalone program.
 *
 * Run by hand it checks immediately and prints. Run by the SAPI DLL
 * (--from-sapi) it honours the throttle, says nothing on a console it does
 * not have, and shows a tray balloon instead.
 */

#include "spfy_update.h"
#include "upd_internal.h"
#include "env.h"
#include "spfy/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0, FILE *fp)
{
    fprintf(fp,
        "usage: %s [options]\n"
        "\n"
        "Checks %s for a newer engine and for rebuilt versions of the\n"
        "voices installed on this machine. Run with no options it checks\n"
        "immediately; the automatic check (spfy_synth, and the SAPI voice\n"
        "when a program speaks) runs at most once every %d days.\n"
        "\n"
        "  --check           check now, ignoring the throttle (default)\n"
        "  --if-due          check only if the interval has elapsed\n"
        "  --from-sapi       what the SAPI DLL passes: --if-due --quiet --gui\n"
        "  --gui             show a tray balloon as well as printing\n"
        "  --quiet           print nothing\n"
        "  --status          print the stored state and exit\n"
        "  --enable          re-enable automatic checks\n"
        "  --disable         turn automatic checks off for this user\n"
        "  --interval <days> set the automatic interval (0 = every run)\n"
        "  --dismiss         do not report again what is currently available\n"
        "  --url <url>       check this manifest instead of the built-in one\n"
        "  --version         print the engine version and exit\n"
        "\n"
        "SPFY_NO_UPDATE_CHECK=1 disables every automatic check, including\n"
        "the one inside the SAPI voice.\n",
        argv0, spfy_upd_url(), SPFY_UPD_DEFAULT_DAYS);
}

static int print_status(void)
{
    spfy_upd_state st;
    char dir[1024];

    spfy_upd_state_load(&st);
    printf("spfy %s\n", SPFY_VERSION);
    printf("  manifest      %s\n", spfy_upd_url());
    if (spfy_upd_state_dir(dir, sizeof dir) == 0)
        printf("  state         %s\n", dir);
    printf("  automatic     %s%s\n", st.enabled ? "on" : "off",
           spfy_upd_machine_opt_out()
               ? "  (overridden: no_update_check sits beside the binaries)"
               : "");
    printf("  interval      %d day(s)%s\n",
           st.interval_days >= 0 ? st.interval_days : SPFY_UPD_DEFAULT_DAYS,
           st.interval_days == 0 ? "  (every run)" : "");
    printf("  last check    %lld\n", st.last_check);
    printf("  last success  %lld\n", st.last_success);
    if (st.skip_engine[0]) printf("  dismissed     engine %s\n", st.skip_engine);
    if (st.skip_voices[0]) printf("  dismissed     %s\n", st.skip_voices);
    if (spfy_upd_version_is_dev(SPFY_VERSION))
        printf("  note          this is a dev build; engine updates are "
               "never reported\n");
    return 0;
}

/* Record everything currently on offer as "do not tell me again". */
static int dismiss_current(const char *argv0)
{
    spfy_upd_state st;
    spfy_upd_manifest *m;
    spfy_upd_report *rep;
    char *body = NULL;
    size_t body_n = 0;
    int i, rc = 1;

    spfy_upd_state_load(&st);
    if (spfy_upd_fetch(spfy_upd_url(), 20, &body, &body_n) != 0) {
        fprintf(stderr, "spfy_update: cannot reach %s\n", spfy_upd_url());
        return 1;
    }
    m   = (spfy_upd_manifest *)calloc(1, sizeof *m);
    rep = (spfy_upd_report *)calloc(1, sizeof *rep);
    if (!m || !rep) { free(body); free(m); free(rep); return 1; }

    if (spfy_upd_manifest_parse(body, body_n, m) == 0) {
        spfy_upd_compare(m, argv0, NULL, rep);
        if (rep->engine_update)
            spfy_upd_strlcpy(st.skip_engine, rep->remote_version,
                             sizeof st.skip_engine);
        st.skip_voices[0] = '\0';
        for (i = 0; i < rep->n_voices; i++) {
            char one[128];
            size_t have = strlen(st.skip_voices);
            snprintf(one, sizeof one, "%s%s@%s", have ? "," : "",
                     rep->voices[i].id, rep->voices[i].remote_version);
            if (have + strlen(one) + 1 < sizeof st.skip_voices)
                strcat(st.skip_voices, one);
        }
        spfy_upd_state_save(&st);
        printf("dismissed %d item(s)\n", rep->engine_update + rep->n_voices);
        rc = 0;
    }
    free(body); free(m); free(rep);
    return rc;
}

int main(int argc, char **argv)
{
    int force = 1, quiet = 0, gui = 0;
    int i;

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--check") == 0) {
            force = 1;
        } else if (strcmp(a, "--if-due") == 0) {
            force = 0;
        } else if (strcmp(a, "--from-sapi") == 0) {
            force = 0; quiet = 1; gui = 1;
        } else if (strcmp(a, "--gui") == 0) {
            gui = 1;
        } else if (strcmp(a, "--quiet") == 0 || strcmp(a, "-q") == 0) {
            quiet = 1;
        } else if (strcmp(a, "--status") == 0) {
            return print_status();
        } else if (strcmp(a, "--version") == 0) {
            printf("%s\n", SPFY_VERSION);
            return 0;
        } else if (strcmp(a, "--enable") == 0 || strcmp(a, "--disable") == 0) {
            spfy_upd_state st;
            spfy_upd_state_load(&st);
            st.enabled = (a[2] == 'e');
            if (spfy_upd_state_save(&st) != 0) {
                fprintf(stderr, "spfy_update: cannot write the state file\n");
                return 1;
            }
            printf("automatic update checks are %s\n",
                   st.enabled ? "on" : "off");
            return 0;
        } else if (strcmp(a, "--interval") == 0) {
            spfy_upd_state st;
            if (i + 1 >= argc) { usage(argv[0], stderr); return 2; }
            spfy_upd_state_load(&st);
            st.interval_days = atoi(argv[++i]);
            if (st.interval_days < 0) st.interval_days = 0;
            if (spfy_upd_state_save(&st) != 0) {
                fprintf(stderr, "spfy_update: cannot write the state file\n");
                return 1;
            }
            printf("interval set to %d day(s)\n", st.interval_days);
            return 0;
        } else if (strcmp(a, "--dismiss") == 0) {
            return dismiss_current(argv[0]);
        } else if (strcmp(a, "--url") == 0) {
            static char buf[600];
            if (i + 1 >= argc) { usage(argv[0], stderr); return 2; }
            snprintf(buf, sizeof buf, "SPFY_UPDATE_URL=%s", argv[++i]);
            putenv(buf);
            spfy_env_reset();
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage(argv[0], stdout);
            return 0;
        } else {
            fprintf(stderr, "spfy_update: unknown option '%s'\n", a);
            usage(argv[0], stderr);
            return 2;
        }
    }

    {
        int rc = spfy_upd_run(argv[0], force, gui, quiet ? NULL : stdout);
        if (rc > 0) return 0;
        if (rc == 0) {
            if (!quiet) printf("up to date (spfy %s)\n", SPFY_VERSION);
            return 0;
        }
        if (rc == SPFY_UPD_NOT_DUE) {
            if (!quiet)
                printf("no check run: %s. --check forces one.\n",
                       spfy_upd_machine_opt_out()
                           ? "switched off by no_update_check beside the binaries"
                           : "the interval has not elapsed, or checks are off");
            return 0;
        }
        if (!quiet) {
            if (rc == SPFY_UPD_BAD_MANIFEST)
                fprintf(stderr, "spfy_update: %s answered, but not with a "
                                "manifest this build understands\n",
                        spfy_upd_url());
            else
                fprintf(stderr, "spfy_update: cannot reach %s (offline, "
                                "blocked, or not published yet)\n",
                        spfy_upd_url());
        }
        return 1;
    }
}
