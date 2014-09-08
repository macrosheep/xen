/*
 * Copyright (C) 2014      Citrix Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "libxl_osdeps.h"

#include "libxl_internal.h"

/*
 * Infrastructure for converting a legacy migration stream into a libxl v2
 * stream.
 *
 * This is done by fork()ing the python conversion script, which takes in a
 * legacy stream, and puts out a suitably-formatted v2 stream.
 */

static void helper_failed(libxl__egc *egc,
                          libxl__conversion_helper_state *chs, int rc);
static void helper_exited(libxl__egc *egc, libxl__ev_child *ch,
                          pid_t pid, int status);
static void helper_done(libxl__egc *egc,
                        libxl__conversion_helper_state *chs);

void libxl__convert_legacy_stream(libxl__egc *egc,
                                  libxl__conversion_helper_state *chs)
{
    STATE_AO_GC(chs->ao);
    int ret = 0;

    chs->rc = 0;
    libxl__ev_child_init(&chs->child);

    libxl__carefd *child_in = NULL, *child_out = NULL;

    if (chs->legacy_width == 0) {
#ifdef __i386__
        chs->legacy_width = 32;
#else
        chs->legacy_width = 64;
#endif
    }

    libxl__carefd_begin();
    int fds[2];
    if (libxl_pipe(CTX, fds)) {
        ret = ERROR_FAIL;
        libxl__carefd_unlock();
        goto err;
    }
    child_out = libxl__carefd_record(CTX, fds[0]);
    child_in  = libxl__carefd_record(CTX, fds[1]);
    libxl__carefd_unlock();

    pid_t pid = libxl__ev_child_fork(gc, &chs->child, helper_exited);
    if (!pid) {
        char * const args[] =
        {
            getenv("LIBXL_CONVERT_HELPER") ?:
                LIBEXEC_BIN "/convert-legacy-stream.py",
            "--in",     GCSPRINTF("%d", chs->legacy_fd),
            "--out",    GCSPRINTF("%d", fds[1]),
            "--width",  GCSPRINTF("%u", chs->legacy_width),
            "--guest",  chs->hvm ? "hvm" : "pv",
            "--format", "libxl",
            /* "--verbose", */
            NULL,
        };

        libxl_fd_set_cloexec(CTX, chs->legacy_fd, 0);
        libxl_fd_set_cloexec(CTX, libxl__carefd_fd(child_in), 0);

        libxl__exec(gc,
                    -1, -1, -1,
                    args[0], args, NULL);
    }

    libxl__carefd_close(child_in);
    chs->v2_carefd = child_out;

    assert(!ret);
    return;

 err:
    assert(ret);
    helper_failed(egc, chs, ret);
}

void libxl__convert_legacy_stream_abort(libxl__egc *egc,
                                        libxl__conversion_helper_state *chs,
                                        int rc)
{
    helper_failed(egc, chs, rc);
}

static void helper_failed(libxl__egc *egc,
                          libxl__conversion_helper_state *chs, int rc)
{
    STATE_AO_GC(chs->ao);

    if (!chs->rc)
        chs->rc = rc;

    if (!libxl__ev_child_inuse(&chs->child)) {
        helper_done(egc, chs);
        return;
    }

    libxl__kill(gc, chs->child.pid, SIGKILL, "conversion helper");
}

static void helper_exited(libxl__egc *egc, libxl__ev_child *ch,
                          pid_t pid, int status)
{
    libxl__conversion_helper_state *chs = CONTAINER_OF(ch, *chs, child);
    STATE_AO_GC(chs->ao);

    if (status) {
        libxl_report_child_exitstatus(CTX, XTL_ERROR, "conversion helper",
                                      pid, status);
        chs->rc = ERROR_FAIL;
    }
    else
        chs->rc = 0;

    helper_done(egc, chs);
}

static void helper_done(libxl__egc *egc,
                        libxl__conversion_helper_state *chs)
{
    STATE_AO_GC(chs->ao);

    assert(!libxl__ev_child_inuse(&chs->child));

    chs->completion_callback(egc, chs, chs->rc);
}
