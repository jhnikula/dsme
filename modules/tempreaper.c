/**
   @file tempreaper.c

   DSME module to clean up orphaned temporary files.
   <p>
   Copyright (C) 2011 Nokia Corporation.

   @author Matias Muhonen <ext-matias.muhonen@nokia.com>

   This file is part of Dsme.

   Dsme is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License
   version 2.1 as published by the Free Software Foundation.

   Dsme is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with Dsme.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __cplusplus
#define _GNU_SOURCE
#endif

#include "diskmonitor.h"
#include "dsme/modules.h"
#include "dsme/logging.h"

#include <errno.h>
#include <glib.h>
#include <pwd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define GETPWNAM_BUFLEN 1024
#define MIN_PRIORITY 5
#define RPDIR_PATH DSME_SBIN_PATH"/rpdir"

static pid_t reaper_pid = -1;

static bool drop_privileges(void)
{
    bool success = false;
    struct passwd  pwd;
    struct passwd* result = 0;
    char buf[GETPWNAM_BUFLEN];

    memset(&buf, 0, sizeof buf);

    (void)getpwnam_r("user", &pwd, buf, GETPWNAM_BUFLEN, &result);
    if (!result) {
        (void)getpwnam_r("nobody", &pwd, buf, GETPWNAM_BUFLEN, &result);
    }
    if (!result) {
        goto out;
    }

    if (setgid(pwd.pw_gid) != 0) {
        goto out;
    }
    if (setuid(pwd.pw_uid) != 0) {
        goto out;
    }

    success = true;

out:
    return success;
}

static pid_t reaper_process_new(void)
{
    /* The tempdirs we will cleanup are given as an argument.
     */
    char* const argv[] = {(char*)"rpdir",
                          (char*)"/var/tmp",
                          (char*)0};
    pid_t pid = fork();

    if (pid == 0) {
        /* Child; set a reasonably low priority, DSME runs with the priority -1
           so we don't want to use the inherited priority */
        if (setpriority(PRIO_PROCESS, 0, MIN_PRIORITY) != 0) {
            _exit(EXIT_FAILURE);
        }

        if (!drop_privileges()) {
            _exit(EXIT_FAILURE);
        }

        execv(RPDIR_PATH, argv);
        _exit(EXIT_FAILURE);
    } else if (pid == -1) {
        /* error */
        dsme_log(LOG_CRIT, "fork() failed: %s", strerror(errno));
    } else {
        /* parent */
    }
    return pid;
}

static void temp_reaper_finished(GPid pid, gint status, gpointer unused)
{
    reaper_pid = -1;

    if (WEXITSTATUS(status) != 0) {
        dsme_log(LOG_WARNING, "tempreaper: reaper process failed (PID %i).", pid);
        return;
    }

    dsme_log(LOG_DEBUG, "tempreaper: reaper process finished (PID %i).", pid);
}

static bool disk_space_running_out(const DSM_MSGTYPE_DISK_SPACE* msg)
{
    const char *mount_path = DSMEMSG_EXTRA(msg);

    /* TODO: we should actually check the mount entries to figure out
       on which mount(s) temp_dirs are mounted on. We now assume that all
       temp_dirs are mounted on the root partition. */
    return (strcmp(mount_path, "/") == 0);
}

DSME_HANDLER(DSM_MSGTYPE_DISK_SPACE, conn, msg)
{
    if (reaper_pid != -1) {
        dsme_log(LOG_DEBUG, "tempreaper: reaper process already running (PID %i). Return.",
                 reaper_pid);
        return;
    }

    if (disk_space_running_out(msg)) {
        reaper_pid = reaper_process_new();

        if (reaper_pid != -1) {
            g_child_watch_add(reaper_pid, temp_reaper_finished, temp_reaper_finished);
        }
    }
}

module_fn_info_t message_handlers[] = {
    DSME_HANDLER_BINDING(DSM_MSGTYPE_DISK_SPACE),
    { 0 }
};

void module_init(module_t* module)
{
    dsme_log(LOG_DEBUG, "tempreaper.so loaded");
}

void module_fini(void)
{
    if (reaper_pid != -1) {
        dsme_log(LOG_INFO, "killing temp reaper with pid %i", reaper_pid);
        kill(reaper_pid, SIGKILL);
    }

    dsme_log(LOG_DEBUG, "tempreaper.so unloaded");
}