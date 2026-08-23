/* Start the update helper and walk away.
 *
 * This is the whole of what spfy_sapi.dll is allowed to do. That DLL is an
 * in-process COM server: its Speak() runs on Narrator's and Balabolka's own
 * thread, so anything that blocks -- a DNS lookup, a TLS handshake, a modal
 * dialog -- blocks a screen reader mid-sentence. So the DLL never fetches.
 * It stats one small file and, at most ONCE per process, starts a detached
 * spfy_update.exe that owns every part of the check.
 *
 * The once-per-process latch matters: Balabolka calls Speak per paragraph,
 * and a helper that failed to start (not installed, deleted, blocked by
 * policy) must not be retried on every one of them.
 */

#include "spfy_update.h"
#include "upd_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <windows.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

void spfy_upd_spawn_helper(const char *self_dir)
{
    static int tried = 0;
    char dir[1024];
    char exe[1152];

    if (tried) return;
    tried = 1;

    if (self_dir && *self_dir) {
        spfy_upd_strlcpy(dir, self_dir, sizeof dir);
    } else if (spfy_upd_self_dir(dir, sizeof dir) != 0) {
        return;
    }

#ifdef _WIN32
    if (snprintf(exe, sizeof exe, "%s\\spfy_update.exe", dir) >= (int)sizeof exe)
        return;
    if (spfy_upd_file_stat(exe, NULL, NULL) != 0) return;
    {
        char cmd[1280];
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;

        /* The helper is quiet on the console it does not have, and shows a
         * tray balloon instead. --from-sapi tells it so. */
        if (snprintf(cmd, sizeof cmd, "\"%s\" --quiet --gui --from-sapi", exe)
            >= (int)sizeof cmd)
            return;

        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        memset(&pi, 0, sizeof pi);

        /* DETACHED_PROCESS: no console is created and none is inherited, so
         * nothing of ours can ever write to the host's stdio.
         * CREATE_BREAKAWAY_FROM_JOB is NOT set -- if the host is in a job
         * object that kills children, that is the host's call to make. */
        if (CreateProcessA(exe, cmd, NULL, NULL, FALSE,
                           DETACHED_PROCESS | CREATE_NO_WINDOW,
                           NULL, dir, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
    }
#else
    if (snprintf(exe, sizeof exe, "%s/spfy_update", dir) >= (int)sizeof exe)
        return;
    if (spfy_upd_file_stat(exe, NULL, NULL) != 0) return;
    {
        pid_t pid = fork();
        if (pid == 0) {
            /* Double-fork so the helper is reparented to init and the caller
             * never has to wait() for it. */
            if (fork() == 0) {
                int fd = open("/dev/null", O_RDWR);
                if (fd >= 0) {
                    dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
                    if (fd > 2) close(fd);
                }
                setsid();
                execl(exe, exe, "--quiet", "--from-sapi", (char *)NULL);
            }
            _exit(0);
        } else if (pid > 0) {
            int status;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
                ;
        }
    }
#endif
}
